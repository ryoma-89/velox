"""ベンチマーク実行（ウォームアップ・複数回測定・環境記録・テレメトリー）。"""
from __future__ import annotations

import time

from velox.backends.adb import Adb
from velox.backends.aosp import AospEmulator, guest_info
from velox.bench import micro, store
from velox.core import config as cfgmod
from velox.core import paths
from velox.core.util import now_iso, run as exec_cmd
from velox import telemetry

SUITES = {
    "quick": ["cpu_hash_1t", "cpu_hash_mt", "mem_dd"],
    "micro": list(micro.TESTS),
}


class RunAborted(RuntimeError):
    pass


def run_suite(suite: str = "micro", reps: int = 3, warmup: int = 1, tag: str = "",
              cfg: dict | None = None) -> dict:
    emu = AospEmulator()
    st = emu.status()
    if not st["running"] or not st.get("boot_completed"):
        raise RunAborted("エミュレーターが起動していません（emu start）")
    if st.get("guest", {}).get("software_rendering_suspected"):
        raise RunAborted("ソフトウェア描画が疑われるため測定を中止しました")
    if suite == "antutu":
        from velox.bench import antutu
        return antutu.run(reps=reps, tag=tag)
    tests = SUITES.get(suite)
    if tests is None:
        raise RunAborted(f"未知のスイート: {suite}")
    cfg = cfg or st["runtime"].get("profile") or cfgmod.load_active()
    adb = Adb(st["runtime"]["serial"])
    rooted = adb.ensure_root()
    commit = exec_cmd(["git", "-C", str(paths.ROOT), "rev-parse", "HEAD"], timeout=5)
    run = {
        "id": store.new_id(), "suite": suite, "tag": tag, "started_at": now_iso(),
        "profile_fingerprint": cfgmod.fingerprint(cfg), "profile": cfg,
        "env": {"git_commit": commit.out.strip() if commit.ok else None,
                "emulator_version": emu.sdk.emulator_version(), "guest": st.get("guest", {}),
                "runtime": {k: st["runtime"].get(k) for k in ("command", "affinity", "boot_seconds", "tuned", "emulator_patch")}},
        "reps": reps, "warmup": warmup, "status": "running", "guest_root": rooted,
    }
    host_before = telemetry.sample(1.0)
    run["env"]["host_background"] = {"cpu": host_before.get("cpu", {}).get("total"),
                                     "gpu_util": host_before.get("gpu", {}).get("utilization.gpu")}
    raw: dict[str, list[float]] = {}
    observations: dict[str, list[dict]] = {}
    meta: dict[str, micro.Metric] = {}
    t0 = time.monotonic()
    try:
        with telemetry.Recorder(period=5.0) as rec:
            for i in range(warmup + reps):
                for m in micro.run_once(adb, tests):
                    meta[m.name] = m
                    if i >= warmup:
                        raw.setdefault(m.name, []).append(m.value)
                        if m.observations is not None:
                            observations.setdefault(m.name, []).append(m.observations)
        run["status"] = "ok"
    except Exception as e:
        run["status"] = "error"
        run["error"] = str(e)
        rec = locals().get("rec")
    run["duration_s"] = round(time.monotonic() - t0, 1)
    run["telemetry"] = rec.summary() if rec else None
    run["metrics"] = {n: {"unit": meta[n].unit, "higher_is_better": meta[n].higher_is_better,
                          "detail": meta[n].detail, "values": v, "stats": store.stats(v)}
                      for n, v in raw.items()}
    for name, entries in observations.items():
        run["metrics"][name]["observations"] = entries
    store.append(run)
    return run


def boot_benchmark(cfg: dict, reps: int = 3) -> dict:
    """コールドブート時間（UX の一指標）。再起動を伴うため最適化ループからも利用する。"""
    emu = AospEmulator()
    vals = []
    for _ in range(reps):
        if emu.vm_pids():
            emu.stop()
        info = emu.start(cfg, wait=True)
        if "boot_seconds_total" in info:
            vals.append(info["boot_seconds_total"])
    return {"boot_seconds": store.stats(vals), "values": vals}


def guest_snapshot() -> dict:
    emu = AospEmulator()
    st = emu.status()
    return guest_info(Adb(st["runtime"]["serial"])) if st.get("boot_completed") else {}
