"""Windows ショートカットから指定 Android アプリを直接起動するエントリーポイント。"""
from __future__ import annotations

import argparse
import sys

from velox.backends import apps as appmod, desktop as deskmod
from velox.backends.aosp import AospEmulator
from velox.core import devices as devmod


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Velox Android アプリ直接起動")
    ap.add_argument("--device", required=True, help="端末 ID")
    ap.add_argument("--package", required=True, help="起動する Android パッケージ名")
    ap.add_argument("--windowed", action="store_true", help="Freeform ウィンドウで起動")
    args = ap.parse_args(argv)

    emu = AospEmulator()
    st = emu.status(light=True)
    if not st.get("running"):
        dev = devmod.get_device(args.device)
        info = emu.start(dev["config"], wait=True)
        serial = info.get("serial") or "emulator-5554"
    else:
        serial = (st.get("runtime") or {}).get("serial") or "emulator-5554"

    if args.windowed:
        deskmod.launch_app_freeform(serial, args.package, mode="windowed")
    else:
        appmod.launch_app(serial, args.package)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
