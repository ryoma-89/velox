"""ホストテレメトリー（依存ライブラリなし）。

- コア別 CPU 使用率: NtQuerySystemInformation(SystemProcessorPerformanceInformation)
- メモリ: GlobalMemoryStatusEx
- GPU: nvidia-smi（使用率・VRAM・温度・電力・クロック）
- CPU 温度・パッケージ電力: Windows 標準 API では取得不可（KNOWN_ISSUES 参照）。
  将来 LibreHardwareMonitor 等のプロバイダを差し込めるよう provider 構造にしている。
"""
from __future__ import annotations

import struct
import sys
import time

from velox.core.util import now_iso, run
from velox.hw import cpu_topology


def _cpu_times() -> list[tuple[int, int, int]]:
    """論理 CPU ごとの (idle, kernel, user) 100ns 単位。kernel は idle を含む。"""
    import ctypes
    n = cpu_topology.detect().cpus.__len__()
    size = 48 * n
    buf = ctypes.create_string_buffer(size)
    ret = ctypes.c_ulong()
    st = ctypes.windll.ntdll.NtQuerySystemInformation(8, buf, size, ctypes.byref(ret))
    if st != 0:
        raise OSError(f"NtQuerySystemInformation status=0x{st & 0xffffffff:x}")
    return [struct.unpack_from("<qqq", buf.raw, i * 48) for i in range(ret.value // 48)]


def cpu_usage(interval: float = 0.5, extra: dict | None = None) -> list[float]:
    """コア別 CPU 使用率。extra を渡すと同じ区間の PDH カウンタ（ディスク I/O 等）も格納する。"""
    if sys.platform != "win32":
        return []
    from velox.hw import pdh
    q = pdh.Query()
    a = _cpu_times(); q.start(); time.sleep(interval); b = _cpu_times()
    if extra is not None:
        extra.update(q.finish())
    else:
        q.finish()
    out = []
    for (i0, k0, u0), (i1, k1, u1) in zip(a, b):
        total = (k1 - k0) + (u1 - u0)
        out.append(round(100.0 * (1 - (i1 - i0) / total), 1) if total > 0 else 0.0)
    return out


def memory() -> dict:
    if sys.platform != "win32":
        return {}
    import ctypes

    class MS(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong)] + \
                   [(n, ctypes.c_ulonglong) for n in ("total", "avail", "ptotal", "pavail", "vtotal", "vavail", "ext")]
    m = MS(); m.dwLength = ctypes.sizeof(MS)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return {"total_mb": m.total // 2**20, "used_mb": (m.total - m.avail) // 2**20, "load_pct": m.dwMemoryLoad}


GPU_Q = "utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw,clocks.sm,clocks.mem,pstate"


def gpu() -> dict:
    r = run(["nvidia-smi", f"--query-gpu={GPU_Q}", "--format=csv,noheader,nounits"], timeout=10)
    if not r.ok or not r.out.strip():
        return {}
    vals = [v.strip() for v in r.out.strip().splitlines()[0].split(",")]
    out = {}
    for k, v in zip(GPU_Q.split(","), vals):
        try:
            out[k] = float(v)
        except ValueError:
            out[k] = v
    return out


def sample(interval: float = 0.5, light: bool = False) -> dict:
    """1 サンプル。light=True はベンチマーク中用（nvidia-smi 呼び出しのみ省略しない最小構成）。"""
    topo = cpu_topology.detect()
    counters: dict = {}
    usage = cpu_usage(interval, counters)
    s = {"t": now_iso(), "memory": memory(), "gpu": gpu(), "io": counters}
    if usage:
        p = [usage[c.index] for c in topo.p_cores if c.index < len(usage)]
        e = [usage[c.index] for c in topo.e_cores if c.index < len(usage)]
        s["cpu"] = {"total": round(sum(usage) / len(usage), 1),
                    "p_avg": round(sum(p) / len(p), 1) if p else None,
                    "e_avg": round(sum(e) / len(e), 1) if e else None}
        if not light:
            s["cpu"]["per_core"] = usage
    s["unavailable"] = ["cpu.temperature", "cpu.package_power"]
    return s


class Recorder:
    """ベンチマーク中にバックグラウンドで低頻度サンプリングする。"""

    def __init__(self, period: float = 5.0):
        import threading
        self.period = period
        self.samples: list[dict] = []
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._loop, daemon=True)

    def _loop(self):
        while not self._stop.is_set():
            try:
                self.samples.append(sample(0.5, light=True))
            except Exception as e:  # テレメトリー失敗でベンチマークを止めない
                self.samples.append({"error": str(e)})
            self._stop.wait(self.period)

    def __enter__(self):
        self._t.start(); return self

    def __exit__(self, *a):
        self._stop.set(); self._t.join(timeout=10)

    def summary(self) -> dict:
        def agg(path):
            vals = []
            for s in self.samples:
                cur = s
                for k in path:
                    cur = cur.get(k) if isinstance(cur, dict) else None
                if isinstance(cur, (int, float)):
                    vals.append(cur)
            return {"max": max(vals), "avg": round(sum(vals) / len(vals), 1)} if vals else None
        return {"n": len(self.samples),
                "cpu_total": agg(("cpu", "total")), "cpu_p": agg(("cpu", "p_avg")), "cpu_e": agg(("cpu", "e_avg")),
                "gpu_util": agg(("gpu", "utilization.gpu")), "gpu_temp": agg(("gpu", "temperature.gpu")),
                "gpu_power": agg(("gpu", "power.draw")), "gpu_clock": agg(("gpu", "clocks.sm")),
                "vram_mb": agg(("gpu", "memory.used")), "ram_used_mb": agg(("memory", "used_mb")),
                "disk_read_mbps": agg(("io", "disk_read_mbps")), "disk_write_mbps": agg(("io", "disk_write_mbps")),
                "ctx_switches_per_s": agg(("io", "ctx_switches_per_s"))}
