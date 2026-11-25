"""プロジェクトおよび実用版（スタンドアロン）のパス定義。

- 開発環境（リポジトリ直下）:
  STATE = .velox/、PROFILES = profiles/、RESULTS = benchmarks/results/、LOGS = .velox/logs/
- 実用環境（VELOX_HOME 指定、または app/velox/... の親に manifest.json がある構成）:
  ROOT = VELOX_HOME（例: <VELOX_INSTALL_DIR>）
  STATE = config/、PROFILES = profiles/、AVD_DIR = avd/、USERDATA = userdata/、
  SCREENSHOTS = userdata/screenshots/、LOGS = logs/、BACKUPS = backups/、
  UPDATES = updates/、TOOLS = tools/、RUNTIME_DIR = runtime/
"""
from __future__ import annotations

import os
from pathlib import Path


def _detect_standalone_home() -> Path | None:
    env_home = os.environ.get("VELOX_HOME", "").strip()
    if env_home:
        return Path(env_home).resolve()
    # 例: <VELOX_INSTALL_DIR>\app\velox\core\paths.py -> parents[3] == <VELOX_INSTALL_DIR>
    pkg_parent = Path(__file__).resolve().parents[2]
    if pkg_parent.name == "app" and (pkg_parent.parent / "manifest.json").is_file():
        return pkg_parent.parent
    return None


_STANDALONE_HOME = _detect_standalone_home()
STANDALONE: bool = _STANDALONE_HOME is not None

if _STANDALONE_HOME is not None:
    ROOT = _STANDALONE_HOME
    STATE = Path(os.environ.get("VELOX_STATE", ROOT / "config"))
    PROFILES = ROOT / "profiles"
    RESULTS = ROOT / "userdata" / "benchmarks"
    DB = RESULTS / "results.sqlite"
    LOGS = ROOT / "logs"
    AVD_DIR = ROOT / "avd"
    USERDATA = ROOT / "userdata"
    SCREENSHOTS = USERDATA / "screenshots"
    BACKUPS = ROOT / "backups"
    UPDATES = ROOT / "updates"
    TOOLS = ROOT / "tools"
    RUNTIME_DIR = ROOT / "runtime"
    ANDROID_DIR = ROOT / "android"
else:
    ROOT = Path(__file__).resolve().parents[2]
    STATE = Path(os.environ.get("VELOX_STATE", ROOT / ".velox"))
    PROFILES = ROOT / "profiles"
    RESULTS = ROOT / "benchmarks" / "results"
    DB = RESULTS / "results.sqlite"
    LOGS = STATE / "logs"
    AVD_DIR = STATE / "avd"
    USERDATA = STATE / "userdata"
    SCREENSHOTS = USERDATA / "screenshots"
    BACKUPS = STATE / "backups"
    UPDATES = STATE / "updates"
    TOOLS = STATE / "tools"
    RUNTIME_DIR = STATE / "runtime"
    ANDROID_DIR = STATE / "android"


def ensure() -> None:
    dirs = [STATE, PROFILES, RESULTS, LOGS]
    if STANDALONE:
        dirs.extend([AVD_DIR, USERDATA, SCREENSHOTS, BACKUPS, UPDATES])
    for p in dirs:
        p.mkdir(parents=True, exist_ok=True)
