"""生成器の変更前後のホスト生成物の差分だけを、ホストの既存ファイルに当てる（KI-20）。

ホストの生成物はこの AOSP ツリーの生成器より新しい版で作られているため、全体を再生成すると
機能が消える。そこで `gfxstream_codegen --keep` で作った変更前（before）と変更後（after）の
host/ を比べ、その差分だけをホストのファイルに当てる。

- ホストのファイルには CRLF と LF が混在するので、LF にそろえて当ててから元の改行に戻す。
- オペコード名の表（api_opcode_to_string）と拡張一覧（supported_extensions）は、ホスト側で
  並びが違うため差分が当たらないことがある。その場合は追加分だけを取り出して入れる。
- それ以外のファイルで当てられない差分が残ったときは、何も書き込まずに終了する。

例:
  python -m scripts.guest.gfxstream_codegen --keep %TEMP%\\cg-before   （生成器を変える前）
  python -m scripts.guest.gfxstream_codegen --keep %TEMP%\\cg-after    （変えた後）
  python -m scripts.guest.apply_host_codegen_diff %TEMP%\\cg-before %TEMP%\\cg-after
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "third_party/aosp/emu-main-win/hardware/google/gfxstream/host/vulkan"
OPCODE_FILE = "cereal/common/goldfish_vk_marshaling.cpp"
EXT_FILE = "cereal/common/goldfish_vk_supported_extensions.cpp"


def lf(data: bytes) -> bytes:
    return data.replace(b"\r\n", b"\n")


def find_patch() -> str:
    exe = shutil.which("patch")
    if exe:
        return exe
    git_usr = Path(r"C:\Program Files\Git\usr\bin\patch.exe")
    if git_usr.exists():
        return str(git_usr)
    raise SystemExit("patch コマンドが見つからない（Git for Windows の usr/bin を PATH に入れる）")


def insert_missing_opcode_names(text: str, file_diff: str) -> str:
    """オペコード名の表に、まだ無い `case OP_xxx: { return "OP_xxx"; }` を default の前に足す。"""
    start = text.index("const char* api_opcode_to_string(const uint32_t opcode) {")
    default = text.index("        default:", start)
    body = text[start:default]
    lines = [l[1:] for l in file_diff.splitlines() if l.startswith("+") and not l.startswith("+++")]
    added, guard, i = [], None, 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("#ifdef "):
            guard = line
        m = re.match(r"\s+case (OP_\w+): \{$", line)
        if m and i + 2 < len(lines) and f'return "{m.group(1)}";' in lines[i + 1]:
            if f"case {m.group(1)}:" not in body:
                added += [guard or "#if 1", line, lines[i + 1], lines[i + 2], "#endif"]
            i += 3
            continue
        i += 1
    return text[:default] + ("\n".join(added) + "\n" if added else "") + text[default:]


def insert_extension_names(text: str, file_diff: str) -> str:
    """デバイス拡張の一覧（アルファベット順）に、まだ無い拡張名を順番どおりに足す。

    生成器の差分はデバイス拡張だけを追加する前提（インスタンス拡張の追加は手で確認する）。"""
    for name in re.findall(r'^\+\s*"(VK_[A-Za-z0-9_]+)",', file_diff, re.M):
        if f'"{name}",' in text:
            continue
        lines = text.split("\n")
        begin = next(i for i, l in enumerate(lines) if "GetDeviceExtensionsSupportedByCodegen()" in l)
        entries = [(i, m.group(1)) for i, l in enumerate(lines)
                   if i > begin and (m := re.match(r'\s*"(VK_[A-Za-z0-9_]+)",$', l))]
        end = next((j for j, (i, _) in enumerate(entries) if j and i != entries[j - 1][0] + 1), len(entries))
        entries = entries[:end]
        pos = next((i for i, n in entries if n > name), entries[-1][0] + 1)
        indent = re.match(r"\s*", lines[entries[0][0]]).group(0)
        lines.insert(pos, f'{indent}"{name}",')
        text = "\n".join(lines)
    return text


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("before", type=Path)
    ap.add_argument("after", type=Path)
    ap.add_argument("--host", type=Path, default=HOST)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    before, after = a.before / "host", a.after / "host"
    patch_exe = find_patch()
    diff_exe = str(Path(patch_exe).with_name("diff.exe" if patch_exe.endswith(".exe") else "diff"))
    with tempfile.TemporaryDirectory(prefix="vx-hostdiff-") as tmp:
        work = Path(tmp) / "work"
        diff_text: list[str] = []
        file_diffs: dict[str, str] = {}
        for f in sorted(after.rglob("*")):
            if not f.is_file() or f.name == ".clang-format":
                continue
            rel = f.relative_to(after).as_posix()
            old = before / rel
            old_lines = lf(old.read_bytes()).decode("utf-8").splitlines(keepends=True) if old.exists() else []
            new_lines = lf(f.read_bytes()).decode("utf-8").splitlines(keepends=True)
            if old_lines == new_lines:
                continue
            # GNU diff の hunk 分割は patch と相性がよい（difflib の分割では当たらない hunk があった）
            na, nb = Path(tmp) / "a" / rel, Path(tmp) / "b" / rel
            for path, lines in ((na, old_lines), (nb, new_lines)):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("".join(lines), encoding="utf-8", newline="\n")
            diff = subprocess.run(
                [diff_exe, "-u", "--label", f"a/{rel}", "--label", f"b/{rel}", str(na), str(nb)],
                capture_output=True, text=True, encoding="utf-8",
            )
            if diff.returncode != 1 or not diff.stdout:
                raise SystemExit(f"差分を生成できません（{rel}、終了コード {diff.returncode}）: {diff.stderr}")
            d = diff.stdout
            file_diffs[rel] = d
            diff_text.append(d)
            dst = work / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            target = a.host / rel
            if target.exists():
                dst.write_bytes(lf(target.read_bytes()))
        if not file_diffs:
            print("差分なし")
            return 0
        diff_path = Path(tmp) / "host.diff"
        diff_path.write_text("".join(diff_text), encoding="utf-8", newline="\n")
        p = subprocess.run([patch_exe, "-p1", "--no-backup-if-mismatch", "-r", "-", "-i", str(diff_path)],
                           cwd=work, capture_output=True, text=True)
        failed, current = set(), None
        for line in p.stdout.splitlines():
            m = re.match(r"patching file (\S+)", line)
            if m:
                current = m.group(1)
            elif "FAILED" in line and current:
                failed.add(current)
        if p.returncode not in (0, 1) or (p.returncode == 1 and not failed):
            raise SystemExit(f"patch を実行できません（終了コード {p.returncode}）: {p.stderr or p.stdout}")
        unresolved = []
        for rel in sorted(failed):
            text = (work / rel).read_text(encoding="utf-8")
            if rel == OPCODE_FILE:
                text = insert_missing_opcode_names(text, file_diffs[rel])
            elif rel == EXT_FILE:
                text = insert_extension_names(text, file_diffs[rel])
            else:
                unresolved.append(rel)
                continue
            (work / rel).write_text(text, encoding="utf-8", newline="\n")
            print("  一覧へ追加分を挿入:", rel)
        if unresolved:
            print(p.stdout[-4000:])
            raise SystemExit(f"差分を当てられなかった: {unresolved}")
        print(len(file_diffs), "ファイルに差分を当てた")
        if a.dry_run:
            return 0
        for rel in file_diffs:
            src = (work / rel).read_bytes()
            target = a.host / rel
            crlf = target.exists() and b"\r\n" in target.read_bytes()[:4096]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(src.replace(b"\n", b"\r\n") if crlf else src)
            print("  更新:", rel)
    return 0


if __name__ == "__main__":
    sys.exit(main())
