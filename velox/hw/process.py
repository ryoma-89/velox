"""ホストプロセスのスケジューリング制御（Windows）。

- affinity: 子プロセスは親の affinity を継承するので、起動直前に自プロセスの
  affinity を一時的に変更して Popen し、直後に戻す。
- priority: CreateProcess の優先度クラスで指定（REALTIME は使わない）。
- power throttling (EcoQoS): Windows 11 はバックグラウンド/非フォーカスの
  プロセスを E コアへ寄せたりクロックを抑えたりする。SetProcessInformation で
  ExecutionSpeed のスロットリングを明示的に無効化する。
"""
from __future__ import annotations

import contextlib
import subprocess
import sys

PRIORITY_FLAGS = {"normal": 0x20, "above_normal": 0x8000, "high": 0x80}


def _k32():
    import ctypes
    return ctypes.WinDLL("kernel32", use_last_error=True)


@contextlib.contextmanager
def inherited_affinity(mask: int | None):
    """with ブロック内で起動した子プロセスに affinity マスクを継承させる。"""
    if sys.platform != "win32" or not mask:
        yield
        return
    import ctypes
    from ctypes import wintypes
    k32 = _k32()
    k32.GetCurrentProcess.restype = wintypes.HANDLE
    k32.GetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    k32.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
    h = k32.GetCurrentProcess()
    proc, sysm = ctypes.c_size_t(), ctypes.c_size_t()
    k32.GetProcessAffinityMask(h, ctypes.byref(proc), ctypes.byref(sysm))
    if not k32.SetProcessAffinityMask(h, mask & sysm.value):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        yield
    finally:
        k32.SetProcessAffinityMask(h, proc.value)


def popen(cmd: list[str], *, affinity: int | None, priority: str, env=None, log=None) -> subprocess.Popen:
    flags = PRIORITY_FLAGS.get(priority, 0x20) if sys.platform == "win32" else 0
    if sys.platform == "win32":
        flags |= 0x00000200  # CREATE_NEW_PROCESS_GROUP（CLI 終了でエミュレーターを巻き込まない）
    with inherited_affinity(affinity):
        return subprocess.Popen(cmd, env=env, stdout=log or subprocess.DEVNULL, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, creationflags=flags, close_fds=True)


def descendants(pid: int) -> list[tuple[int, str]]:
    """pid 配下の子孫プロセス（pid, exe 名）を列挙する。"""
    parent: dict[int, list[tuple[int, str]]] = {}
    for p, pp, n in snapshot():
        parent.setdefault(pp, []).append((p, n))
    out, stack = [], [pid]
    while stack:
        for child in parent.get(stack.pop(), []):
            out.append(child); stack.append(child[0])
    return out


def find_processes(names: tuple[str, ...]) -> list[int]:
    """実行ファイル名（小文字）で全プロセスから検索する。"""
    return [p for p, _pp, n in snapshot() if n.lower() in names]


def snapshot() -> list[tuple[int, int, str]]:
    """全プロセスの (pid, ppid, exe 名)。"""
    if sys.platform != "win32":
        return []
    import ctypes
    from ctypes import wintypes

    class PE32(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]
    k32 = _k32()
    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    e = PE32(); e.dwSize = ctypes.sizeof(PE32)
    out = []
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        out.append((e.th32ProcessID, e.th32ParentProcessID, e.szExeFile))
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def tune_process(pid: int, *, affinity: int | None = None, disable_throttling: bool = True,
                 priority: str | None = None) -> dict:
    """既存プロセスにスケジューリング設定を適用する。結果を dict で返す。"""
    if sys.platform != "win32":
        return {"pid": pid, "skipped": "non-windows"}
    import ctypes
    from ctypes import wintypes
    k32 = _k32()
    k32.OpenProcess.restype = wintypes.HANDLE
    h = k32.OpenProcess(0x0200 | 0x0400, False, pid)  # SET_INFORMATION | QUERY_INFORMATION
    res = {"pid": pid}
    if not h:
        res["error"] = f"OpenProcess failed ({ctypes.get_last_error()})"
        return res
    try:
        if affinity:
            # SetProcessAffinityMask は、プロセス内の全スレッドの affinity を上書きする。QEMU と gfxstream が
            # 役割ごとに付けた配置（cpu.thread_placement、第 13〜14 段階）を消さないよう、子プロセスが
            # 起動時に継承したマスクが既に同じなら呼ばない（第 14 段階で、vCPU スレッドの作成とこの呼び出しの
            # 前後関係によって配置が起動ごとに消えたり残ったりしていた）。
            proc_mask, sys_mask = ctypes.c_size_t(), ctypes.c_size_t()
            k32.GetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t),
                                                   ctypes.POINTER(ctypes.c_size_t)]
            if k32.GetProcessAffinityMask(h, ctypes.byref(proc_mask), ctypes.byref(sys_mask)) and \
                    proc_mask.value == affinity:
                res["affinity"] = True
                res["affinity_inherited"] = True
            else:
                k32.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
                res["affinity"] = bool(k32.SetProcessAffinityMask(h, affinity))
        if priority:
            res["priority"] = bool(k32.SetPriorityClass(h, PRIORITY_FLAGS[priority]))
        if disable_throttling:
            class PPT(ctypes.Structure):
                _fields_ = [("Version", wintypes.ULONG), ("ControlMask", wintypes.ULONG), ("StateMask", wintypes.ULONG)]
            # PROCESS_POWER_THROTTLING_EXECUTION_SPEED=1, IGNORE_TIMER_RESOLUTION=4
            st = PPT(1, 0x1 | 0x4, 0)  # 制御対象にし、状態は OFF（=スロットリングしない）
            k32.SetProcessInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
            res["power_throttling_off"] = bool(k32.SetProcessInformation(h, 4, ctypes.byref(st), ctypes.sizeof(st)))
    finally:
        k32.CloseHandle(h)
    return res
