"""共通ユーティリティ（外部コマンド実行・JSON 出力・時刻）。"""
from __future__ import annotations

import datetime as _dt
import json
import subprocess
import sys
from dataclasses import dataclass

CREATE_NO_WINDOW = 0x08000000 if sys.platform == "win32" else 0


@dataclass
class CmdResult:
    code: int
    out: str
    err: str

    @property
    def ok(self) -> bool:
        return self.code == 0


def run(cmd: list[str], timeout: float = 60, input: str | None = None) -> CmdResult:
    """外部コマンドを実行する。存在しない・タイムアウトは code<0 で返し例外にしない。"""
    try:
        # stdin を閉じておく（vulkaninfo 等は stdin 待ちで停止することがある）
        kw = {"input": input.encode()} if input is not None else {"stdin": subprocess.DEVNULL}
        p = subprocess.run(cmd, capture_output=True, timeout=timeout,
                           creationflags=CREATE_NO_WINDOW, **kw)
    except FileNotFoundError:
        return CmdResult(-127, "", f"not found: {cmd[0]}")
    except subprocess.TimeoutExpired:
        return CmdResult(-124, "", f"timeout after {timeout}s: {' '.join(cmd)}")
    return CmdResult(p.returncode, decode(p.stdout), decode(p.stderr))


def decode(b: bytes | None) -> str:
    """UTF-8 を優先し、失敗したら OS のコードページ（日本語 Windows では cp932）で復号する。"""
    if not b:
        return ""
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        return b.decode("mbcs" if sys.platform == "win32" else "latin-1", errors="replace")


def setup_stdio() -> None:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def powershell_json(script: str, timeout: float = 60):
    r = run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
             f"[Console]::OutputEncoding=[Text.Encoding]::UTF8; {script} | ConvertTo-Json -Depth 4 -Compress"],
            timeout=timeout)
    if not r.ok or not r.out.strip():
        return None
    try:
        return json.loads(r.out)
    except json.JSONDecodeError:
        return None


def now_iso() -> str:
    return _dt.datetime.now().astimezone().isoformat(timespec="seconds")


def dumps(obj) -> str:
    return json.dumps(obj, ensure_ascii=False, indent=2, default=str)
