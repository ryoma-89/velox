"""AOSP Android Emulator（QEMU 系 + WHPX + gfxstream）バックエンド。

設計: バックエンドは「プロファイル → AVD config.ini + 起動引数 + ホスト側
スケジューリング設定」への変換と、起動・停止・状態取得だけを担う。
GUI と CLI はどちらもこのモジュール（velox.core.service 経由）を使う。
"""
from __future__ import annotations

import json
import os
import time
from pathlib import Path

from velox.backends import frame_rate
from velox.backends.adb import Adb, AdbError
from velox.backends.sdk import Sdk, avd_home, find_sdk, parse_api_level
from velox.core import paths
from velox.core.util import now_iso, run
from velox.hw import cpu_topology, process

VM_PROCESS_NAMES = ("qemu-system-x86_64.exe", "qemu-system-x86_64-headless.exe", "qemu-system-x86_64")


class BackendError(RuntimeError):
    pass


def resolve_affinity(spec: str) -> int | None:
    topo = cpu_topology.detect()
    if spec in ("auto", "PE"):
        return topo.mask("PE") if spec == "PE" else None
    if spec in ("P", "E"):
        return topo.mask(spec)
    if spec.startswith("0x"):
        return int(spec, 16)
    raise BackendError(f"不明な affinity 指定: {spec}")


# cpu.thread_placement の役割 → ホスト側（gfxstream 0019、QEMU 0010）が読む環境変数
PLACEMENT_ENV = {
    "vcpu": "VELOX_VCPU_AFFINITY",            # QEMU の vCPU スレッド（"CPU n/WHPX"）
    "render": "VELOX_RENDER_AFFINITY",        # gfxstream の RenderThread（解読とドライバーへの記録）
    "prefetch": "VELOX_PREFETCH_AFFINITY",    # ASG 受信の先読みスレッド
    "qemu_other": "VELOX_QEMU_THREAD_AFFINITY",  # QEMU のその他のスレッド（I/O、スレッドプール）
}


def resolve_placement(spec: str) -> int:
    """配置の指定をマスクにする。カンマ区切りの項の和で、"-" で始まる項は取り除く。

    項: "P" | "E" | "PE" | "0x..." | "P#n" / "E#n"（EfficiencyClass で分けた n 番目のコア、0 始まり）。
    例: "PE,-P#0,-P#1"（先頭 2 つの P コアを除く全コア）。番号はトポロジーの検出順で、決め打ちしない。
    """
    topo = cpu_topology.detect()
    mask = 0
    for term in (t.strip() for t in spec.split(",")):
        if not term:
            continue
        neg = term.startswith("-")
        term = term[1:] if neg else term
        if "#" in term:
            kind, _, idx = term.partition("#")
            cores = topo.p_cores if kind == "P" else topo.e_cores if kind == "E" else None
            if cores is None or not idx.isdigit() or int(idx) >= len(cores):
                raise BackendError(f"配置の項を解決できません: {term}")
            m = 1 << cores[int(idx)].index
        else:
            m = resolve_affinity(term) or 0
        mask = mask & ~m if neg else mask | m
    return mask


def placement_env(placement: dict) -> dict:
    """役割ごとの配置（"P" | "E" | "PE" | "0x..."）を、EfficiencyClass から求めたマスクに変換する。

    CPU 番号は決め打ちしない（cpu_topology.detect() が GetSystemCpuSetInformation で判別する）。
    "auto" または未指定の役割は環境変数を出さない（OS のスケジューラーに任せる）。
    """
    env = {}
    for role, spec in placement.items():
        if role not in PLACEMENT_ENV:
            raise BackendError(f"cpu.thread_placement の役割が不明です: {role}")
        if spec in (None, "", "auto"):
            continue
        mask = resolve_placement(spec)
        if not mask:
            raise BackendError(f"cpu.thread_placement.{role} を解決できません: {spec}")
        env[PLACEMENT_ENV[role]] = hex(mask)
    return env


