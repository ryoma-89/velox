"""管理者権限なしのスレッド・サンプラー（Windows）。

対象スレッドを一瞬だけ SuspendThread し、GetThreadContext で命令ポインタ（RIP）を読んで
ResumeThread する。これを一定間隔で繰り返し、RIP をモジュールと関数名（dbghelp と PDB）に
変換して数える。最上位フレームだけの「自己時間」の分布で、コールスタックは取らない。

ETW（WPR）は管理者権限が必要なので、その代わりに使う。対象プロセスのスレッドを止めるため、
間隔は 1 ms 以上にし、短時間の調査だけに使う。
"""
from __future__ import annotations

import collections
import ctypes
import os
import time
from ctypes import wintypes

from velox.hw import threads as _threads

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

THREAD_SUSPEND_RESUME = 0x0002
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
CONTEXT_AMD64 = 0x00100000
CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1


class CONTEXT(ctypes.Structure):
    # x64 CONTEXT は 16 バイト境界が必要。必要なのは Rip（オフセット 0xF8）だけなので、
    # 全体を確保して ContextFlags と Rip の位置だけ使う。
    _fields_ = [("buf", ctypes.c_ubyte * 1232)]


def _aligned_context():
    raw = ctypes.create_string_buffer(1232 + 16)
    addr = (ctypes.addressof(raw) + 15) & ~15
    return raw, addr


k32.OpenThread.restype = wintypes.HANDLE
k32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.SuspendThread.argtypes = [wintypes.HANDLE]
k32.SuspendThread.restype = wintypes.DWORD
k32.ResumeThread.argtypes = [wintypes.HANDLE]
k32.ResumeThread.restype = wintypes.DWORD
k32.GetThreadContext.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
k32.GetThreadContext.restype = wintypes.BOOL
k32.CloseHandle.argtypes = [wintypes.HANDLE]


def sample_thread(tid: int, seconds: float, interval: float = 0.002) -> collections.Counter:
    """tid の RIP を interval 秒ごとに読み、出現回数を返す。"""
    h = k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, False, tid)
    if not h:
        raise OSError(ctypes.get_last_error(), f"OpenThread({tid}) failed")
    raw, addr = _aligned_context()
    counts: collections.Counter = collections.Counter()
    end = time.perf_counter() + seconds
    try:
        while time.perf_counter() < end:
            ctypes.c_uint32.from_address(addr + 0x30).value = CONTEXT_CONTROL  # ContextFlags
            if k32.SuspendThread(h) != 0xFFFFFFFF:
                ok = k32.GetThreadContext(h, addr)
                k32.ResumeThread(h)
                if ok:
                    counts[ctypes.c_uint64.from_address(addr + 0xF8).value] += 1  # Rip
            time.sleep(interval)
    finally:
        k32.CloseHandle(h)
    return counts


class _Symbolizer:
    def __init__(self, pid: int, search_path: str):
        self.dbg = ctypes.WinDLL("dbghelp")
        self.proc = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        self.dbg.SymSetOptions(0x00000002 | 0x00000004 | 0x00000010)  # UNDNAME | DEFERRED_LOADS | LOAD_LINES 無し
        self.dbg.SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
        if not self.dbg.SymInitializeW(self.proc, search_path, True):
            raise OSError(ctypes.get_last_error(), "SymInitialize failed")
        self.mods = _threads._modules(pid)

    def module_of(self, addr: int) -> tuple[str, int]:
        for base, size, name in self.mods:
            if base <= addr < base + size:
                return name, addr - base
        return "?", addr

    def name(self, addr: int) -> str:
        class SYMBOL_INFO(ctypes.Structure):
            _fields_ = [("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG),
                        ("Reserved", ctypes.c_uint64 * 2), ("Index", wintypes.ULONG),
                        ("Size", wintypes.ULONG), ("ModBase", ctypes.c_uint64),
                        ("Flags", wintypes.ULONG), ("Value", ctypes.c_uint64),
                        ("Address", ctypes.c_uint64), ("Register", wintypes.ULONG),
                        ("Scope", wintypes.ULONG), ("Tag", wintypes.ULONG),
                        ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG),
                        ("Name", ctypes.c_char * 512)]
        info = SYMBOL_INFO()
        info.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 512 + 1
        info.MaxNameLen = 511
        disp = ctypes.c_uint64()
        mod, off = self.module_of(addr)
        if self.dbg.SymFromAddr(self.proc, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(info)):
            return f"{mod}!{info.Name.decode(errors='replace')}"
        return f"{mod}+0x{off:x}"


