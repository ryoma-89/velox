"""CPU トポロジー検出（ハイブリッド構成の P コア / E コア判別）。

Windows では GetSystemCpuSetInformation の EfficiencyClass を使う。
EfficiencyClass が大きいほど高性能コア（Intel ハイブリッドでは P=1, E=0）。
ハードウェア依存部分はこのモジュールに閉じ込め、他プラットフォームは
フォールバック実装（全コアを同一クラス扱い）を返す。
"""
from __future__ import annotations

import os
import struct
import sys
from dataclasses import dataclass, asdict


@dataclass(frozen=True)
class LogicalCpu:
    index: int            # グループ内論理プロセッサ番号（affinity マスクのビット位置）
    group: int
    core_index: int
    llc_index: int        # 最終レベルキャッシュ（L3 / E コアクラスタ L2 判別の参考）
    efficiency_class: int
    kind: str             # "P" | "E" | "U"(不明)


@dataclass
class CpuTopology:
    cpus: list[LogicalCpu]

    @property
    def p_cores(self) -> list[LogicalCpu]:
        return [c for c in self.cpus if c.kind == "P"]

    @property
    def e_cores(self) -> list[LogicalCpu]:
        return [c for c in self.cpus if c.kind == "E"]

    def mask(self, kinds: str = "PE") -> int:
        m = 0
        for c in self.cpus:
            if c.kind in kinds or (c.kind == "U" and "P" in kinds):
                m |= 1 << c.index
        return m

    def to_dict(self) -> dict:
        return {
            "logical": len(self.cpus),
            "p_cores": [c.index for c in self.p_cores],
            "e_cores": [c.index for c in self.e_cores],
            "mask_p": hex(self.mask("P")),
            "mask_e": hex(self.mask("E")),
            "mask_all": hex(self.mask("PE")),
            "cpus": [asdict(c) for c in self.cpus],
        }


def _windows_topology() -> CpuTopology:
    import ctypes
    from ctypes import wintypes

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    fn = k32.GetSystemCpuSetInformation
    fn.argtypes = [ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG),
                   wintypes.HANDLE, wintypes.ULONG]
    fn.restype = wintypes.BOOL
    needed = wintypes.ULONG(0)
    fn(None, 0, ctypes.byref(needed), None, 0)
    buf = ctypes.create_string_buffer(needed.value)
    if not fn(buf, needed, ctypes.byref(needed), None, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    raw = buf.raw[: needed.value]
    entries = []
    off = 0
    while off < len(raw):
        size, typ = struct.unpack_from("<II", raw, off)
        if size == 0:
            break
        if typ == 0:  # CpuSetInformation
            _id, group, lp, core, llc, _numa, eff = struct.unpack_from("<IHBBBBB", raw, off + 8)
            entries.append((lp, group, core, llc, eff))
        off += size
    classes = sorted({e[4] for e in entries})
    hybrid = len(classes) > 1
    cpus = []
    for lp, group, core, llc, eff in sorted(entries, key=lambda e: (e[1], e[0])):
        if hybrid:
            kind = "P" if eff == classes[-1] else "E"
        else:
            kind = "P"
        cpus.append(LogicalCpu(lp, group, core, llc, eff, kind))
    return CpuTopology(cpus)


def detect() -> CpuTopology:
    if sys.platform == "win32":
        try:
            return _windows_topology()
        except OSError:
            pass
    n = os.cpu_count() or 1
    return CpuTopology([LogicalCpu(i, 0, i, 0, 0, "U") for i in range(n)])