class AospEmulator:
    def __init__(self, sdk: Sdk | None = None):
        self.sdk = sdk or find_sdk()
        if not self.sdk:
            raise BackendError("Android SDK が見つかりません")
        self.runtime_file = paths.STATE / "runtime.json"

    # ---- AVD -------------------------------------------------------------
    @staticmethod
    def avd_root(cfg: dict | None = None) -> Path:
        home = ((cfg or {}).get("avd") or {}).get("home")
        if home:
            p = Path(home)
            return p if p.is_absolute() else paths.ROOT / p
        return avd_home()

    def avd_dir(self, name: str, cfg: dict | None = None) -> Path:
        return self.avd_root(cfg) / f"{name}.avd"

    def pick_sysdir(self, cfg: dict) -> str:
        avd = cfg["avd"]
        img = self.sdk.find_image(
            sysdir=avd.get("sysdir"),
            api_level=avd.get("api_level"),
            abi=avd.get("abi"),
            tag=avd.get("tag"),
        )
        if img is not None:
            return img["sysdir"]
        if avd.get("sysdir"):
            # 明示 sysdir が SDK 内に見つからない場合は既存挙動互換で検証エラーを出す
            target = self.sdk.root / avd["sysdir"].replace("/", os.sep)
            if target.is_dir():
                return avd["sysdir"]
            raise BackendError(f"指定されたシステムイメージが見つかりません: {avd['sysdir']}")
        avail = [f"{i['api']}/{i['tag']}/{i['abi']}" for i in self.sdk.system_images()]
        raise BackendError(
            f"条件に一致するシステムイメージがありません (api_level={avd.get('api_level')}, "
            f"abi={avd.get('abi') or 'x86_64'}, tag={avd.get('tag')})。導入済み: {avail}"
        )

    @staticmethod
    def check_api_compatibility(cfg: dict, sysdir: str) -> int | None:
        """選択された Android API レベルと構成の互換性を検査し、未対応構成を安全に拒否する。"""
        parts = sysdir.replace("\\", "/").strip("/").split("/")
        api_num = parse_api_level(parts[1]) if len(parts) >= 2 else None
        if api_num is None:
            return None
        feats = cfg.get("gpu", {}).get("features") or []
        if api_num < 35 and cfg.get("gpu", {}).get("vulkan_1_4"):
            raise BackendError(
                f"API {api_num} は gpu.vulkan_1_4=true に対応していません（API 35 以上が必要）"
            )
        if api_num < 34 and "GuestAngle" in feats:
            raise BackendError(
                f"API {api_num} は GuestAngle に対応していません（API 34 以上が必要）"
            )
        return api_num

    def write_avd(self, cfg: dict) -> Path:
        """プロファイルから AVD を生成・更新する（userdata は保持）。"""
        name = cfg["avd"]["name"]
        sysdir = self.pick_sysdir(cfg)
        self.check_api_compatibility(cfg, sysdir)
        parts = sysdir.replace("\\", "/").strip("/").split("/")
        api, tag = parts[1], parts[2]
        abi = parts[3] if len(parts) >= 4 else (cfg["avd"].get("abi") or "x86_64")
        cpu_arch = "arm64" if abi == "arm64-v8a" else abi
        d = self.avd_dir(name, cfg)
        d.mkdir(parents=True, exist_ok=True)
        (self.avd_root(cfg) / f"{name}.ini").write_text(
            f"avd.ini.encoding=UTF-8\npath={d}\npath.rel=avd/{name}.avd\ntarget={api}\n", encoding="utf-8")
        c, m, g, disp = cfg["cpu"], cfg["memory"], cfg["gpu"], cfg["display"]
        ini = {
            "AvdId": name, "avd.ini.displayname": name, "avd.ini.encoding": "UTF-8",
            "abi.type": abi, "hw.cpu.arch": cpu_arch, "tag.id": tag,
            "image.sysdir.1": sysdir.replace("/", os.sep),
            "PlayStore.enabled": "yes" if "playstore" in tag else "no",
            "hw.cpu.ncore": c["cores"], "hw.ramSize": m["ram_mb"], "vm.heapSize": m["vm_heap_mb"],
            "disk.dataPartition.size": f"{m['data_partition_gb']}G",
            "hw.gpu.enabled": "yes", "hw.gpu.mode": g["mode"],
            "hw.lcd.width": disp["width"], "hw.lcd.height": disp["height"], "hw.lcd.density": disp["density"],
            "hw.mainKeys": "no",
            "hw.keyboard": "yes", "hw.audioInput": "yes" if cfg["boot"]["audio"] else "no",
            "hw.audioOutput": "yes" if cfg["boot"]["audio"] else "no",
            "fastboot.forceColdBoot": "no" if cfg["boot"]["snapshot"] else "yes",
            "showDeviceFrame": "no",
            "hw.gltransport": g.get("transport", "pipe"),
        }
        kp = (cfg["boot"].get("kernel_params") or "").strip()
        if kp:
            ini["kernel.parameters"] = kp
        (d / "config.ini").write_text("".join(f"{k}={v}\n" for k, v in ini.items()), encoding="utf-8")
        return d

    # ---- 起動引数 --------------------------------------------------------
    def build_command(self, cfg: dict, port: int = 5554) -> list[str]:
        c, g, b = cfg["cpu"], cfg["gpu"], cfg["boot"]
        cmd = [str(self.emulator_exe(cfg)), "-avd", cfg["avd"]["name"], "-port", str(port),
               "-accel", "on", "-gpu", g["mode"], "-cores", str(c["cores"]),
               "-memory", str(cfg["memory"]["ram_mb"]), "-no-boot-anim",
               "-vsync-rate", str(cfg["display"]["refresh_hz"])]
        if not b["snapshot"]:
            cmd += ["-no-snapshot"]
        if not b["window"]:
            cmd += ["-no-window"]
        if not b["audio"]:
            cmd += ["-no-audio"]
        if g["features"]:
            cmd += ["-feature", ",".join(g["features"])]
        extra = list(cfg.get("extra_args", []))
        qemu_args: list[str] = []
        if "-qemu" in extra:  # -qemu 以降はすべて QEMU に渡るので末尾に集約する
            i = extra.index("-qemu")
            extra, qemu_args = extra[:i], extra[i + 1:]
        # 公式ビルドは vCPU を 6 に制限する（KNOWN_ISSUES KI-9）。smp_override 時は QEMU に直接渡す
        if c.get("smp_override") and c["cores"] > 6 and "-smp" not in qemu_args:
            qemu_args += ["-smp", str(c["cores"])]
        cmd += extra
        if qemu_args:
            cmd += ["-qemu"] + qemu_args
        return cmd

    def emulator_exe(self, cfg: dict) -> Path:
        exe_name = "emulator.exe" if os.name == "nt" else "emulator"
        d = (cfg.get("emulator") or {}).get("dir")
        if not d:
            if paths.STANDALONE:
                bundled = paths.RUNTIME_DIR / "current" / exe_name
                if bundled.exists():
                    return bundled
            return self.sdk.emulator
        p = Path(d)
        if not p.is_absolute():
            p = paths.ROOT / p
        exe = p / exe_name
        if not exe.exists():
            raise BackendError(f"emulator.dir に実行ファイルがありません: {exe}")
        return exe

    def check_patch_requirements(self, cfg: dict) -> dict | None:
        """パッチ版エミュレーターの前提条件（例: GuestAngle 必須）を検査する。"""
        d = (cfg.get("emulator") or {}).get("dir")
        if not d:
            return None
        p = Path(d)
        if not p.is_absolute():
            p = paths.ROOT / p
        man = p / "VELOX_PATCH.json"
        if not man.exists():
            return None
        info = json.loads(man.read_text(encoding="utf-8"))
        req = info.get("requires_feature")
        if req and req not in cfg["gpu"].get("features", []):
            raise BackendError(f"このエミュレーター（{d}）は gpu.features に {req} が必要です "
                               f"（宣言値と実ドライバ能力の不一致を防ぐため）")
        return info

    def build_env(self, cfg: dict) -> dict:
        env = dict(os.environ)
        # SDK 外のエミュレーターでもシステムイメージを見つけられるようにする
        env.setdefault("ANDROID_SDK_ROOT", str(self.sdk.root))
        env.setdefault("ANDROID_HOME", str(self.sdk.root))
        if (cfg.get("avd") or {}).get("home"):
            env["ANDROID_AVD_HOME"] = str(self.avd_root(cfg))
        if cfg["gpu"].get("disable_implicit_vk_layers"):
            # Vulkan Loader 1.3.234+ : 暗黙レイヤー（OBS/RTSS/Steam overlay 等）を読み込まない
            env["VK_LOADER_LAYERS_DISABLE"] = "~implicit~"
            env["DISABLE_RTSS_LAYER"] = "1"
        if cfg["gpu"].get("vulkan_1_4"):
            # opengles.cpp: 既定は guest Vulkan 1.3 上限（b/512021931）。正規の環境変数で 1.4 を許可
            env["ANDROID_EMU_VK_ENABLE_1_4"] = "1"
        for key, value in (cfg["gpu"].get("host_env") or {}).items():
            if not key.startswith("VELOX_") or not str(value).isprintable():
                raise BackendError(f"gpu.host_env には VELOX_ で始まる名前だけを指定できます: {key}")
            env[key] = str(value)
        # Preview 0.2: フォーカス中ウィンドウのホーム復帰キーおよびゲームモード 1:1 キーマップ設定先を既定連携
        env.setdefault("VELOX_HOME_KEY", "F12")
        env.setdefault("VELOX_KEYMAP_FILE", str(paths.STATE / "active_keymap.json"))
        # 第 13 段階: 受信の先読み（gfxstream 0018）のスナップショットの保存・復元の経路は実機で
        # 検証していない。スナップショットを使い得る構成では先読みを使わず、従来の受信経路にする。
        snapshot_capable = cfg["boot"].get("snapshot") or "-no-snapstorage" not in (cfg.get("extra_args") or [])
        # VELOX_ASG_PREFETCH_SNAPSHOT_TEST=1 は、その経路を検証する試験（第 14 段階）だけが使う。
        if (snapshot_capable and env.get("VELOX_ASG_PREFETCH") == "1"
                and env.get("VELOX_ASG_PREFETCH_SNAPSHOT_TEST") != "1"):
            env.pop("VELOX_ASG_PREFETCH")
        env.update(placement_env(cfg["cpu"].get("thread_placement") or {}))
        if cfg["gpu"].get("host_validation"):
            # gfxstream の正規の切替。ホストの Validation Layer はランタイムに同梱されている
            env["ANDROID_EMU_VVL_BEHAVIOR"] = cfg["gpu"]["host_validation"]
        feats = cfg["gpu"].get("gfxstream_features") or []
        if feats:
            # emulator -help-feature: ANDROID_EMULATOR_FEATURES でも feature を上書きできる（gfxstream も参照する）
            env["ANDROID_EMULATOR_FEATURES"] = ",".join(feats)
        return env

    # ---- ライフサイクル --------------------------------------------------
    def runtime(self) -> dict | None:
        if self.runtime_file.exists():
            return json.loads(self.runtime_file.read_text(encoding="utf-8"))
        return None

    def vm_pids(self) -> list[int]:
        return process.find_processes(VM_PROCESS_NAMES)

    def start(self, cfg: dict, wait: bool = True, timeout: float = 420) -> dict:
        if self.vm_pids():
            raise BackendError("エミュレーターは既に起動しています（emu stop で停止）")
        paths.ensure()
        patch = self.check_patch_requirements(cfg)
        self.write_avd(cfg)
        port = 5554
        serial = f"emulator-{port}"
        cmd = self.build_command(cfg, port)
        affinity = resolve_affinity(cfg["cpu"]["affinity"])
        log_path = paths.LOGS / f"emulator-{time.strftime('%Y%m%d-%H%M%S')}.log"
        log = open(log_path, "wb")
        t0 = time.monotonic()
        proc = process.popen(cmd, affinity=affinity, priority=cfg["cpu"]["priority"],
                             env=self.build_env(cfg), log=log)
        info = {"pid": proc.pid, "serial": serial, "port": port, "started_at": now_iso(),
                "command": cmd, "affinity": hex(affinity) if affinity else None, "log": str(log_path),
                "emulator_patch": patch, "profile": cfg}
        self.runtime_file.write_text(json.dumps(info, ensure_ascii=False, indent=2), encoding="utf-8")
        # VM 本体（qemu-system-*）に EcoQoS 無効化などを適用
        tuned = []
        for _ in range(60):
            kids = [p for p, n in process.descendants(proc.pid) if n.lower() in VM_PROCESS_NAMES]
            if kids:
                for pid in kids:
                    tuned.append(process.tune_process(pid, affinity=affinity,
                                                      disable_throttling=cfg["cpu"]["disable_power_throttling"]))
                break
            if proc.poll() is not None:
                raise BackendError(f"エミュレーターが即終了しました (code={proc.returncode})。ログ: {log_path}")
            time.sleep(0.5)
        info["tuned"] = tuned
        if wait:
            try:
                info["boot_seconds"] = round(Adb(serial).wait_boot(timeout) + 0, 2)
                info["boot_seconds_total"] = round(time.monotonic() - t0, 2)
            except AdbError as e:
                info["boot_error"] = str(e)
            else:
                # debug.* と表示設定は前回の起動や別プロファイルから残り得るので、毎回この設定で上書きする
                try:
                    info["frame_rate"] = frame_rate.apply(Adb(serial), cfg["display"])
                except AdbError as e:
                    info["frame_rate"] = {"applied": False, "reason": str(e)}
                info["guest_props"] = apply_guest_props(Adb(serial), cfg["gpu"].get("guest_props") or {})
                if cfg["memory"].get("guest_thp"):
                    try:
                        info["guest_thp"] = apply_guest_thp(Adb(serial), cfg["memory"]["guest_thp"])
                    except AdbError as e:
                        info["guest_thp"] = {"applied": False, "mode": cfg["memory"]["guest_thp"], "reason": str(e)}
        self.runtime_file.write_text(json.dumps(info, ensure_ascii=False, indent=2), encoding="utf-8")
        return info

    def stop(self, timeout: float = 60) -> dict:
        rt = self.runtime() or {}
        serial = rt.get("serial", "emulator-5554")
        res = {"serial": serial}
        # `emu kill` は VM をすぐに止めるので、ゲストのページキャッシュにある書き込み（adb push した
        # ファイルなど）が失われることがある（KI-33）。先にゲストのファイルシステムを同期する。
        try:
            synced = Adb(serial).shell("sync", timeout=30)
            res["guest_sync"] = synced.ok
        except AdbError as e:
            res["guest_sync_error"] = str(e)
        try:
            res["kill"] = Adb(serial).cmd("emu", "kill", timeout=20).out.strip()
        except AdbError as e:
            res["kill_error"] = str(e)
        t0 = time.monotonic()
        while self.vm_pids() and time.monotonic() - t0 < timeout:
            time.sleep(1)
        left = self.vm_pids()
        if left:  # adb 経由で止まらない場合のみ強制終了
            for pid in left:
                run(["taskkill", "/PID", str(pid), "/T", "/F"], timeout=20)
            res["forced"] = left
        if self.runtime_file.exists():
            self.runtime_file.unlink()
        res["stopped"] = not self.vm_pids()
        return res

    def status(self, *, light: bool = False) -> dict:
        pids = self.vm_pids()
        rt = self.runtime() or {}
        st = {"running": bool(pids), "vm_pids": pids, "runtime": rt}
        if not light and pids and rt.get("serial"):
            try:
                adb = Adb(rt["serial"])
                st["boot_completed"] = adb.boot_completed()
                if st["boot_completed"]:
                    st["guest"] = guest_info(adb)
                    st["frame_rate"] = {"configured": frame_rate.desired(rt.get("profile", {}).get("display", {})
                                                                        or {"refresh_hz": 60}),
                                        "observed": frame_rate.observe(adb),
                                        "applied_at_start": rt.get("frame_rate", {}).get("applied")}
            except AdbError as e:
                st["adb_error"] = str(e)
        return st


