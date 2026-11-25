"""Windows Git がテキストとして展開した AOSP ビルド用リンクを復旧する。"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path

from velox.core import paths
from velox.core.util import setup_stdio

PROJECTS = ("external/qemu", "prebuilts/clang/host/windows-x86")


def git(root: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *args])


def tracked_links(project: Path) -> dict[Path, Path]:
    links = {}
    for entry in git(project, "ls-tree", "-rz", "HEAD").split(b"\0"):
        if not entry.startswith(b"120000 blob "):
            continue
        metadata, filename = entry.split(b"\t", 1)
        link = git(project, "cat-file", "blob", metadata.split()[-1].decode())
        alias = project / os.fsdecode(filename)
        if (alias.is_symlink() or not alias.is_file() or alias.stat().st_size != len(link)
                or alias.read_bytes() != link):
            continue
        links[alias] = (alias.parent / os.fsdecode(link)).resolve()
    return links


def materialize(root: Path, projects: tuple[str, ...] = PROJECTS) -> None:
    links = {}
    for project in projects:
        links.update(tracked_links(root / project))
    for alias, target in links.items():
        visited = {alias}
        while target in links and target not in visited:
            visited.add(target)
            target = links[target]
        if target in visited or not target.is_relative_to(root) or not alias.resolve().is_relative_to(root):
            raise ValueError(f"AOSP ツリー内の通常ファイルに解決できません {alias}")
        if not target.is_file():
            print(f"対象ファイル未取得、またはディレクトリへのリンク {alias.relative_to(root)}")
            continue
        shutil.copyfile(target, alias)
        print(f"{alias.relative_to(root)} <= {target.relative_to(root)}")


def main() -> None:
    setup_stdio()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=paths.ROOT / "third_party" / "aosp" / "emu-main-win")
    parser.add_argument("--project", action="append", default=None,
                        help="対象のサブプロジェクト（複数可）。既定は external/qemu と Windows の Clang")
    args = parser.parse_args()
    materialize(args.source_root.resolve(strict=True), tuple(args.project) if args.project else PROJECTS)


if __name__ == "__main__":
    main()
