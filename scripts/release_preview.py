"""Velox Preview リリースビルド＆実用環境パッケージ生成スクリプト。

開発環境（<VELOX_DEV_ROOT>）から完全に独立した配布パッケージを生成する。
明示的に実行した場合のみ動作し、開発中のビルドを実用環境へ自動反映しない。

使い方:
  # リリースパッケージの生成（SHA-256 マニフェスト付き）
  python scripts/release_preview.py --version preview-0.1

  # 新規インストール（例: <VELOX_INSTALL_DIR>）
  python scripts/release_preview.py --version preview-0.1 --install-dir <VELOX_INSTALL_DIR>
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from velox.backends.sdk import find_sdk
from velox.core.util import now_iso
from velox.optimize import recommend

DEFAULT_RUNTIME = REPO_ROOT / ".velox/emulator-builds/whpx20-short-release/runtime-phase20-emu-v1"
DEFAULT_HAL_A17 = REPO_ROOT / ".velox/guest-gfxstream/builds/phase18-vk14-v1/vulkan.ranchu.so"
DEFAULT_LAUNCHER_APK = REPO_ROOT / ".velox/apps/velox-launcher.apk"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def git_head(repo: Path) -> str | None:
    try:
        r = subprocess.run(
            ["git", "-c", "safe.directory=*", "-C", str(repo), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=10,
        )
        return r.stdout.strip() if r.returncode == 0 else None
    except Exception:
        return None


def build_launcher_exe(out_exe: Path, out_ico: Path) -> None:
    launcher_dir = REPO_ROOT / "launcher"
    subprocess.run([sys.executable, str(launcher_dir / "make_icon.py"), str(out_ico)], check=True)
    res_path = out_exe.parent / "_velox.res"
    # windres は rc 内の相対パスを参照するため launcher_dir で実行
    shutil.copy2(out_ico, launcher_dir / "velox.ico")
    subprocess.run(
        ["windres", str(launcher_dir / "velox.rc"), "-O", "coff", "-o", str(res_path)],
        cwd=str(launcher_dir), check=True,
    )
    subprocess.run(
        [
            "gcc", "-mwindows", "-municode", "-O2", "-Wall", "-Wextra", "-Werror",
            str(launcher_dir / "velox_launcher.c"), str(res_path), "-o", str(out_exe),
        ],
        check=True,
    )
    if res_path.exists():
        res_path.unlink()


def copy_standalone_python(dst_py: Path) -> None:
    src_py = Path(sys.base_prefix)
    dst_py.mkdir(parents=True, exist_ok=True)
    for fname in ("python.exe", "pythonw.exe", "python3.dll", "python312.dll",
                  "vcruntime140.dll", "vcruntime140_1.dll", "LICENSE.txt"):
        f = src_py / fname
        if f.is_file():
            shutil.copy2(f, dst_py / fname)
    for dname, ignores in (
        ("DLLs", ("*.pdb",)),
        ("tcl", ("*.pdb", "demos", "tzdata")),
        ("Lib", ("__pycache__", "test", "tests", "site-packages", "idlelib", "ensurepip", "turtledemo", "pydoc_data")),
    ):
        src_d = src_py / dname
        if src_d.is_dir():
            shutil.copytree(src_d, dst_py / dname, ignore=shutil.ignore_patterns(*ignores), dirs_exist_ok=True)


def copy_platform_tools(dst_pt: Path) -> None:
    sdk = find_sdk()
    if not sdk:
        raise RuntimeError("Android SDK platform-tools が見つかりません")
    src_pt = sdk.root / "platform-tools"
    dst_pt.mkdir(parents=True, exist_ok=True)
    for fname in ("adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll", "libwinpthread-1.dll", "NOTICE.txt", "source.properties"):
        f = src_pt / fname
        if f.is_file():
            shutil.copy2(f, dst_pt / fname)


def build_initial_profiles(version: str) -> dict[str, dict]:
    """ハードウェア自動検出＋第 19 段階実測に基づく実用版初期 4 プロファイルを生成する。"""
    host = recommend.detect_host()
    runtime_rel = "runtime/current"

    def make_prof(mode: str, *, width: int, height: int, density: int, refresh_hz: int,
                  fps_limit: int | None, ram_mb: int | None = None, highres_slack: int = 0) -> dict:
        rec = recommend.recommend_config(mode=mode, api_level=37)
        cfg = rec["config"]
        cfg["avd"]["name"] = "velox-a17-main"
        cfg["avd"]["home"] = "avd"
        cfg["avd"]["api_level"] = 37
        cfg["avd"]["abi"] = "x86_64"
        cfg["avd"]["tag"] = "google_apis"
        if ram_mb is not None:
            cfg["memory"]["ram_mb"] = ram_mb
        cfg["display"]["width"] = width
        cfg["display"]["height"] = height
        cfg["display"]["density"] = density
        cfg["display"]["refresh_hz"] = refresh_hz
        cfg["display"]["fps_limit"] = fps_limit
        cfg["display"]["frame_rate_mode"] = "standard" if fps_limit == 60 else "performance"
        cfg["boot"]["window"] = True
        cfg["boot"]["audio"] = True
        cfg["boot"]["snapshot"] = False  # D-037 ガード維持
        cfg["boot"]["kernel_params"] = "clocksource=tsc"
        cfg["gpu"]["mode"] = "host"
        cfg["gpu"]["transport"] = "asg"
        cfg["gpu"]["vulkan_1_4"] = False  # 既定は Vulkan 1.3.0
        cfg["gpu"]["disable_implicit_vk_layers"] = True
        cfg["gpu"]["features"] = ["VulkanBatchedDescriptorSetUpdate"]
        host_env = {
            "VELOX_ASG_SPIN_US": "50",
            "VELOX_RENDER_PRIORITY": "above_normal",
            "VELOX_VK_FLUSH_NO_COPY": "1",
            "VELOX_VK_CACHED_UPLOAD": "1",
            "VELOX_ASG_PREFETCH": "1",
            "VELOX_FENCE_SHARED": "1",
            "VELOX_DRIVE_AIO": "native",
            "VELOX_VIRTIO_BLK_QUEUES": "4",
            "VELOX_HIGHRES_POLL": "1",
            "VELOX_WHPX_PRERUN_FAST": "1",
            "VELOX_WIN32_AIO_POOL": "1",
        }
        if mode == "gaming":
            host_env["VELOX_ASG_ADAPTIVE_SPIN_US"] = "500"
            if highres_slack > 0:
                host_env["VELOX_HIGHRES_POLL_SLACK_US"] = str(highres_slack)
        elif mode == "compute":
            host_env["VELOX_ASG_ADAPTIVE_SPIN_US"] = "500"
        elif mode == "low_power":
            host_env.pop("VELOX_ASG_PREFETCH", None)
            host_env.pop("VELOX_HIGHRES_POLL", None)
            host_env["VELOX_ASG_SPIN_US"] = "20"
        cfg["gpu"]["host_env"] = host_env
        cfg["gpu"]["guest_props"] = {
            "debug.velox.cmd_stream_kb": "128",
            "debug.velox.cmd_stream_first_kb": "2",
            "debug.velox.fence_shared": "1",
            "debug.velox.ray_tracing": "1",
            "debug.velox.ray_tracing_pipeline": "1",
            "debug.velox.cooperative_matrix": "1",
        }
        cfg["emulator"]["dir"] = runtime_rel
        cfg["extra_args"] = ["-no-snapstorage", "-no-metrics"]
        return cfg

    return {
        "gaming": make_prof("gaming", width=1920, height=1080, density=320, refresh_hz=120, fps_limit=120,
                            ram_mb=min(16384, max(8192, (host.ram_mb // 4096) * 2048)), highres_slack=500),
        "compute": make_prof("compute", width=1920, height=1080, density=320, refresh_hz=120, fps_limit=120,
                             ram_mb=min(16384, max(8192, (host.ram_mb // 4096) * 2048))),
        "balanced": make_prof("balanced", width=1920, height=1080, density=320, refresh_hz=90, fps_limit=90,
                              ram_mb=min(12288, max(8192, (host.ram_mb // 4096) * 1024))),
        "low-power": make_prof("low_power", width=1280, height=720, density=240, refresh_hz=60, fps_limit=60,
                               ram_mb=6144),
    }


def make_readme(version: str, manifest: dict) -> str:
    return f"""# Velox {version}（実用版・スタンドアロン環境）