def profile(pid: int, tid: int, seconds: float, pdb_dirs: list[str], interval: float = 0.002,
            top: int = 40) -> dict:
    counts = sample_thread(tid, seconds, interval)
    total = sum(counts.values())
    sym = _Symbolizer(pid, ";".join(pdb_dirs + [os.environ.get("_NT_SYMBOL_PATH", "")]))
    by_func: collections.Counter = collections.Counter()
    by_mod: collections.Counter = collections.Counter()
    for addr, n in counts.items():
        by_func[sym.name(addr)] += n
        by_mod[sym.module_of(addr)[0]] += n
    pct = lambda n: round(100.0 * n / total, 2) if total else 0.0  # noqa: E731
    return {"pid": pid, "tid": tid, "samples": total, "seconds": seconds,
            "modules": [{"module": m, "pct": pct(n)} for m, n in by_mod.most_common(15)],
            "functions": [{"function": f, "pct": pct(n)} for f, n in by_func.most_common(top)]}


# ---- コールスタック付きサンプリング ------------------------------------------

CONTEXT_FULL = CONTEXT_AMD64 | 0x1 | 0x2 | 0x8  # CONTROL | INTEGER | FLOATING_POINT


class _ADDRESS64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", wintypes.WORD), ("Mode", ctypes.c_int)]


class _KDHELP64(ctypes.Structure):
    _fields_ = [("data", ctypes.c_uint64 * 20)]


class _STACKFRAME64(ctypes.Structure):
    _fields_ = [("AddrPC", _ADDRESS64), ("AddrReturn", _ADDRESS64), ("AddrFrame", _ADDRESS64),
                ("AddrStack", _ADDRESS64), ("AddrBStore", _ADDRESS64), ("FuncTableEntry", ctypes.c_void_p),
                ("Params", ctypes.c_uint64 * 4), ("Far", wintypes.BOOL), ("Virtual", wintypes.BOOL),
                ("Reserved", ctypes.c_uint64 * 3), ("KdHelp", _KDHELP64)]


def sample_stacks(pid: int, tid: int, seconds: float, interval: float = 0.003,
                  depth: int = 16) -> collections.Counter:
    """tid のコールスタック（アドレスのタプル）を数える。止めている間に StackWalk64 で辿る。"""
    dbg = ctypes.WinDLL("dbghelp")
    proc = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    dbg.SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
    dbg.SymInitializeW(proc, None, True)  # 関数テーブル（.pdata）の参照に必要
    dbg.StackWalk64.argtypes = [wintypes.DWORD, wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(_STACKFRAME64),
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    fta = ctypes.cast(dbg.SymFunctionTableAccess64, ctypes.c_void_p)
    gmb = ctypes.cast(dbg.SymGetModuleBase64, ctypes.c_void_p)
    h = k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, False, tid)
    if not h:
        raise OSError(ctypes.get_last_error(), f"OpenThread({tid}) failed")
    raw, addr = _aligned_context()
    counts: collections.Counter = collections.Counter()
    end = time.perf_counter() + seconds
    try:
        while time.perf_counter() < end:
            ctypes.c_uint32.from_address(addr + 0x30).value = CONTEXT_FULL
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                break
            try:
                if not k32.GetThreadContext(h, addr):
                    continue
                frame = _STACKFRAME64()
                frame.AddrPC.Offset = ctypes.c_uint64.from_address(addr + 0xF8).value   # Rip
                frame.AddrStack.Offset = ctypes.c_uint64.from_address(addr + 0x98).value  # Rsp
                frame.AddrFrame.Offset = ctypes.c_uint64.from_address(addr + 0xA0).value  # Rbp
                for a in (frame.AddrPC, frame.AddrStack, frame.AddrFrame):
                    a.Mode = 3  # AddrModeFlat
                stack = []
                while len(stack) < depth and dbg.StackWalk64(0x8664, proc, h, ctypes.byref(frame), addr,
                                                               None, fta, gmb, None):
                    if not frame.AddrPC.Offset:
                        break
                    stack.append(frame.AddrPC.Offset)
                counts[tuple(stack)] += 1
            finally:
                k32.ResumeThread(h)
            time.sleep(interval)
    finally:
        k32.CloseHandle(h)
        dbg.SymCleanup(proc)
        k32.CloseHandle(proc)
    return counts


