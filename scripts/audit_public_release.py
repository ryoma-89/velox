"""Velox リポジトリ監査ツール（開発版 dev / 公開版 public 共通・CI 対応）。

検査項目:
  1. 機密情報検査（GitHub トークン・AWS/Google/OpenAI API キー・秘密鍵・ハードコードパスワード）
  2. 個人情報・ローカル絶対パス検査（public モードでは個人メール・C:\\Users\\...・E:\\development 等を厳格禁止）
  3. 禁止ファイル・拡張子・大容量ファイル検査（APK/ディスクイメージ/署名鍵/バイナリ/ダンプ等）
  4. パッチ整合性検査（patches/**/*.patch の unified diff ヘッダおよび非空ハンク確認）
  5. 設定プロファイル検証（profiles/*.json の velox.core.config.validate 通過）
  6. ライセンス整合性検査（public モードで LICENSE / THIRD_PARTY_NOTICES.md / LICENSES/* の存在および上流ライセンス明記）
  7. Markdown ローカルリンク整合性検査
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
from typing import Any

PROHIBITED_EXTS = {
    ".apk", ".aab", ".qcow2", ".img", ".raw", ".vmdk", ".vhdx",
    ".dmp", ".mdmp", ".hprof",
    ".jks", ".keystore", ".p12", ".pfx", ".pem", ".key",
    ".exe", ".dll", ".so", ".dylib", ".pdb", ".obj", ".o", ".a", ".lib", ".res",
    ".zip", ".7z", ".tar", ".gz", ".xz", ".rar", ".bin", ".spv",
}

PROHIBITED_NAMES = {
    "adbkey", "adbkey.pub", ".env", ".env.local", ".env.production",
}

SKIP_DIRS = {
    ".git", ".velox", "third_party", "__pycache__", ".pytest_cache", ".mypy_cache",
    ".ruff_cache", ".venv", "venv", "build", "dist", "out",
}

TEXT_EXTS = {
    ".py", ".c", ".h", ".cpp", ".hpp", ".java", ".xml", ".json", ".jsonl",
    ".md", ".txt", ".ps1", ".cmd", ".sh", ".patch", ".yml", ".yaml",
    ".ini", ".cfg", ".rc", ".comp", ".vert", ".frag", ".rgen", ".rchit", ".rmiss",
    ".manifest", ".options", "",
}

SECRET_PATTERNS: list[tuple[str, re.Pattern[bytes]]] = [
    ("github_token", re.compile(rb"(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,}")),
    ("aws_access_key", re.compile(rb"AKIA[0-9A-Z]{16}")),
    ("google_api_key", re.compile(rb"AIza[0-9A-Za-z\-_]{35}")),
    ("private_key_pem", re.compile(rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH )?PRIVATE KEY-----")),
    ("openai_api_key", re.compile(rb"sk-[A-Za-z0-9]{24,}")),
]

PUBLIC_PII_PATTERNS: list[tuple[str, re.Pattern[bytes]]] = [
    ("local_windows_user_path", re.compile(rb"C:[/\\]Users[/\\]Ryoma", re.IGNORECASE)),
    ("personal_gmail", re.compile(rb"ryoma2004ou@gmail\.com", re.IGNORECASE)),
    ("local_dev_drive_path", re.compile(rb"E:[/\\]development[/\\](?:velox|\.vx)", re.IGNORECASE)),
    ("local_app_drive_path", re.compile(rb"E:[/\\]Applications[/\\]Velox", re.IGNORECASE)),
]

LINK_RE = re.compile(r"\[[^\]]+\]\(([^)]+)\)")


def iter_repo_files(root: Path) -> list[Path]:
    out: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        dp = Path(dirpath)
        for fn in filenames:
            out.append(dp / fn)
    return sorted(out)


def audit_repository(root: Path, mode: str = "public", max_file_mb: float = 5.0) -> dict[str, Any]:
    root = root.resolve()
    violations: list[dict[str, str]] = []
    warnings: list[dict[str, str]] = []
    files = iter_repo_files(root)
    max_bytes = int(max_file_mb * 1024 * 1024)

    total_bytes = 0
    patch_count = 0
    profile_count = 0
    md_links_checked = 0

    for fp in files:
        rel = fp.relative_to(root).as_posix()
        sz = fp.stat().st_size
        total_bytes += sz
        ext = fp.suffix.lower()
        name_low = fp.name.lower()

        # 1. Prohibited extensions / names / size
        if ext in PROHIBITED_EXTS or name_low in PROHIBITED_NAMES:
            violations.append({"check": "prohibited_file", "path": rel, "detail": f"禁止ファイル種別 ({ext or name_low})"})
            continue
        if sz > max_bytes:
            violations.append({"check": "large_file", "path": rel, "detail": f"サイズ超過: {sz / (1024 * 1024):.2f} MiB > {max_file_mb} MiB"})

        # 2. Text content scans (secrets, public PII)
        if ext in TEXT_EXTS and sz <= max_bytes:
            # Skip scanning the auditor itself for regex literals
            is_self = (rel == "scripts/audit_public_release.py")
            data = fp.read_bytes()
            if not is_self:
                for sname, srx in SECRET_PATTERNS:
                    if srx.search(data):
                        violations.append({"check": "secret", "path": rel, "detail": f"機密パターン検出: {sname}"})
                if mode == "public":
                    for pname, prx in PUBLIC_PII_PATTERNS:
                        if prx.search(data):
                            violations.append({"check": "pii_or_local_path", "path": rel, "detail": f"個人情報/ローカルパス検出: {pname}"})

            # 3. Patch syntax integrity
            if rel.startswith("patches/") and ext == ".patch":
                patch_count += 1
                txt = data.decode("utf-8", errors="replace")
                has_diff = ("diff --git " in txt) or ("--- " in txt and "+++ " in txt)
                has_hunk = ("@@ " in txt) or ("new file mode" in txt) or ("deleted file mode" in txt)
                if not (has_diff and has_hunk):
                    violations.append({"check": "patch_integrity", "path": rel, "detail": "有効な unified diff ヘッダ/ハンクがありません"})

            # 4. Markdown relative link integrity (public mode checks all md; dev mode checks README/WORKFLOW/AGENTS)
            if ext == ".md":
                check_md = (mode == "public") or (rel in ("README.md", "AGENTS.md", "CLAUDE.md", "docs/WORKFLOW_GOVERNANCE.md"))
                if check_md:
                    txt = data.decode("utf-8", errors="replace")
                    # Strip fenced code blocks before extracting markdown links
                    txt_nocode = re.sub(r"```.*?```", "", txt, flags=re.DOTALL)
                    for m in LINK_RE.finditer(txt_nocode):
                        href = m.group(1).strip().split()[0]
                        if not href or href.startswith(("http://", "https://", "mailto:", "#", "file://")):
                            continue
                        target_rel = href.split("#", 1)[0].split("?", 1)[0]
                        if not target_rel:
                            continue
                        md_links_checked += 1
                        resolved = (fp.parent / target_rel).resolve()
                        if not resolved.exists():
                            violations.append({"check": "broken_link", "path": rel, "detail": f"リンク先が存在しません: {href}"})

    # 5. Profile config validation
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    try:
        from velox.core import config as cfgmod
        prof_dir = root / "profiles"
        if prof_dir.is_dir():
            for pf in sorted(prof_dir.glob("*.json")):
                profile_count += 1
                try:
                    raw = json.loads(pf.read_text(encoding="utf-8"))
                    merged = cfgmod.merge(cfgmod.DEFAULT, raw)
                    cfgmod.validate(merged)
                except Exception as exc:
                    violations.append({"check": "profile_config", "path": pf.relative_to(root).as_posix(), "detail": str(exc)})
    except Exception as exc:
        warnings.append({"check": "profile_config", "path": "velox/core/config.py", "detail": f"import skipped: {exc}"})

    # 6. Public license integrity check
    if mode == "public":
        required_files = [
            "LICENSE",
            "NOTICE",
            "THIRD_PARTY_NOTICES.md",
            "LICENSES/Apache-2.0.txt",
            "LICENSES/GPL-2.0-only.txt",
            "LICENSES/MIT.txt",
            "LICENSES/BSD-3-Clause.txt",
            "README.md",
            "README.ja.md",
            "CONTRIBUTING.md",
            "SECURITY.md",
            "CHANGELOG.md",
            "ROADMAP.md",
        ]
        for rf in required_files:
            if not (root / rf).is_file():
                violations.append({"check": "license_or_doc_missing", "path": rf, "detail": "公開必須ファイルが不足しています"})
        tpn = root / "THIRD_PARTY_NOTICES.md"
        if tpn.is_file():
            tpn_txt = tpn.read_text(encoding="utf-8", errors="replace")
            for must in ("GPL-2.0", "Apache", "MIT", "BSD-3-Clause", "QEMU", "gfxstream", "Mesa"):
                if must not in tpn_txt:
                    violations.append({"check": "license_notice", "path": "THIRD_PARTY_NOTICES.md", "detail": f"必須ライセンス記載欠落: {must}"})

    return {
        "root": str(root),
        "mode": mode,
        "file_count": len(files),
        "total_bytes": total_bytes,
        "total_mb": round(total_bytes / (1024 * 1024), 2),
        "patch_count": patch_count,
        "profile_count": profile_count,
        "md_links_checked": md_links_checked,
        "violation_count": len(violations),
        "violations": violations,
        "warnings": warnings,
        "passed": len(violations) == 0,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Velox リポジトリ監査ツール")
    ap.add_argument("--root", type=Path, default=Path("."))
    ap.add_argument("--mode", choices=("dev", "public"), default="public")
    ap.add_argument("--max-file-mb", type=float, default=5.0)
    ap.add_argument("--json", action="store_true", dest="as_json")
    args = ap.parse_args()

    res = audit_repository(args.root, mode=args.mode, max_file_mb=args.max_file_mb)
    if args.as_json:
        print(json.dumps(res, ensure_ascii=False, indent=2))
    else:
        print(
            f"[AUDIT {res['mode'].upper()}] files={res['file_count']} size={res['total_mb']} MiB "
            f"patches={res['patch_count']} profiles={res['profile_count']} "
            f"links={res['md_links_checked']} violations={res['violation_count']}"
        )
        for v in res["violations"][:50]:
            print(f"  FAIL [{v['check']}] {v['path']}: {v['detail']}")
        for w in res["warnings"]:
            print(f"  WARN [{w['check']}] {w['path']}: {w['detail']}")
    return 0 if res["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