Windows 上で Android アプリやゲームを高速かつ安定して動作させるための独立した Velox 実用環境です。
開発ディレクトリ（`<VELOX_DEV_ROOT>`）には依存せず、本ディレクトリ内で完結して動作します。

## 起動方法

1. デスクトップまたはスタートメニューの **Velox** アイコンをダブルクリックします。
2. または本フォルダ直下の `Velox.exe` をダブルクリックします（コンソール画面は表示されません）。
3. GUI の「ホーム」画面で端末を選択し、**▶ 起動** をクリックしてください。

## ウィンドウ操作・ショートカット

- **マウス**: クリック／ドラッグでタッチ・スワイプ操作
- **キーボード**: そのまま Android 端末へ入力可能
- **画面サイズ変更**: Android ウィンドウの端をドラッグして自由に変更
- **フルスクリーン**: Android ウィンドウ選択中に `Alt + Enter`（またはウィンドウ上部ツールバー）
- **戻る / ホーム / 履歴 / 回転 / スクショ / クリップボード**: Velox GUI「ホーム」下部のクイック操作バーから実行
- **APK インストール**: GUI「アプリ管理」から選択、または `.apk` ファイルを Velox GUI ウィンドウへドラッグ＆ドロップ

## ディレクトリ構成

| パス | 用途 | アップデート時の扱い |
|---|---|---|
| `Velox.exe` | コンソール非表示 GUI ランチャー | 更新対象（更新前バックアップあり） |
| `app\\` | Velox GUI・バックエンド本体 | 更新対象（更新前バックアップあり） |
| `runtime\\{version}\\` | 検証済みエミュレーターランタイム | 新版は別フォルダへ配置し `runtime\\current` を切替 |
| `tools\\` | 同梱 Python 3.12 および `adb` | 必要時のみ更新 |
| `android\\` | 共有 SDK 依存情報・任意 HAL バックアップ | 保持 |
| `profiles\\` | 初期性能プロファイル（Gaming / Compute / Balanced / Low Power） | 既存変更を保護 |
| `avd\\` | 実用版専用 Android 端末（AVD） | **絶対に上書き・削除しない** |
| `userdata\\` | スクリーンショット等のユーザーデータ | **絶対に上書き・削除しない** |
| `config\\` | 端末設定・GUI 設定・変更履歴 | **絶対に上書き・削除しない** |
| `logs\\` | 実行ログ | 保持 |
| `backups\\` | 端末バックアップおよび更新前バックアップ | 保持 |
| `updates\\` | 更新履歴・適用済みパッケージ記録 | 保持 |