def _pe_exports(path: str) -> list[tuple[int, str]]:
    import struct
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    opt = pe + 24
    nsec, optsz = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    exp_rva = struct.unpack_from("<I", d, opt + 112)[0]
    secs = [struct.unpack_from("<8sIIII", d, opt + optsz + i * 40) for i in range(nsec)]

    def off(rva):
        for _n, vs, va, rs, rp in secs:
            if va <= rva < va + max(vs, rs):
                return rva - va + rp
        return None
    if not exp_rva or off(exp_rva) is None:
        return []
    e = off(exp_rva)
    nnames, af, an, ao = struct.unpack_from("<IIII", d, e + 24)
    res = []
    for i in range(nnames):
        nrva = struct.unpack_from("<I", d, off(an) + 4 * i)[0]
        s = off(nrva)
        name = d[s:d.index(b"\0", s)].decode(errors="replace")
        ordi = struct.unpack_from("<H", d, off(ao) + 2 * i)[0]
        res.append((struct.unpack_from("<I", d, off(af) + 4 * ordi)[0], name))
    return sorted(res)


def symbolize(pid: int, addrs: set[int], pdb_modules: dict[str, str], symbolizer: str) -> dict[int, str]:
    """アドレスを "モジュール!関数" にする。pdb_modules は {モジュール名(小文字): 実体の exe/dll パス}。
    PDB があるモジュールは llvm-symbolizer、それ以外はエクスポート表の直前の名前を使う。"""
    import bisect
    import subprocess
    mods = _threads._modules(pid)
    paths = {}
    try:
        k = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        for base, _size, name in mods:
            buf = ctypes.create_unicode_buffer(1024)
            psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPWSTR, wintypes.DWORD]
            if psapi.GetModuleFileNameExW(k, ctypes.c_void_p(base), buf, 1024):
                paths[base] = buf.value
        k32.CloseHandle(k)
    except OSError:
        pass
    out: dict[int, str] = {}
    by_mod: dict[tuple[int, str], list[int]] = collections.defaultdict(list)
    for a in addrs:
        for base, size, name in mods:
            if base <= a < base + size:
                by_mod[(base, name)].append(a)
                break
        else:
            out[a] = "?"
    for (base, name), lst in by_mod.items():
        pdb_exe = pdb_modules.get(name.lower())
        if pdb_exe:
            # PE の既定ベースは llvm-symbolizer がファイルから読むので、RVA + ImageBase を渡す。
            import struct
            with open(pdb_exe, "rb") as f:
                hdr = f.read(4096)
            pe = struct.unpack_from("<I", hdr, 0x3C)[0]
            image_base = struct.unpack_from("<Q", hdr, pe + 24 + 24)[0]
            q = "\n".join(hex(image_base + a - base) for a in lst) + "\n"
            r = subprocess.run([symbolizer, f"--obj={pdb_exe}", "--functions=linkage", "--no-inlines",
                                "--output-style=GNU"], input=q, capture_output=True, text=True, timeout=300)
            lines = [ln for ln in r.stdout.splitlines()]
            # GNU 形式は 1 アドレスあたり「関数名」「ファイル:行」の 2 行
            for i, a in enumerate(lst):
                fn = lines[2 * i] if 2 * i < len(lines) else "??"
                out[a] = f"{name}!{fn}"
        else:
            ex = _pe_exports(paths.get(base, "")) if paths.get(base) else []
            rv = [r for r, _ in ex]
            for a in lst:
                i = bisect.bisect_right(rv, a - base) - 1
                out[a] = f"{name}!{ex[i][1]}" if i >= 0 else f"{name}+0x{a - base:x}"
    return out
