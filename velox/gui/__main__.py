"""python -m velox.gui エントリーポイント。"""
from __future__ import annotations

import sys

from velox.gui.app import launch

if __name__ == "__main__":
    profile = sys.argv[1] if len(sys.argv) > 1 else None
    sys.exit(launch(initial_profile=profile))