## 採用ランタイム・バージョン情報

- バージョン: `{version}`（作成日時: `{manifest['built_at']}`）
- ランタイム: `runtime-{version}`（Phase 20 `runtime-phase20-emu-v1`：gfxstream Patch 0031 Vulkan ホットパス最適化 + QEMU Patch 0018 WHPX ロックレス高速パス + Patch 0019 Win32 AIO プール + Patch 0016/0017 PC 入力・RDP・ゲームモード）
- 既定グラフィックス: Host GLES 3.1（Patch 0028+0030 適用済み）＋ Vulkan 1.3.0（既定；GuestAngle / Vulkan 1.4 は設定画面から任意選択）
- Snapshot 保護: D-037 ガード維持（既定コールドブート）
- 共有依存関係: 公式 Android SDK の `system-images`（Android 17 API 37 / Android 12 API 31、`android\\sdk-dependency.json` に明記）。開発作業による影響を受けません。

## アップデートとロールバック

実用環境への更新は自動では行われません。ユーザーが明示的に以下を実行した場合のみ適用されます（実行前に Velox を停止してください）。

```powershell
# アップデート適用（SHA-256 検証・更新前バックアップ・動作確認・失敗時自動ロールバック付き）
.\\tools\\python\\python.exe .\\app\\scripts\\update_preview.py apply --target . --package <配布パッケージディレクトリ>

# 直前バージョンへロールバック（AVD・アプリ・ユーザーデータは完全保持）
.\\tools\\python\\python.exe .\\app\\scripts\\update_preview.py rollback --target .
```