def apply_guest_props(adb: Adb, props: dict) -> dict:
    """gpu.guest_props をゲストに設定し、読み戻した値を返す。debug.velox.* 以外は受け付けない。"""
    result = {}
    for key, value in props.items():
        value = str(value)
        if not key.startswith("debug.velox.") or not value.replace(".", "").replace("-", "").isalnum():
            raise BackendError(f"gpu.guest_props に設定できない値です: {key}={value}")
        adb.shell(f"setprop {key} {value}", timeout=10)
        result[key] = adb.getprop(key)
    return result


GUEST_THP_MODES: dict[str, tuple[str, str]] = {
    "madvise": ("madvise", "madvise"),
    "always_nodefrag": ("always", "never"),
    "always_defer": ("always", "defer+madvise"),
}


def apply_guest_thp(adb: Adb, mode: str | None) -> dict:
    """ゲストの Transparent Huge Pages (THP) 方針を設定し、読み戻した状態を返す。
    userdebug/eng ビルド以外では適用をスキップする（権限昇格回避策は使わない）。"""
    if mode is None:
        return {"applied": False, "mode": None, "reason": "disabled"}
    if mode not in GUEST_THP_MODES:
        raise BackendError(f"不正な memory.guest_thp 値です: {mode!r}")
    enabled, defrag = GUEST_THP_MODES[mode]
    if not adb.ensure_root():
        return {"applied": False, "mode": mode, "reason": "root unavailable (non-userdebug build)"}
    base = "/sys/kernel/mm/transparent_hugepage"
    w = adb.shell(f"echo {enabled} > {base}/enabled && echo {defrag} > {base}/defrag", timeout=15)
    obs_en = adb.shell(f"cat {base}/enabled", timeout=10).out.strip()
    obs_df = adb.shell(f"cat {base}/defrag", timeout=10).out.strip()
    ok = w.ok and (f"[{enabled}]" in obs_en) and (f"[{defrag}]" in obs_df)
    res = {"applied": ok, "mode": mode, "enabled": obs_en, "defrag": obs_df}
    if not ok and not obs_en:
        res["reason"] = "guest kernel lacks transparent_hugepage sysfs"
    return res


