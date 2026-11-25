"""Windows PDH（Performance Data Helper）による性能カウンタ取得。

言語に依存しないよう PdhAddEnglishCounterW を使う。
start() → （任意の待ち時間）→ finish() で区間平均を得るので、
CPU 使用率の計測区間と共有できる。
"""
from __future__ import annotations

import sys
import time

COUNTERS = {
    "disk_read_mbps": r"\PhysicalDisk(_Total)\Disk Read Bytes/sec",
    "disk_write_mbps": r"\PhysicalDisk(_Total)\Disk Write Bytes/sec",
    "disk_queue": r"\PhysicalDisk(_Total)\Current Disk Queue Length",
    "ctx_switches_per_s": r"\System\Context Switches/sec",
}
_SCALE = {"disk_read_mbps": 1 / 2**20, "disk_write_mbps": 1 / 2**20}


class Query:
    def __init__(self):
        self.ok = False
        if sys.platform != "win32":
            return
        import ctypes
        self._c = ctypes
        self.pdh = ctypes.WinDLL("pdh")
        self.q = ctypes.c_void_p()
        if self.pdh.PdhOpenQueryW(None, None, ctypes.byref(self.q)) != 0:
            return
        self.handles = {}
        for k, path in COUNTERS.items():
            h = ctypes.c_void_p()
            if self.pdh.PdhAddEnglishCounterW(self.q, ctypes.c_wchar_p(path), None, ctypes.byref(h)) == 0:
                self.handles[k] = h
        self.ok = True

    def start(self) -> None:
        if self.ok:
            self.pdh.PdhCollectQueryData(self.q)

    def finish(self) -> dict:
        if not self.ok:
            return {}
        ctypes = self._c
        from ctypes import wintypes

        class FMT(ctypes.Structure):
            _fields_ = [("CStatus", wintypes.DWORD), ("pad", wintypes.DWORD), ("doubleValue", ctypes.c_double)]
        self.pdh.PdhCollectQueryData(self.q)
        out = {}
        for k, h in self.handles.items():
            v = FMT()
            if self.pdh.PdhGetFormattedCounterValue(h, 0x00000200, None, ctypes.byref(v)) == 0:  # PDH_FMT_DOUBLE
                out[k] = round(v.doubleValue * _SCALE.get(k, 1), 2)
        self.pdh.PdhCloseQuery(self.q)
        self.ok = False
        return out


def sample(interval: float = 0.5) -> dict:
    q = Query(); q.start(); time.sleep(interval)
    return q.finish()
