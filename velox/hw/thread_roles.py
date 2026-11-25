"""エミュレーターのスレッドの名前・CPU 時間・affinity を読む（Windows、標準ライブラリの ctypes だけ）。

第 13 段階で、ソース版のホストはスレッドに名前を付ける（gfxstream 0019: "gfxstream-render"、
"velox-asg-prefetch"、QEMU 0010: "CPU n/WHPX" など）。名前から役割を判別し、
cpu.thread_placement の設定どおりに affinity が掛かっているか、役割ごとの CPU 使用率はどれだけかを
テレメトリーとして確かめる。現在どのコアで動いているかは OS が公開しないので、affinity マスクと
（ホスト側の VELOX_ASG_PROFILE が記録する）P コア上の標本の割合で確認する。
"""
from __future__ import annotations

import sys
import time
from dataclasses import dataclass, asdict

ROLE_PREFIXES = (
    ("vcpu", "CPU "),
    ("render", "gfxstream-render"),
    ("prefetch", "velox-asg-prefetch"),
    ("fencewatch", "velox-fence-watch"),
)


def role_of(name: str) -> str:
    for role, prefix in ROLE_PREFIXES:
        if name.startswith(prefix) and (role != "vcpu" or name.endswith("/WHPX")):
            return role
    return "other"


@dataclass
class ThreadInfo:
    tid: int
    name: str
    role: str
    cpu_ms: float
    affinity: int | None

    def to_dict(self) -> dict:
        d = asdict(self)
        d["affinity"] = hex(self.affinity) if self.affinity is not None else None
        return d


def list_threads(pid: int) -> list[ThreadInfo]:
    if sys.platform != "win32":
        return []
    import ctypes
    from ctypes import wintypes

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    ntdll = ctypes.WinDLL("ntdll")

    class THREADENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ThreadID", wintypes.DWORD), ("th32OwnerProcessID", wintypes.DWORD),
                    ("tpBasePri", wintypes.LONG), ("tpDeltaPri", wintypes.LONG),
                    ("dwFlags", wintypes.DWORD)]

    class THREAD_BASIC_INFORMATION(ctypes.Structure):
        _fields_ = [("ExitStatus", wintypes.LONG), ("TebBaseAddress", ctypes.c_void_p),
                    ("UniqueProcess", ctypes.c_void_p), ("UniqueThread", ctypes.c_void_p),
                    ("AffinityMask", ctypes.c_size_t), ("Priority", wintypes.LONG),
                    ("BasePriority", wintypes.LONG)]

    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    k32.OpenThread.restype = wintypes.HANDLE
    get_desc = getattr(k32, "GetThreadDescription", None)
    TH32CS_SNAPTHREAD = 0x4
    THREAD_QUERY_LIMITED_INFORMATION = 0x0800
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if not snap or snap == wintypes.HANDLE(-1).value:
        return []
    out: list[ThreadInfo] = []
    try:
        entry = THREADENTRY32()
        entry.dwSize = ctypes.sizeof(entry)
        ok = k32.Thread32First(snap, ctypes.byref(entry))
        while ok:
            if entry.th32OwnerProcessID == pid:
                tid = entry.th32ThreadID
                h = k32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, tid)
                if h:
                    try:
                        name = ""
                        if get_desc:
                            p = ctypes.c_wchar_p()
                            if get_desc(wintypes.HANDLE(h), ctypes.byref(p)) >= 0 and p.value:
                                name = p.value
                                k32.LocalFree(p)
                        c, e, kt, ut = (wintypes.FILETIME() for _ in range(4))
                        cpu_ms = 0.0
                        if k32.GetThreadTimes(wintypes.HANDLE(h), ctypes.byref(c), ctypes.byref(e),
                                              ctypes.byref(kt), ctypes.byref(ut)):
                            def ft(x):
                                return (x.dwHighDateTime << 32 | x.dwLowDateTime) / 1e4
                            cpu_ms = ft(kt) + ft(ut)
                        tbi = THREAD_BASIC_INFORMATION()
                        affinity = None
                        if ntdll.NtQueryInformationThread(wintypes.HANDLE(h), 0, ctypes.byref(tbi),
                                                          ctypes.sizeof(tbi), None) == 0:
                            affinity = tbi.AffinityMask
                        out.append(ThreadInfo(tid, name, role_of(name), cpu_ms, affinity))
                    finally:
                        k32.CloseHandle(wintypes.HANDLE(h))
            ok = k32.Thread32Next(snap, ctypes.byref(entry))
    finally:
        k32.CloseHandle(wintypes.HANDLE(snap))
    return out


def role_summary(pid: int, interval: float = 2.0) -> dict:
    """interval 秒の間の役割ごとの CPU 使用率（コア 1 個 = 100%）と affinity マスクの一覧。"""
    before = {t.tid: t for t in list_threads(pid)}
    time.sleep(interval)
    after = list_threads(pid)
    roles: dict[str, dict] = {}
    for t in after:
        prev = before.get(t.tid)
        used = t.cpu_ms - (prev.cpu_ms if prev else 0.0)
        r = roles.setdefault(t.role, {"threads": 0, "cpu_pct": 0.0, "busiest_pct": 0.0,
                                      "affinity": set()})
        r["threads"] += 1
        pct = 100.0 * used / (interval * 1000)
        r["cpu_pct"] += pct
        r["busiest_pct"] = max(r["busiest_pct"], pct)
        if t.affinity is not None:
            r["affinity"].add(hex(t.affinity))
    for r in roles.values():
        r["cpu_pct"] = round(r["cpu_pct"], 1)
        r["busiest_pct"] = round(r["busiest_pct"], 1)
        r["affinity"] = sorted(r["affinity"])
    return {"interval_s": interval, "roles": roles}