def guest_info(adb: Adb) -> dict:
    """ゲスト側の構成とレンダラー（ソフトウェアフォールバック検出用）を取得する。"""
    props = {k: adb.getprop(k) for k in (
        "ro.build.version.release", "ro.build.version.sdk", "ro.build.fingerprint", "ro.product.cpu.abilist",
        "ro.dalvik.vm.native.bridge", "ro.hardware.egl", "ro.hardware.vulkan", "ro.boot.qemu.gles")}
    sf = adb.shell("dumpsys SurfaceFlinger | grep -iE 'GLES:|renderer|vulkan' | head -5", timeout=30).out.strip()
    nproc = adb.shell("nproc; cat /proc/meminfo | head -1", timeout=15).out.split()
    vk = vulkan_device(adb)
    text = (sf + " " + json.dumps(vk)).lower()
    software = any(s in text for s in ("swiftshader", "llvmpipe", "lavapipe", "software"))
    return {"props": props, "surfaceflinger": sf, "vulkan": vk, "nproc": nproc[0] if nproc else None,
            "memtotal_kb": nproc[2] if len(nproc) > 2 else None,
            "software_rendering_suspected": software}


def vulkan_device(adb: Adb) -> dict:
    """ゲストから見える Vulkan デバイス（`cmd gpu vkjson`）。拡張は性能・互換性に関わるものだけ抜き出す。"""
    r = adb.shell("cmd gpu vkjson", timeout=60)
    try:
        data = json.loads(r.out)
    except json.JSONDecodeError:
        return {"error": (r.out + r.err).strip()[:200]}
    devs = []
    for d in data.get("devices", []):
        p = d.get("properties", {})
        api = int(p.get("apiVersion", 0))  # vkjson は数値を float で返すことがある
        exts = [e.get("extensionName") for e in d.get("extensions", [])]
        devs.append({
            "deviceName": p.get("deviceName"),
            "apiVersion": f"{api >> 22}.{(api >> 12) & 0x3ff}.{api & 0xfff}",
            "driverVersion": p.get("driverVersion"),
            "extension_count": len(exts),
            "notable": sorted(e for e in exts if e and any(k in e for k in (
                "cooperative_matrix", "ray_query", "ray_tracing", "acceleration_structure",
                "shader_float16", "descriptor_indexing", "timeline_semaphore", "dynamic_rendering"))),
        })
    return {"devices": devs}
