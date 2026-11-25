"""ゲスト（Android x86_64）用の gfxstream Vulkan ドライバー vulkan.ranchu.so をソースからビルドする。

構成:
  1. goldfish 系の依存（renderControl エンコーダーなど）を NDK で静的ライブラリにする
     （scripts/guest/goldfish-deps）。
  2. その依存を pkg-config（scripts/guest/pkgconf_shim.py）経由で Mesa に渡し、
     Mesa の meson で gfxstream ドライバーだけを Android 向けにクロスビルドする。
  3. 生成物を .velox/guest-gfxstream/builds/<tag>/ にコピーし、ソースのリビジョンと
     差分、ハッシュを build-info.json に記録する。

例:
  .velox\\tools\\mesa-venv\\Scripts\\python.exe -m scripts.guest.build_guest_vulkan --tag base

meson と mako は .velox/tools/mesa-venv にある前提（docs/research/guest-gfxstream-build.md）。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AOSP = ROOT / "third_party/aosp/emu-main-win"
DEFAULT_NDK = ROOT / ".velox/sdk-toolchain/ndk/30.0.16248370"
DEFAULT_BUILD_ROOT = Path(os.environ.get("VELOX_GUEST_BUILD_ROOT") or (ROOT / ".velox/guest-gfxstream/work"))
OUT_ROOT = ROOT / ".velox/guest-gfxstream/builds"

MESA_OPTIONS = [
    "-Dplatforms=android",
    "-Dandroid-stub=true",
    "-Dandroid-libbacktrace=disabled",
    "-Dvulkan-drivers=gfxstream",
    "-Dgallium-drivers=",
    "-Dopengl=false",
    "-Degl=disabled",
    "-Dgles1=disabled",
    "-Dgles2=disabled",
    "-Dglx=disabled",
    "-Dgbm=disabled",
    "-Dllvm=disabled",
    "-Dzstd=disabled",
    "-Dexpat=disabled",
    "-Dxmlconfig=disabled",
    "-Dvalgrind=disabled",
    "-Dlibunwind=disabled",
    "-Dperfetto=false",
    "-Dvirtgpu_kumquat=false",
    "-Dbuild-tests=false",
    "-Dvulkan-layers=",
    "-Dtools=",
    "-Dallow-fallback-for=libdrm",
    "-Dlibdrm:default_library=static",
]


def run(cmd: list[str], cwd: Path | None = None, env: dict | None = None, log: Path | None = None) -> None:
    print("+", " ".join(str(c) for c in cmd), flush=True)
    if log is None:
        subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, check=True)
        return
    with open(log, "a", encoding="utf-8", errors="replace") as f:
        f.write("+ " + " ".join(str(c) for c in cmd) + "\n")
        f.flush()
        p = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if p.returncode != 0:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-60:]
        print("\n".join(tail))
        raise SystemExit(f"失敗: {cmd[0]} (終了コード {p.returncode})。ログ: {log}")


def posix(p: Path) -> str:
    return p.resolve().as_posix()


def write_cross_file(path: Path, ndk: Path, api: int, pkgconf: list[str], pc_dir: Path,
                     sanitize: bool = False) -> None:
    bin_dir = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin"
    target = f"--target=x86_64-linux-android{api}"
    cpp_driver_args = "" if sanitize else ", '-static-libstdc++'"
    # -static-libstdc++: NDK の libc++ を静的にリンクし、プラットフォームの libc++.so（std::__1）と混ぜない。
    text = f"""[binaries]
c = ['{posix(bin_dir / "clang.exe")}', '{target}']
cpp = ['{posix(bin_dir / "clang++.exe")}', '{target}'{cpp_driver_args}]
ar = '{posix(bin_dir / "llvm-ar.exe")}'
strip = '{posix(bin_dir / "llvm-strip.exe")}'
c_ld = 'lld'
cpp_ld = 'lld'
pkg-config = [{", ".join(repr(x) for x in pkgconf)}]

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '{posix(pc_dir)}'

