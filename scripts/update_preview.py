"""Velox 実用環境アップデーター＆ロールバックツール（明示実行専用）。

原則:
- 実用版（例: <VELOX_INSTALL_DIR>）が停止していることを確認してから実行する。
- 更新前に配布パッケージ内全ファイルの SHA-256 を検証する。
- 新ランタイムは runtime\\<new_version>\\ へ別ディレクトリ配置する。
- 更新前に旧 app / Velox.exe / manifest.json / active runtime 名を backups\\ へ退避する。
- 既存の avd\\、userdata\\、config\\、profiles\\ は一切上書き・削除しない。
- 更新後に自動検証（Python import・emulator -version・SHA-256）を実行し、
  失敗時は自動でロールバックする。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def is_velox_running(target: Path) -> bool:
    # 1. config/runtime.json の確認
    rt = target / "config" / "runtime.json"
    if rt.is_file():
        return True
    # 2. QEMU プロセス確認
    if os.name == "nt":
        r = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq qemu-system-x86_64.exe", "/NH"],
            capture_output=True, text=True, timeout=15,
        )
        if "qemu-system-x86_64.exe" in r.stdout.lower():
            return True
        r2 = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq qemu-system-x86_64-headless.exe", "/NH"],
            capture_output=True, text=True, timeout=15,
        )
        if "qemu-system-x86_64-headless.exe" in r2.stdout.lower():
            return True
    return False


def verify_package_sha256(pkg_dir: Path) -> dict:
    man_path = pkg_dir / "release-manifest.json"
    if not man_path.is_file():
        raise RuntimeError(f"release-manifest.json がありません: {man_path}")
    rel = json.loads(man_path.read_text(encoding="utf-8"))
    files = rel.get("files_sha256") or {}
    if not files:
        raise RuntimeError("release-manifest.json のファイル一覧が空です")
    for rel_path, expected in files.items():
        fp = pkg_dir / rel_path
        if not fp.is_file():
            raise RuntimeError(f"配布ファイル欠損: {rel_path}")
        actual = sha256_file(fp)
        if actual != expected:
            raise RuntimeError(f"SHA-256 不一致: {rel_path} (expected {expected[:12]}, got {actual[:12]})")
    return rel


def switch_runtime_current(runtime_root: Path, version_name: str) -> None:
    cur = runtime_root / "current"
    dst = runtime_root / version_name
    if not dst.is_dir():
        raise RuntimeError(f"ランタイムディレクトリが存在しません: {dst}")
    if cur.exists() or cur.is_symlink():
        if os.name == "nt":
            subprocess.run(["cmd", "/c", "rmdir", str(cur)], check=False, capture_output=True)
            if cur.exists():
                shutil.rmtree(cur)
        else:
            if cur.is_symlink():
                cur.unlink()
            else:
                shutil.rmtree(cur)
    if os.name == "nt":
        r = subprocess.run(["cmd", "/c", "mklink", "/J", str(cur), str(dst)], capture_output=True, text=True)
        if r.returncode != 0:
            shutil.copytree(dst, cur)
    else:
        cur.symlink_to(dst, target_is_directory=True)


def verify_installation(target: Path) -> None:
    man_path = target / "manifest.json"
    if not man_path.is_file():
        raise RuntimeError("manifest.json が存在しません")
    man = json.loads(man_path.read_text(encoding="utf-8"))
    ver = man.get("active_runtime")
    dll = target / "runtime" / "current" / "lib64" / "libgfxstream_backend.dll"
    exp_dll = (man.get("critical_sha256") or {}).get("libgfxstream_backend.dll")
    if exp_dll and (not dll.is_file() or sha256_file(dll) != exp_dll):
        raise RuntimeError("libgfxstream_backend.dll の SHA-256 検証に失敗しました")
    qemu_gui = target / "runtime" / "current" / "qemu" / "windows-x86_64" / "qemu-system-x86_64.exe"
    exp_qemu = (man.get("critical_sha256") or {}).get("qemu-system-x86_64.exe")
    if exp_qemu and (not qemu_gui.is_file() or sha256_file(qemu_gui) != exp_qemu):
        raise RuntimeError("qemu-system-x86_64.exe の SHA-256 検証に失敗しました")

    py_exe = target / "tools" / "python" / "python.exe"
    if not py_exe.is_file():
        raise RuntimeError("同梱 python.exe が見つかりません")
    env = dict(os.environ)
    env["VELOX_HOME"] = str(target)
    env["PYTHONHOME"] = str(target / "tools" / "python")
    env["PYTHONPATH"] = str(target / "app")
    env["PYTHONNOUSERSITE"] = "1"
    r = subprocess.run(
        [str(py_exe), "-s", "-c",
         "from velox.core import paths, devices; from velox.gui import app; "
         "assert paths.STANDALONE; print('VERIFY_OK', paths.ROOT)"],
        env=env, capture_output=True, text=True, timeout=30,
    )
    if r.returncode != 0 or "VERIFY_OK" not in r.stdout:
        raise RuntimeError(f"GUI/バックエンド自己診断失敗: {r.stderr.strip() or r.stdout.strip()}")

    emu_exe = target / "runtime" / "current" / "emulator.exe"
    r2 = subprocess.run([str(emu_exe), "-version"], capture_output=True, text=True, timeout=20)
    if r2.returncode != 0:
        raise RuntimeError(f"emulator.exe -version 失敗: {r2.stderr.strip()}")
    print(f"[検証 OK] version={ver}, standalone={target}")


def create_pre_update_backup(target: Path) -> Path:
    man = {}
    if (target / "manifest.json").is_file():
        man = json.loads((target / "manifest.json").read_text(encoding="utf-8"))
    old_ver = man.get("version", "unknown")
    ts = time.strftime("%Y%m%d-%H%M%S")
    bdir = target / "backups" / f"pre-update-{old_ver}-{ts}"
    bdir.mkdir(parents=True, exist_ok=True)
    if (target / "app").is_dir():
        shutil.copytree(target / "app", bdir / "app")
    for fname in ("Velox.exe", "velox.ico", "manifest.json", "README.md"):
        f = target / fname
        if f.is_file():
            shutil.copy2(f, bdir / fname)
    meta = {
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "previous_version": old_ver,
        "previous_runtime": man.get("active_runtime") or old_ver,
    }
    (bdir / "update-backup.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    return bdir


def restore_from_backup(target: Path, backup_dir: Path) -> None:
    meta_path = backup_dir / "update-backup.json"
    if not meta_path.is_file():
        raise RuntimeError(f"update-backup.json がありません: {meta_path}")
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    if (backup_dir / "app").is_dir():
        if (target / "app").exists():
            shutil.rmtree(target / "app")
        shutil.copytree(backup_dir / "app", target / "app")
    for fname in ("Velox.exe", "velox.ico", "manifest.json", "README.md"):
        f = backup_dir / fname
        if f.is_file():
            shutil.copy2(f, target / fname)
    prev_rt = meta.get("previous_runtime")
    if prev_rt and (target / "runtime" / prev_rt).is_dir():
        switch_runtime_current(target / "runtime", prev_rt)
    print(f"[ロールバック完了] previous_version={meta.get('previous_version')}, runtime={prev_rt}")


def apply_update(target: Path, pkg_dir: Path) -> None:
    if is_velox_running(target):
        raise RuntimeError("Velox が実行中です。先にすべての Android 端末を停止してからアップデートを実行してください。")

    print("[1/5] 配布パッケージの SHA-256 を検証中...")
    rel = verify_package_sha256(pkg_dir)
    new_ver = rel["version"]
    print(f"      検証通過 ({rel['file_count']} files, version={new_ver})")

    print("[2/5] 更新前バックアップを作成中（AVD・userdata・config は変更しません）...")
    bdir = create_pre_update_backup(target)
    print(f"      バックアップ先: {bdir}")

    try:
        print(f"[3/5] 新ランタイムを別ディレクトリ runtime\\{new_ver} へ配置中...")
        src_rt = pkg_dir / "runtime" / new_ver
        dst_rt = target / "runtime" / new_ver
        if src_rt.is_dir():
            if dst_rt.exists():
                shutil.rmtree(dst_rt)
            shutil.copytree(src_rt, dst_rt)
            switch_runtime_current(target / "runtime", new_ver)

        print("[4/5] アプリ本体・ランチャーを更新中（AVD/userdata/config/profiles は保護）...")
        if (pkg_dir / "app").is_dir():
            if (target / "app").exists():
                shutil.rmtree(target / "app")
            shutil.copytree(pkg_dir / "app", target / "app")
        if (pkg_dir / "apps").is_dir():
            shutil.copytree(pkg_dir / "apps", target / "apps", dirs_exist_ok=True)
        (target / "shared").mkdir(parents=True, exist_ok=True)
        for fname in ("Velox.exe", "velox.ico", "manifest.json", "README.md"):
            f = pkg_dir / fname
            if f.is_file():
                shutil.copy2(f, target / fname)
        # tools/platform-tools 更新（存在時のみ）
        if (pkg_dir / "tools" / "platform-tools").is_dir():
            shutil.copytree(pkg_dir / "tools" / "platform-tools", target / "tools" / "platform-tools", dirs_exist_ok=True)

        print("[5/5] 更新後の動作確認を実行中...")
        verify_installation(target)

        (target / "updates").mkdir(parents=True, exist_ok=True)
        shutil.copy2(pkg_dir / "release-manifest.json", target / "updates" / f"applied-{new_ver}-{time.strftime('%Y%m%d-%H%M%S')}.json")
        print(f"=== アップデート成功: {new_ver} ===")
    except Exception as exc:
        print(f"[エラー] 更新検証に失敗したため自動ロールバックを実行します: {exc}")
        restore_from_backup(target, bdir)
        raise


def main() -> int:
    ap = argparse.ArgumentParser(description="Velox 実用環境アップデーター＆ロールバック")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_app = sub.add_parser("apply", help="配布パッケージを適用")
    p_app.add_argument("--target", type=Path, required=True)
    p_app.add_argument("--package", type=Path, required=True)

    p_rb = sub.add_parser("rollback", help="直前バックアップへロールバック")
    p_rb.add_argument("--target", type=Path, required=True)
    p_rb.add_argument("--backup", type=Path, default=None)

    p_ver = sub.add_parser("verify", help="実用環境の整合性を検証")
    p_ver.add_argument("--target", type=Path, required=True)

    args = ap.parse_args()
    target = args.target.resolve()

    if args.cmd == "apply":
        apply_update(target, args.package.resolve())
    elif args.cmd == "rollback":
        if is_velox_running(target):
            raise RuntimeError("Velox が実行中です。先に停止してください。")
        bdir = args.backup
        if not bdir:
            cands = sorted((target / "backups").glob("pre-update-*"), reverse=True)
            if not cands:
                raise RuntimeError("ロールバック可能なバックアップが見つかりません。")
            bdir = cands[0]
        restore_from_backup(target, bdir.resolve())
        verify_installation(target)
    elif args.cmd == "verify":
        verify_installation(target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
