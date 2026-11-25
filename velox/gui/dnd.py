"""Windows ネイティブ Drag & Drop（WM_DROPFILES）ヘルパー（追加依存ゼロ）。

ctypes で tkinter トップレベルウィンドウへ DragAcceptFiles とサブクラス WndProc を登録し、
エクスプローラーからドロップされたファイルパス一覧を安全にコールバックへ渡す。
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes
import sys
from typing import Callable


def enable_high_dpi() -> float:
    """Windows Per-Monitor V2 High-DPI 対応を有効化し、推定スケーリング倍率を返す。"""
    if sys.platform != "win32":
        return 1.0
    try:
        # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
        ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:
            try:
                ctypes.windll.user32.SetProcessDPIAware()
            except Exception:
                pass
    try:
        dpi = ctypes.windll.user32.GetDpiForSystem()
        if dpi and dpi > 0:
            return dpi / 96.0
    except Exception:
        pass
    return 1.0


def enable_dark_titlebar(tk_root) -> None:
    """Windows 10/11 のウィンドウタイトルバーをダークモードに設定する（失敗時は無視）。"""
    if sys.platform != "win32":
        return
    try:
        tk_root.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(tk_root.winfo_id())
        if not hwnd:
            hwnd = tk_root.winfo_id()
        val = ctypes.c_int(1)
        # DWMWA_USE_IMMERSIVE_DARK_MODE (20 on Win11 / 19 on older Win10)
        for attr in (20, 19):
            ctypes.windll.dwmapi.DwmSetWindowAttribute(
                wintypes.HWND(hwnd),
                ctypes.c_uint(attr),
                ctypes.byref(val),
                ctypes.sizeof(val),
            )
    except Exception:
        pass


class WindowsFileDropper:
    """Tk ウィンドウに WM_DROPFILES をフックする（GC でコールバックが消えないよう保持）。"""

    WM_DROPFILES = 0x0233
    GWLP_WNDPROC = -4

    def __init__(self, tk_root, on_files: Callable[[list[str]], None]):
        self._root = tk_root
        self._on_files = on_files
        self._orig_proc = None
        self._new_proc = None
        self._hwnd = None

    def attach(self) -> bool:
        if sys.platform != "win32":
            return False
        try:
            self._root.update_idletasks()
            user32 = ctypes.windll.user32
            shell32 = ctypes.windll.shell32

            hwnd = user32.GetParent(self._root.winfo_id())
            if not hwnd:
                hwnd = self._root.winfo_id()
            self._hwnd = hwnd

            shell32.DragAcceptFiles.argtypes = [wintypes.HWND, wintypes.BOOL]
            shell32.DragQueryFileW.argtypes = [
                wintypes.HANDLE,
                wintypes.UINT,
                wintypes.LPWSTR,
                wintypes.UINT,
            ]
            shell32.DragQueryFileW.restype = wintypes.UINT
            shell32.DragFinish.argtypes = [wintypes.HANDLE]

            LRESULT = ctypes.c_ssize_t
            WPARAM = ctypes.c_size_t
            LPARAM = ctypes.c_ssize_t
            WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT, WPARAM, LPARAM)

            user32.CallWindowProcW.argtypes = [
                ctypes.c_void_p,
                wintypes.HWND,
                wintypes.UINT,
                WPARAM,
                LPARAM,
            ]
            user32.CallWindowProcW.restype = LRESULT
            user32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_void_p]
            user32.SetWindowLongPtrW.restype = ctypes.c_void_p

            def _wndproc(h, msg, wp, lp):
                if msg == self.WM_DROPFILES:
                    hdrop = wintypes.HANDLE(wp)
                    try:
                        count = shell32.DragQueryFileW(hdrop, 0xFFFFFFFF, None, 0)
                        files: list[str] = []
                        for i in range(count):
                            n = shell32.DragQueryFileW(hdrop, i, None, 0)
                            buf = ctypes.create_unicode_buffer(n + 1)
                            shell32.DragQueryFileW(hdrop, i, buf, n + 1)
                            if buf.value:
                                files.append(buf.value)
                        if files:
                            self._root.after(0, lambda f=files: self._on_files(f))
                    finally:
                        shell32.DragFinish(hdrop)
                    return 0
                return user32.CallWindowProcW(self._orig_proc, h, msg, wp, lp)

            self._new_proc = WNDPROC(_wndproc)
            ptr = ctypes.cast(self._new_proc, ctypes.c_void_p).value
            self._orig_proc = user32.SetWindowLongPtrW(hwnd, self.GWLP_WNDPROC, ptr)
            shell32.DragAcceptFiles(hwnd, True)
            return True
        except Exception:
            return False

    def detach(self) -> None:
        if sys.platform != "win32" or not self._hwnd or not self._orig_proc:
            return
        try:
            ctypes.windll.user32.SetWindowLongPtrW(self._hwnd, self.GWLP_WNDPROC, self._orig_proc)
            self._orig_proc = None
        except Exception:
            pass