[built-in options]
# -DANDROID: AOSP の Soong が全モジュールに付けるマクロ。gfxstream の AHardwareBuffer 対応などが
# `#if defined(ANDROID)` で囲まれているため、付けないと Android 専用の経路が黙って無効になる。
c_args = ['-DANDROID', '-fno-omit-frame-pointer']
cpp_args = ['-DANDROID', '-fno-omit-frame-pointer']
c_link_args = ['-Wl,--build-id=sha1']
cpp_link_args = ['-Wl,--build-id=sha1', '-static-libstdc++']

[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
"""
    path.write_text(text, encoding="utf-8")


def write_pc_files(pc_dir: Path, deps_build: Path, gfx: Path, mesa: Path) -> None:
    pc_dir.mkdir(parents=True, exist_ok=True)
    lib = f"-L{posix(deps_build)} -lvelox_goldfish"
    shim = ROOT / "scripts/guest/goldfish-deps/shim/include"
    pcs = {
        "android-base": ([shim], lib),
        "android-arect": ([], ""),
        # AOSP の libutils は libbase のヘッダーを再公開する。gfxstream_android が GetProperty を使う。
        "android-utils": ([shim], lib),
        "android-aemu-base": ([gfx / "guest/android-emu"], lib),
        "android-aemu-gl-codec": ([gfx / "guest/OpenglCodecCommon", gfx / "guest/OpenglCodecCommon/include",
                                   gfx / "third_party/opengl/include", gfx / "common/logging/include", shim], lib),
        "android-aemu-qemupipe": ([gfx / "guest/qemupipe/include", gfx / "guest/qemupipe/include-types"], lib),
        # Mesa 自身の aemu（src/gfxstream/aemu/ring_buffer.cpp）と同名の C 関数を持つため、
        # gfxstream 側の libringbuffer はリンクしない。
        "android-aemu-ringbuffer": ([], ""),
        "android-aemu-rc-encoder": ([gfx / "guest/renderControl_enc", shim], lib),
        "android-aemu-gralloc-headers": ([gfx / "guest/gralloc_cb/include"], ""),
        "android-nativebase-headers": ([mesa / "include/android_stub"], ""),
    }
    for name, (incs, libs) in pcs.items():
        cflags = " ".join(f"-I{posix(i)}" for i in incs)
        (pc_dir / f"{name}.pc").write_text(
            f"Name: {name}\nDescription: Velox goldfish dependency\nVersion: 1.0.0\n"
            f"Cflags: {cflags}\nLibs: {libs}\n",
            encoding="utf-8",
        )


def git_info(repo: Path) -> dict:
    def g(*args: str) -> str:
        return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True).stdout.strip()

    diff = subprocess.run(["git", "-C", str(repo), "diff", "HEAD"], capture_output=True).stdout
    return {
        "head": g("rev-parse", "HEAD"),
        "branch": g("rev-parse", "--abbrev-ref", "HEAD"),
        "log": g("log", "--oneline", "-8"),
        "dirty_diff_sha256": hashlib.sha256(diff).hexdigest() if diff else None,
    }


def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", required=True, help="出力名（例: base、asyncreset）")
    ap.add_argument("--ndk", type=Path, default=DEFAULT_NDK)
    ap.add_argument("--mesa", type=Path, default=AOSP / "external/mesa3d")
    ap.add_argument("--gfxstream", type=Path, default=AOSP / "hardware/google/gfxstream")
    ap.add_argument("--build-root", type=Path, default=DEFAULT_BUILD_ROOT,
                    help="中間生成物（Windows のパス長制限を避けるため短いパス）")
    ap.add_argument("--api", type=int, default=37)
    ap.add_argument("--buildtype", default="release", choices=["release", "debugoptimized", "debug"])
    ap.add_argument("--sanitize", choices=["address,undefined", "thread"],
                    help="検証専用。対応する NDK ランタイムを事前に読み込んで HAL を直接試験する")
    ap.add_argument("--reconfigure", action="store_true")
    ap.add_argument("-j", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    args = ap.parse_args()

    venv_bin = Path(sys.executable).parent
    meson = [sys.executable, "-m", "mesonbuild.mesonmain"]
    ninja = AOSP / "prebuilts/ninja/windows-x86/ninja.exe"
    if not ninja.exists():
        raise SystemExit(f"ninja が見つからない: {ninja}")
    try:
        import mako  # noqa: F401
        import yaml  # noqa: F401
    except ImportError:
        raise SystemExit("mako / PyYAML がない。.velox/tools/mesa-venv の python で実行する")

    br = args.build_root
    br.mkdir(parents=True, exist_ok=True)
    logs = br / "logs"
    logs.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = logs / f"build-{args.tag}-{stamp}.log"
    suffix = {None: "", "address,undefined": "-asan-ubsan", "thread": "-tsan"}[args.sanitize]
    pc_dir = br / f"pkgconfig{suffix}"
    cross = br / f"android-x86_64{suffix}.ini"
    pkgconf = [posix(Path(sys.executable)), posix(ROOT / "scripts/guest/pkgconf_shim.py")]
    write_cross_file(cross, args.ndk, args.api, pkgconf, pc_dir, sanitize=bool(args.sanitize))

    env = dict(os.environ)
    env["PATH"] = str(venv_bin) + os.pathsep + str(ninja.parent) + os.pathsep + env.get("PATH", "")
    env["PKG_CONFIG_LIBDIR"] = str(pc_dir)
    env.pop("PKG_CONFIG_PATH", None)

    flavor = f"{args.buildtype}{suffix}"
    meson_flags = [f"--buildtype={args.buildtype}"]
    if args.sanitize:
        meson_flags += [f"-Db_sanitize={args.sanitize}", "-Db_lundef=false"]

    # 1. goldfish 依存
    deps_build = br / f"deps-{flavor}"
    if args.reconfigure and deps_build.exists():
        shutil.rmtree(deps_build)
    if not (deps_build / "build.ninja").exists():
        run(meson + ["setup", str(deps_build), str(ROOT / "scripts/guest/goldfish-deps"),
                     "--cross-file", str(cross), *meson_flags,
                     f"-Dgfxstream_dir={posix(args.gfxstream)}", f"-Dmesa_dir={posix(args.mesa)}"],
            env=env, log=log)
    run([ninja, "-C", deps_build, f"-j{args.j}"], env=env, log=log)
    write_pc_files(pc_dir, deps_build, args.gfxstream, args.mesa)

    # 2. Mesa（gfxstream Vulkan のみ）
    mesa_build = br / f"mesa-{flavor}"
    if args.reconfigure and mesa_build.exists():
        shutil.rmtree(mesa_build)
    if not (mesa_build / "build.ninja").exists():
        run(meson + ["setup", str(mesa_build), str(args.mesa), "--cross-file", str(cross),
                     *meson_flags, f"-Dplatform-sdk-version={args.api}",
                     "-Db_ndebug=true"] + MESA_OPTIONS,
            env=env, log=log)
    run([ninja, "-C", mesa_build, f"-j{args.j}", "src/gfxstream/guest/vulkan/libvulkan_gfxstream.so"],
        env=env, log=log)

    so = mesa_build / "src/gfxstream/guest/vulkan/libvulkan_gfxstream.so"
    out = OUT_ROOT / args.tag
    out.mkdir(parents=True, exist_ok=True)
    unstripped = out / "vulkan.ranchu.unstripped.so"
    shutil.copy2(so, unstripped)
    stripped = out / "vulkan.ranchu.so"
    strip = args.ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
    run([strip, "--strip-unneeded", "-o", stripped, unstripped], log=log)
    info = {
        "tag": args.tag,
        "built_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "buildtype": args.buildtype,
        "sanitize": args.sanitize,
        "api": args.api,
        "ndk": str(args.ndk),
        "mesa": git_info(args.mesa),
        "gfxstream": git_info(args.gfxstream),
        "mesa_options": MESA_OPTIONS,
        "files": {
            "vulkan.ranchu.so": sha256(stripped),
            "vulkan.ranchu.unstripped.so": sha256(unstripped),
        },
        "log": str(log),
    }
    (out / "build-info.json").write_text(json.dumps(info, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"完了: {stripped}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
