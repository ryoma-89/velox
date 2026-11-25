"""Velox Launcher APK ビルド＆永続署名スクリプト。

Android Studio 同梱 JDK + Android SDK build-tools (aapt2 / d8 / zipalign / apksigner) のみで
ゼロ外部依存・超軽量な Velox Launcher APK をビルドし、永続管理された鍵ストアで署名する。
（鍵ストアは一度生成したら再利用し、アップデート時にユーザーデータを失わない。）
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from velox.backends.sdk import find_sdk
from velox.core.util import now_iso

APP_SRC = REPO_ROOT / "android" / "velox-launcher"
OUT_DIR = REPO_ROOT / ".velox" / "apps"
BUILD_DIR = REPO_ROOT / ".velox" / "apps" / "_build_launcher"
KEYSTORE_PATH = Path(os.environ.get("VELOX_KEYSTORE_PATH") or (REPO_ROOT / ".velox" / "keys" / "velox-launcher.jks"))
KEY_ALIAS = os.environ.get("VELOX_KEY_ALIAS", "velox-launcher")
KEY_PASS = os.environ.get("VELOX_KEYSTORE_PASS", "velox-local-dev-keystore")


def find_jdk_bin() -> Path:
    cands = [
        Path(r"C:\Program Files\Android\Android Studio\jbr\bin"),
        Path(os.environ.get("JAVA_HOME", "")) / "bin",
    ]
    for c in cands:
        if (c / "javac.exe").is_file() and (c / "keytool.exe").is_file():
            return c
    raise RuntimeError("javac / keytool が見つかりません（Android Studio JBR または JDK を確認してください）")


def find_build_tools_and_platform() -> tuple[Path, Path]:
    sdk = find_sdk()
    if not sdk:
        raise RuntimeError("Android SDK が見つかりません")
    bt_root = sdk.root / "build-tools"
    bts = sorted([p for p in bt_root.iterdir() if p.is_dir() and (p / "aapt2.exe").is_file()], reverse=True)
    if not bts:
        raise RuntimeError(f"build-tools/aapt2.exe が見つかりません: {bt_root}")
    plat_root = sdk.root / "platforms"
    plats = sorted([p for p in plat_root.iterdir() if p.is_dir() and (p / "android.jar").is_file()], reverse=True)
    if not plats:
        raise RuntimeError(f"platforms/android.jar が見つかりません: {plat_root}")
    return bts[0], plats[0] / "android.jar"


def ensure_keystore(jdk_bin: Path) -> Path:
    KEYSTORE_PATH.parent.mkdir(parents=True, exist_ok=True)
    if KEYSTORE_PATH.is_file():
        return KEYSTORE_PATH
    subprocess.run(
        [
            str(jdk_bin / "keytool.exe"),
            "-genkeypair", "-v",
            "-keystore", str(KEYSTORE_PATH),
            "-storetype", "PKCS12",
            "-keyalg", "RSA",
            "-keysize", "2048",
            "-validity", "10000",
            "-alias", KEY_ALIAS,
            "-storepass", KEY_PASS,
            "-keypass", KEY_PASS,
            "-dname", "CN=Velox Launcher, OU=Velox, O=Velox Project, L=Local, ST=Local, C=JP",
        ],
        check=True,
    )
    return KEYSTORE_PATH


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_apk(out_apk: Path | None = None) -> dict:
    jdk_bin = find_jdk_bin()
    bt_dir, android_jar = find_build_tools_and_platform()
    keystore = ensure_keystore(jdk_bin)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    res_flat = BUILD_DIR / "res_flat"
    gen_src = BUILD_DIR / "gen"
    classes_dir = BUILD_DIR / "classes"
    dex_dir = BUILD_DIR / "dex"
    for d in (res_flat, gen_src, classes_dir, dex_dir):
        d.mkdir(parents=True, exist_ok=True)

    aapt2 = str(bt_dir / "aapt2.exe")
    d8 = str(bt_dir / "d8.bat")
    zipalign = str(bt_dir / "zipalign.exe")
    apksigner = str(bt_dir / "apksigner.bat")
    env = dict(os.environ)
    env["JAVA_HOME"] = str(jdk_bin.parent)
    env["PATH"] = str(jdk_bin) + os.pathsep + env.get("PATH", "")

    # 1. Compile Android resources
    res_zip = BUILD_DIR / "compiled_res.zip"
    subprocess.run(
        [aapt2, "compile", "--dir", str(APP_SRC / "res"), "-o", str(res_zip)],
        check=True, env=env,
    )

    # 2. Link resources + generate R.java + base APK
    unsigned_apk = BUILD_DIR / "unsigned.apk"
    subprocess.run(
        [
            aapt2, "link",
            "-o", str(unsigned_apk),
            "-I", str(android_jar),
            "--manifest", str(APP_SRC / "AndroidManifest.xml"),
            "--java", str(gen_src),
            "--min-sdk-version", "31",
            "--target-sdk-version", "36",
            str(res_zip),
        ],
        check=True, env=env,
    )

    # 3. Compile Java sources (target Java 11 bytecode for Android 12+ ART compatibility)
    java_files = sorted([str(p) for p in (APP_SRC / "src").rglob("*.java")]
                        + [str(p) for p in gen_src.rglob("*.java")])
    subprocess.run(
        [
            str(jdk_bin / "javac.exe"),
            "--release", "11",
            "-encoding", "UTF-8",
            "-classpath", str(android_jar),
            "-d", str(classes_dir),
            *java_files,
        ],
        check=True, env=env,
    )

    # 4. Convert .class -> classes.dex via d8
    class_files = sorted(str(p) for p in classes_dir.rglob("*.class"))
    subprocess.run(
        [
            d8,
            "--min-api", "31",
            "--lib", str(android_jar),
            "--output", str(dex_dir),
            *class_files,
        ],
        check=True, env=env,
    )

    # 5. Add classes.dex into unsigned APK
    import zipfile
    with zipfile.ZipFile(unsigned_apk, "a", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.write(dex_dir / "classes.dex", "classes.dex")

    # 6. Zipalign + apksigner (v2/v3 APK Signature Scheme)
    aligned_apk = BUILD_DIR / "aligned.apk"
    subprocess.run([zipalign, "-f", "4", str(unsigned_apk), str(aligned_apk)], check=True, env=env)

    final_apk = out_apk or (OUT_DIR / "velox-launcher.apk")
    subprocess.run(
        [
            apksigner, "sign",
            "--ks", str(keystore),
            "--ks-key-alias", KEY_ALIAS,
            "--ks-pass", f"pass:{KEY_PASS}",
            "--key-pass", f"pass:{KEY_PASS}",
            "--out", str(final_apk),
            str(aligned_apk),
        ],
        check=True, env=env,
    )
    subprocess.run([apksigner, "verify", "--verbose", str(final_apk)], check=True, env=env,
                   stdout=subprocess.DEVNULL)

    meta = {
        "package": "com.velox.launcher",
        "version_name": "0.2.0",
        "version_code": 2,
        "built_at": now_iso(),
        "apk_path": str(final_apk),
        "bytes": final_apk.stat().st_size,
        "sha256": sha256_file(final_apk),
        "keystore_sha256": sha256_file(keystore),
        "build_tools": bt_dir.name,
    }
    (OUT_DIR / "velox-launcher.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[Velox Launcher] ビルド＆署名完了: {final_apk} ({meta['bytes']} bytes, sha256={meta['sha256'][:16]}...)")
    return meta


if __name__ == "__main__":
    build_apk()
