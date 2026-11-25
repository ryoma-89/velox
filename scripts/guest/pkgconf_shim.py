"""pkg-config の最小実装（Python 標準ライブラリのみ）。

Windows 上で Mesa を Android 向けにクロスビルドするときに、meson から呼ばれる。
ゲスト用の依存（android-aemu-* など）を記述した .pc ファイルを PKG_CONFIG_LIBDIR
（または PKG_CONFIG_PATH）から探して、--modversion / --cflags / --libs / --variable に答える。

対応していない問い合わせは「見つからない」として終了コード 1 を返す。
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

VERSION = "0.29.2"


def search_dirs() -> list[Path]:
    dirs: list[Path] = []
    for env in ("PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR"):
        for part in os.environ.get(env, "").split(os.pathsep):
            if part.strip():
                dirs.append(Path(part.strip()))
    return dirs


def find_pc(name: str) -> Path | None:
    for d in search_dirs():
        p = d / f"{name}.pc"
        if p.is_file():
            return p
    return None


_VAR_RE = re.compile(r"\$\{([A-Za-z0-9_.]+)\}")


class Pc:
    def __init__(self, path: Path, overrides: dict[str, str]):
        self.path = path
        self.vars: dict[str, str] = {"pcfiledir": path.parent.as_posix()}
        self.fields: dict[str, str] = {}
        for raw in path.read_text(encoding="utf-8").splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            m = re.match(r"^([A-Za-z0-9_.]+)\s*=\s*(.*)$", line)
            if m and ":" not in m.group(1):
                key = m.group(1)
                self.vars[key] = overrides.get(key, self.expand(m.group(2)))
                continue
            m = re.match(r"^([A-Za-z0-9_.]+)\s*:\s*(.*)$", line)
            if m:
                self.fields[m.group(1).lower()] = self.expand(m.group(2))

    def expand(self, s: str) -> str:
        for _ in range(16):
            n = _VAR_RE.sub(lambda m: self.vars.get(m.group(1), ""), s)
            if n == s:
                break
            s = n
        return s

    def requires(self, private: bool) -> list[str]:
        out = []
        keys = ["requires"] + (["requires.private"] if private else [])
        for k in keys:
            for item in re.split(r"[,\s]+", self.fields.get(k, "")):
                if item and not re.match(r"^[<>=!]", item) and not re.match(r"^[0-9]", item):
                    out.append(item)
        return out


def parse_pkgs(args: list[str]) -> list[tuple[str, str | None, str | None]]:
    """'foo >= 1.0 bar' の形を (名前, 演算子, 版) に分ける。"""
    toks: list[str] = []
    for a in args:
        toks.extend(a.replace(",", " ").split())
    out = []
    i = 0
    while i < len(toks):
        name = toks[i]
        op = ver = None
        if i + 2 < len(toks) + 1 and i + 1 < len(toks) and toks[i + 1] in ("<", "<=", ">", ">=", "=", "!="):
            op, ver = toks[i + 1], toks[i + 2] if i + 2 < len(toks) else ""
            i += 3
        else:
            i += 1
        out.append((name, op, ver))
    return out


def vtuple(v: str) -> tuple:
    return tuple(int(x) if x.isdigit() else x for x in re.split(r"[.\-]", v))


def version_ok(have: str, op: str | None, want: str | None) -> bool:
    if not op:
        return True
    a, b = vtuple(have), vtuple(want or "0")
    try:
        return {"<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b, "=": a == b, "!=": a != b}[op]
    except TypeError:
        return True


def collect(names: list[str], overrides: dict[str, str], static: bool) -> list[Pc] | None:
    seen: dict[str, Pc] = {}
    order: list[Pc] = []

    def visit(n: str) -> bool:
        if n in seen:
            return True
        p = find_pc(n)
        if p is None:
            sys.stderr.write(f"Package {n} was not found in the pkg-config search path.\n")
            return False
        pc = Pc(p, overrides)
        seen[n] = pc
        order.append(pc)
        return all(visit(r) for r in pc.requires(static))

    for n in names:
        if not visit(n):
            return None
    return order


def main(argv: list[str]) -> int:
    flags = [a for a in argv if a.startswith("--")]
    rest = [a for a in argv if not a.startswith("--")]
    if "--version" in flags:
        print(VERSION)
        return 0
    overrides: dict[str, str] = {}
    variable = None
    for f in flags:
        if f.startswith("--define-variable="):
            k, _, v = f[len("--define-variable="):].partition("=")
            overrides[k] = v
        elif f.startswith("--variable="):
            variable = f[len("--variable="):]
    pkgs = parse_pkgs(rest)
    if not pkgs:
        return 1
    static = "--static" in flags
    pcs = collect([p[0] for p in pkgs], overrides, static)
    if pcs is None:
        return 1
    by_name = {pc.path.stem: pc for pc in pcs}
    for name, op, ver in pkgs:
        if not version_ok(by_name[name].fields.get("version", "0"), op, ver):
            sys.stderr.write(f"Requested '{name} {op} {ver}' but version is {by_name[name].fields.get('version')}\n")
            return 1
    out: list[str] = []
    if "--modversion" in flags:
        for name, _, _ in pkgs:
            print(by_name[name].fields.get("version", "0"))
        return 0
    if variable is not None:
        print(by_name[pkgs[0][0]].vars.get(variable, ""))
        return 0
    if "--print-variables" in flags:
        for k in by_name[pkgs[0][0]].vars:
            print(k)
        return 0
    want_cflags = any(f.startswith("--cflags") for f in flags)
    want_libs = any(f.startswith("--libs") for f in flags)
    for pc in pcs:
        if want_cflags:
            out.extend(pc.fields.get("cflags", "").split())
        if want_libs:
            out.extend(pc.fields.get("libs", "").split())
            if static:
                out.extend(pc.fields.get("libs.private", "").split())
    if "--cflags-only-I" in flags:
        out = [o for o in out if o.startswith("-I")]
    if "--libs-only-l" in flags:
        out = [o for o in out if o.startswith("-l")]
    if "--libs-only-L" in flags:
        out = [o for o in out if o.startswith("-L")]
    dedup: list[str] = []
    for o in out:
        if o not in dedup or not o.startswith("-I"):
            dedup.append(o)
    print(" ".join(dedup))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
