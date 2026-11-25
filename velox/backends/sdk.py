"""Android SDK の検出（emulator / adb / system-images）。"""
from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path

API_TO_ANDROID_VERSION: dict[int, str] = {
    28: "9", 29: "10", 30: "11", 31: "12", 32: "12L",
    33: "13", 34: "14", 35: "15", 36: "16", 37: "17",
}


def parse_api_level(api_str: str) -> int | None:
    """'android-31' / 'android-37.0' -> 31 / 37。"""
    m = re.match(r"android-(\d+)", str(api_str).strip())
    return int(m.group(1)) if m else None


def _props(path: Path) -> dict[str, str]:
    d: dict[str, str] = {}
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                d[k.strip()] = v.strip()
    except OSError:
        pass
    return d


@dataclass
class Sdk:
    root: Path

    @property
    def emulator(self) -> Path:
        return self.root / "emulator" / ("emulator.exe" if os.name == "nt" else "emulator")

    @property
    def adb(self) -> Path:
        return self.root / "platform-tools" / ("adb.exe" if os.name == "nt" else "adb")

    def emulator_version(self) -> str | None:
        return _props(self.root / "emulator" / "source.properties").get("Pkg.Revision")

    def platform_tools_version(self) -> str | None:
        return _props(self.root / "platform-tools" / "source.properties").get("Pkg.Revision")

    def system_images(self) -> list[dict]:
        """system-images/<api>/<tag>/<abi>/ を列挙する（API レベル昇順）。"""
        base = self.root / "system-images"
        out = []
        if not base.is_dir():
            return out
        for sp in base.glob("*/*/*/source.properties"):
            p = _props(sp)
            api, tag, abi = sp.parent.relative_to(base).parts
            api_num = parse_api_level(api)
            out.append({
                "path": str(sp.parent),
                "sysdir": f"system-images/{api}/{tag}/{abi}/",
                "api": api,
                "api_level": api_num,
                "android_version": API_TO_ANDROID_VERSION.get(api_num) if api_num is not None else None,
                "tag": tag,
                "abi": abi,
                "revision": p.get("Pkg.Revision"),
                "desc": p.get("Pkg.Desc"),
            })
        return sorted(out, key=lambda x: (x["api_level"] or 0, x["api"], x["tag"], x["abi"]))

    def find_image(self, *, sysdir: str | None = None, api_level: int | None = None,
                   abi: str | None = None, tag: str | None = None) -> dict | None:
        """条件に一致するインストール済みシステムイメージを選択する。"""
        imgs = self.system_images()
        if sysdir:
            norm = sysdir.replace("\\", "/").strip("/") + "/"
            for img in imgs:
                if img["sysdir"].replace("\\", "/").strip("/") + "/" == norm:
                    return img
            return None
        want_abi = abi or "x86_64"
        cands = [i for i in imgs if i["abi"] == want_abi]
        if api_level is not None:
            cands = [i for i in cands if i.get("api_level") == int(api_level)]
        if tag:
            cands = [i for i in cands if i["tag"] == tag]
        if not cands:
            return None
        # google_apis を優先し、最新リビジョン/API を返す
        cands.sort(key=lambda x: (x.get("api_level") or 0, x["tag"] == "google_apis", x["tag"]))
        return cands[-1]


def find_sdk() -> Sdk | None:
    cands = [os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT")]
    # スタンドアロン実用環境の明示的 SDK 依存宣言（android/sdk-dependency.json）も参照
    try:
        from velox.core import paths
        dep_file = paths.ANDROID_DIR / "sdk-dependency.json"
        if dep_file.is_file():
            import json
            dep = json.loads(dep_file.read_text(encoding="utf-8"))
            if dep.get("sdk_root"):
                cands.append(str(dep["sdk_root"]))
    except Exception:
        pass
    if os.name == "nt":
        cands.append(str(Path(os.environ.get("LOCALAPPDATA", "")) / "Android" / "Sdk"))
    else:
        cands.append(str(Path.home() / "Android" / "Sdk"))
    for c in cands:
        if c and ((Path(c) / "emulator").is_dir() or (Path(c) / "system-images").is_dir()):
            return Sdk(Path(c))
    return None


def avd_home() -> Path:
    if os.environ.get("ANDROID_AVD_HOME"):
        return Path(os.environ["ANDROID_AVD_HOME"])
    return Path(os.environ.get("ANDROID_USER_HOME", Path.home() / ".android")) / "avd"
