"""プロセス内のスレッド別 CPU 使用率（Windows、依存なし）。

NtQuerySystemInformation(SystemProcessInformation) で全スレッドの CPU 時間を一度に取得し、
2 回の差分から使用率を求める。スレッド名は GetThreadDescription で取得する
（QEMU / gfxstream が名前を設定していれば vCPU と描画スレッドを区別できる）。
100% = 論理 CPU 1 個分。
"""
from __future__ import annotations

import struct
import sys
import time

_PROC_HDR = 256      # sizeof(SYSTEM_PROCESS_INFORMATION) on x64
_THREAD_SZ = 80      # sizeof(SYSTEM_THREAD_INFORMATION) on x64


def _snapshot() -> dict[int, dict]:
    """{pid: {"name": str, "threads": {tid: kernel+user (100ns)}}}"""
    import ctypes
    ntdll = ctypes.windll.ntdll
    size = 1 << 22
    while True:
        buf = ctypes.create_string_buffer(size)
        ret = ctypes.c_ulong()
        st = ntdll.NtQuerySystemInformation(5, buf, size, ctypes.byref(ret)) & 0xFFFFFFFF
        if st == 0xC0000004:  # STATUS_INFO_LENGTH_MISMATCH
            size = max(size * 2, ret.value + 65536)
            continue
        if st != 0:
            raise OSError(f"NtQuerySystemInformation status=0x{st:x}")
        break
    raw, base = buf.raw, ctypes.addressof(buf)
    out, off = {}, 0
    while True:
        nxt, nthreads = struct.unpack_from("<II", raw, off)
        name_len, = struct.unpack_from("<H", raw, off + 56)
        name_ptr, = struct.unpack_from("<Q", raw, off + 64)
        pid, = struct.unpack_from("<Q", raw, off + 80)
        name = ""
        if name_ptr and name_len:
            name = raw[name_ptr - base: name_ptr - base + name_len].decode("utf-16-le", errors="replace")
        threads = {}
        for i in range(nthreads):
            t = off + _PROC_HDR + i * _THREAD_SZ
            kt, ut = struct.unpack_from("<qq", raw, t)
            tid, = struct.unpack_from("<Q", raw, t + 48)
            threads[tid] = kt + ut
        out[pid] = {"name": name, "threads": threads}
        if nxt == 0:
            break
        off += nxt
    return out


def thread_name(tid: int) -> str:
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenThread.restype = wintypes.HANDLE
    h = k32.OpenThread(0x1000, False, tid)  # THREAD_QUERY_LIMITED_INFORMATION
    if not h:
        return ""
    try:
        p = ctypes.c_wchar_p()
        fn = getattr(k32, "GetThreadDescription", None)
        if fn and fn(wintypes.HANDLE(h), ctypes.byref(p)) >= 0 and p.value:
            name = p.value
            k32.LocalFree(p)
            return name
        return ""
    finally:
        k32.CloseHandle(wintypes.HANDLE(h))


def _modules(pid: int) -> list[tuple[int, int, str]]:
    """(base, size, name) のモジュール一覧。"""
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    h = k32.OpenProcess(0x0410, False, pid)  # QUERY_INFORMATION | VM_READ
    if not h:
        return []
    try:
        arr = (ctypes.c_void_p * 2048)()
        need = wintypes.DWORD()
        psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
        if not psapi.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), 3):
            return []

        class MI(ctypes.Structure):
            _fields_ = [("base", ctypes.c_void_p), ("size", wintypes.DWORD), ("entry", ctypes.c_void_p)]
        out = []
        psapi.GetModuleInformation.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MI), wintypes.DWORD]
        psapi.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPWSTR, wintypes.DWORD]
        for i in range(min(need.value // ctypes.sizeof(ctypes.c_void_p), 2048)):
            mi = MI()
            name = ctypes.create_unicode_buffer(260)
            if psapi.GetModuleInformation(h, arr[i], ctypes.byref(mi), ctypes.sizeof(mi)):
                psapi.GetModuleBaseNameW(h, arr[i], name, 260)
                out.append((mi.base or 0, mi.size, name.value))
        return out
    finally:
        k32.CloseHandle(wintypes.HANDLE(h))


def start_module(tid: int, modules: list[tuple[int, int, str]]) -> str:
    """スレッドの Win32 開始アドレスが属するモジュール名（+オフセット）。"""
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenThread.restype = wintypes.HANDLE
    h = k32.OpenThread(0x0040, False, tid)  # THREAD_QUERY_INFORMATION
    if not h:
        return ""
    try:
        addr = ctypes.c_void_p()
        st = ctypes.windll.ntdll.NtQueryInformationThread(wintypes.HANDLE(h), 9, ctypes.byref(addr), 8, None)
        if st != 0 or not addr.value:
            return ""
        for base, size, name in modules:
            if base <= addr.value < base + size:
                return f"{name}+0x{addr.value - base:x}"
        return hex(addr.value)
    finally:
        k32.CloseHandle(wintypes.HANDLE(h))


def sample(process_names: tuple[str, ...], interval: float = 2.0, top: int = 10, with_start: bool = False,
           only_pid: int | None = None) -> dict:
    """対象プロセスのスレッド別 CPU 使用率（上位 top 件）と合計を返す。"""
    if sys.platform != "win32":
        return {}
    names = tuple(n.lower() for n in process_names)
    a = _snapshot(); t0 = time.perf_counter()
    time.sleep(interval)
    b = _snapshot(); dt = time.perf_counter() - t0
    rows = []
    for pid, pb in b.items():
        if (only_pid is not None and pid != only_pid) or pb["name"].lower() not in names or pid not in a:
            continue
        ta = a[pid]["threads"]
        for tid, cb in pb["threads"].items():
            if tid in ta:
                pct = (cb - ta[tid]) / (dt * 1e7) * 100
                if pct > 0.5:
                    rows.append((pid, tid, round(pct, 1)))
    rows.sort(key=lambda r: -r[2])
    mods = _modules(rows[0][0]) if (with_start and rows) else []
    tops = []
    for _pid, tid, pct in rows[:top]:
        d = {"tid": tid, "pct": pct, "name": thread_name(tid)}
        if with_start:
            d["start"] = start_module(tid, mods)
        tops.append(d)
    return {"sum_pct": round(sum(r[2] for r in rows), 1),
            "busy_threads": sum(1 for r in rows if r[2] >= 50), "top": tops}