## ライセンスと利用上の注意

- 本環境はユーザー自身の PC 内で利用する個人用実用環境です。外部公開や第三者への再配布は行わないでください。
- Android システムイメージおよび Google APIs コンポーネントは Google Android SDK ライセンスに従います。
- QEMU / gfxstream / ANGLE / Python / platform-tools 等の各オープンソース・同梱バイナリのライセンスは各ディレクトリ内の `LICENSE` / `NOTICE.txt` を参照してください。
"""


def set_current_junction(runtime_root: Path, version_name: str) -> None:
    cur = runtime_root / "current"
    target = runtime_root / version_name
    if cur.exists() or cur.is_symlink():
        if os.name == "nt":
            subprocess.run(["cmd", "/c", "rmdir", str(cur)], check=False, capture_output=True)
            if cur.exists():
                shutil.rmtree(cur)
        else:
            if cur.is_symlink():
                cur.unlink()
            else:
                shutil.rmtree(cur)
    if os.name == "nt":
        r = subprocess.run(["cmd", "/c", "mklink", "/J", str(cur), str(target)], capture_output=True, text=True)
        if r.returncode != 0:
            shutil.copytree(target, cur)
    else:
        cur.symlink_to(target, target_is_directory=True)


def build_package(version: str, runtime_src: Path, pkg_dir: Path) -> dict:
    if not runtime_src.is_dir():
        raise FileNotFoundError(f"ランタイムディレクトリが見つかりません: {runtime_src}")
    if pkg_dir.exists():
        shutil.rmtree(pkg_dir)
    pkg_dir.mkdir(parents=True, exist_ok=True)

    print(f"[1/6] ネイティブ GUI ランチャー Velox.exe をビルド中...")
    build_launcher_exe(pkg_dir / "Velox.exe", pkg_dir / "velox.ico")

    print(f"[2/6] アプリケーション本体 (app/velox, app/scripts) をコピー中...")
    shutil.copytree(
        REPO_ROOT / "velox",
        pkg_dir / "app" / "velox",
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
    )
    (pkg_dir / "app" / "scripts").mkdir(parents=True, exist_ok=True)
    shutil.copy2(REPO_ROOT / "scripts" / "update_preview.py", pkg_dir / "app" / "scripts" / "update_preview.py")
    if DEFAULT_LAUNCHER_APK.is_file():
        (pkg_dir / "apps").mkdir(parents=True, exist_ok=True)
        shutil.copy2(DEFAULT_LAUNCHER_APK, pkg_dir / "apps" / "velox-launcher.apk")
        meta_j = DEFAULT_LAUNCHER_APK.with_suffix(".json")
        if meta_j.is_file():
            shutil.copy2(meta_j, pkg_dir / "apps" / "velox-launcher.json")

    print(f"[3/6] 検証済みランタイム ({runtime_src.name}) を配置中...")
    rt_dst = pkg_dir / "runtime" / version
    shutil.copytree(runtime_src, rt_dst, ignore=shutil.ignore_patterns("*.pdb"))

    print(f"[4/6] スタンドアロン Python 3.12 および platform-tools を配置中...")
    copy_standalone_python(pkg_dir / "tools" / "python")
    copy_platform_tools(pkg_dir / "tools" / "platform-tools")

    print(f"[5/6] Android 依存情報・HAL・初期 4 プロファイルを生成中...")
    sdk = find_sdk()
    sdk_imgs = sdk.system_images() if sdk else []
    sdk_dep = {
        "note": "Android 公式システムイメージのみ共有 SDK を参照します。開発リポジトリには依存しません。",
        "sdk_root": str(sdk.root) if sdk else None,
        "system_images": [
            {
                "api_level": i.get("api_level"),
                "android_version": i.get("android_version"),
                "tag": i.get("tag"),
                "abi": i.get("abi"),
                "sysdir": i.get("sysdir"),
                "revision": i.get("revision"),
            }
            for i in sdk_imgs
            if i.get("api_level") in (31, 37)
        ],
    }
    (pkg_dir / "android" / "api31").mkdir(parents=True, exist_ok=True)
    (pkg_dir / "android" / "api37").mkdir(parents=True, exist_ok=True)
    (pkg_dir / "android" / "sdk-dependency.json").write_text(
        json.dumps(sdk_dep, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    hal_info = None
    if DEFAULT_HAL_A17.is_file():
        hal_dst = pkg_dir / "android" / "api37" / "hal-phase18-vk14-v1" / "vulkan.ranchu.so"
        hal_dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(DEFAULT_HAL_A17, hal_dst)
        hal_info = {
            "name": "phase18-vk14-v1",
            "api_level": 37,
            "path": "android/api37/hal-phase18-vk14-v1/vulkan.ranchu.so",
            "sha256": sha256_file(hal_dst),
        }

    prof_dir = pkg_dir / "profiles"
    prof_dir.mkdir(parents=True, exist_ok=True)
    for pname, pcfg in build_initial_profiles(version).items():
        (prof_dir / f"{pname}.json").write_text(json.dumps(pcfg, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"[6/6] マニフェストと SHA-256 を生成中...")
    gfx_repo = REPO_ROOT / "third_party/aosp/emu-main-win/hardware/google/gfxstream"
    mesa_repo = REPO_ROOT / "third_party/aosp/emu-main-win/external/mesa3d"
    dll_path = rt_dst / "lib64" / "libgfxstream_backend.dll"
    qemu_gui = rt_dst / "qemu" / "windows-x86_64" / "qemu-system-x86_64.exe"
    qemu_hl = rt_dst / "qemu" / "windows-x86_64" / "qemu-system-x86_64-headless.exe"

    manifest = {
        "product": f"Velox {version}",
        "version": version,
        "built_at": now_iso(),
        "active_runtime": version,
        "git_commits": {
            "velox": git_head(REPO_ROOT),
            "gfxstream": git_head(gfx_repo),
            "mesa3d": git_head(mesa_repo),
        },
        "patches": {
            "gfxstream": [
                "0022-RingStream-prefetch-lifecycle",
                "0023-VeloxFenceWatcher-lock-order",
                "0024-vk_decoder-seqno-tiered-wait",
                "0025-RingStream-adaptive-spin",
                "0026-DeviceOpTracker-reset-fastpath",
                "0027-ReadBuffer-RingStream-SubAllocator-snapshot",
                "0028-GLES-zero-copy-lazy-buffer-shader-cache",
                "0029-postLoadBindContext-fallback-GuestSyncs-stale",
                "0030-harden-InputBuffer-align4-shader-key-GuestSyncs-delete",
                "0031-velox-vulkan-hotpath-lock-alloc-and-subdecode-layout",
            ],
            "qemu": [
                "0001..0015 (WHPX 20vCPU, P/E placement, durable aio/mq, highres waitable timer + slack)",
                "0016-velox-allow-synthesized-mouse-without-active-touch-pen",
                "0017-velox-pc-shortcuts-clipboard-toggle-and-gamemode-keymapper",
                "0018-velox-whpx-lockless-prerun-and-halt",
                "0019-velox-win32-aio-bounce-buffer-pool",
            ],
            "aemu": ["0002-asg-v2-backing", "0003-SubAllocator-load-fix"],
            "mesa3d": ["0028..0030 (phase18-vk14-v1 optional API 37 HAL)"],
        },
        "defaults": {
            "vulkan_api": "1.3.0",
            "snapshot_d037_guard": True,
            "gles_backend": "Host GLES",
        },
        "critical_sha256": {
            "Velox.exe": sha256_file(pkg_dir / "Velox.exe"),
            "libgfxstream_backend.dll": sha256_file(dll_path) if dll_path.is_file() else None,
            "qemu-system-x86_64.exe": sha256_file(qemu_gui) if qemu_gui.is_file() else None,
            "qemu-system-x86_64-headless.exe": sha256_file(qemu_hl) if qemu_hl.is_file() else None,
            "adb.exe": sha256_file(pkg_dir / "tools/platform-tools/adb.exe"),
            "velox-launcher.apk": sha256_file(pkg_dir / "apps/velox-launcher.apk") if (pkg_dir / "apps/velox-launcher.apk").is_file() else None,
        },
        "optional_hal": hal_info,
        "sdk_dependency": sdk_dep,
    }
    (pkg_dir / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    (pkg_dir / "README.md").write_text(make_readme(version, manifest), encoding="utf-8")

    # リリース検証用 SHA-256 マニフェスト（全配布ファイル対象）
    file_hashes: dict[str, str] = {}
    for f in sorted(pkg_dir.rglob("*")):
        if f.is_file() and f.name != "release-manifest.json":
            file_hashes[f.relative_to(pkg_dir).as_posix()] = sha256_file(f)
    rel_man = {
        "schema": 1,
        "version": version,
        "built_at": manifest["built_at"],
        "file_count": len(file_hashes),
        "files_sha256": file_hashes,
    }
    (pkg_dir / "release-manifest.json").write_text(json.dumps(rel_man, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"リリース生成完了: {pkg_dir} ({len(file_hashes)} files)")
    return manifest


def install_fresh(pkg_dir: Path, install_dir: Path, version: str) -> None:
    install_dir.mkdir(parents=True, exist_ok=True)
    for sub in ("avd", "userdata/screenshots", "config", "logs", "backups", "updates"):
        (install_dir / sub).mkdir(parents=True, exist_ok=True)

    # 初回コピー（既存の avd / userdata / config は決して上書きしない）
    for item in pkg_dir.iterdir():
        if item.name == "release-manifest.json":
            continue
        dst = install_dir / item.name
        if item.is_dir():
            shutil.copytree(item, dst, dirs_exist_ok=True)
        else:
            shutil.copy2(item, dst)

    set_current_junction(install_dir / "runtime", version)
    # 配布パッケージのメタデータを updates/ に記録
    shutil.copy2(pkg_dir / "release-manifest.json", install_dir / "updates" / f"installed-{version}.json")
    print(f"インストール完了: {install_dir}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Velox Preview リリースパッケージ生成")
    ap.add_argument("--version", default="preview-0.1")
    ap.add_argument("--runtime-dir", type=Path, default=DEFAULT_RUNTIME)
    ap.add_argument("--output-dir", type=Path, default=None)
    ap.add_argument("--install-dir", type=Path, default=None)
    args = ap.parse_args()

    pkg_dir = args.output_dir or (REPO_ROOT / ".velox" / "releases" / f"velox-{args.version}")
    build_package(args.version, args.runtime_dir, pkg_dir)
    if args.install_dir:
        install_fresh(pkg_dir, args.install_dir, args.version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
