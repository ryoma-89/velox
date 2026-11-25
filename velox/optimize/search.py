"""自動最適化ループ（測定 → 候補生成 → 適用 → 再起動 → 測定 → 比較 → 採用/ロールバック）。

初期実装は「座標降下」: 現在の最良設定から 1 パラメータずつ値を変えて試し、
ノイズを上回る改善があれば採用する。Optuna がインストールされていれば
strategy="optuna" で TPE 探索に切り替えられる（候補生成部だけ差し替え）。

安全装置: 試行回数・時間の上限、停止ファイル、GPU 温度上限、起動失敗時の
自動ロールバック。性能測定は直列実行（並列化しない）。
"""
from __future__ import annotations

import copy
import json
import os
import random
import time
from pathlib import Path

from velox.backends.adb import Adb
from velox.backends.aosp import AospEmulator
from velox.bench import runner, store
from velox.core import config as cfgmod
from velox.core import paths
from velox.core.util import now_iso
from velox.optimize import recommend
from velox import telemetry

OPT_DIR = paths.STATE / "optimize"
STATE_FILE = OPT_DIR / "state.json"
STOP_FILE = OPT_DIR / "STOP"
HISTORY = paths.RESULTS / "optimize.jsonl"

DEFAULT_SPACE: dict[str, list] = {
    "cpu.cores": [4, 6, 8, 12, 16, 20],
    "cpu.affinity": ["P", "E", "PE", "auto"],
    "cpu.priority": ["above_normal", "high"],
    "memory.ram_mb": [8192, 16384, 24576, 32768],
    "gpu.disable_implicit_vk_layers": [True, False],
}

# 第 13 段階: ソース版のホスト（emulator.dir を指定した構成）だけが読むスレッド配置の候補。
# {} は OS 任せ。もう一つは実測で最も良かった「vCPU を先頭 2 つの P コアから外し、ホストの
# RenderThread と先読みは P コアの中で自由にする」配置（docs/research/phase13-thread-scheduling.md）。
PLACEMENT_SPACE: list[dict] = [
    {},
    {"vcpu": "PE,-P#0,-P#1", "render": "P", "prefetch": "P"},
]


def _write_state(st: dict) -> None:
    OPT_DIR.mkdir(parents=True, exist_ok=True)
    STATE_FILE.write_text(json.dumps(st, ensure_ascii=False, indent=2, default=str), encoding="utf-8")


def read_state() -> dict:
    if STATE_FILE.exists():
        return json.loads(STATE_FILE.read_text(encoding="utf-8"))
    return {"status": "idle"}


def request_stop() -> None:
    OPT_DIR.mkdir(parents=True, exist_ok=True)
    STOP_FILE.write_text(now_iso(), encoding="utf-8")


def _candidates(base: dict, space: dict[str, list], strategy: str, rng: random.Random):
    """(key, value) の列を生成する。"""
    if strategy == "coordinate":
        for key, values in space.items():
            cur = cfgmod.get_key(base, key)
            for v in values:
                if v != cur:
                    yield key, v
    elif strategy == "random":
        keys = list(space)
        while True:
            k = rng.choice(keys)
            yield k, rng.choice(space[k])
    else:
        raise ValueError(f"未対応の strategy: {strategy}")


def evaluate(cfg: dict, suite: str, reps: int, settle_s: float) -> dict:
    emu = AospEmulator()
    if emu.vm_pids():
        emu.stop()
    info = emu.start(cfg, wait=True)
    if "boot_error" in info:
        return {"status": "boot_failed", "error": info["boot_error"]}
    count = Adb(info["serial"]).shell("nproc", timeout=15)
    if not count.ok or count.out.strip() != str(cfg["cpu"]["cores"]):
        return {"status": "invalid", "error": f"ゲスト CPU 数が {cfg['cpu']['cores']} と一致しません "
                                               f"({count.out.strip() or count.err.strip()})"}
    time.sleep(settle_s)  # 起動直後のバックグラウンド dexopt 等を落ち着かせる
    run = runner.run_suite(suite, reps=reps, warmup=1, tag="optimize", cfg=cfg)
    run["boot_seconds"] = info.get("boot_seconds_total")
    return run


def _candidate_config(base: dict, key: str, value) -> dict:
    cfg = cfgmod.set_key(base, key, value)
    if key == "cpu.cores" and value > 6 and not cfg["emulator"]["dir"]:
        # SDK の上限を超える vCPU と、その起動に必要な設定を同時に検証する。
        cfg = cfgmod.set_key(cfg, "cpu.smp_override", True)
    return cfg


def optimize(*, suite: str = "quick", reps: int = 3, max_trials: int = 20, max_minutes: float = 180,
             min_gain_pct: float = 2.0, strategy: str = "coordinate", space: dict | None = None,
             gpu_temp_limit: float = 83.0, settle_s: float = 30.0, seed: int = 0) -> dict:
    space = space or DEFAULT_SPACE
    if STOP_FILE.exists():
        STOP_FILE.unlink()
    rng = random.Random(seed)
    t0 = time.monotonic()
    best_cfg = cfgmod.load_active()
    if space is DEFAULT_SPACE:
        space = recommend.hardware_search_space(has_custom_runtime=bool(best_cfg["emulator"].get("dir")))
    st = {"status": "running", "pid": os.getpid(), "started_at": now_iso(), "suite": suite, "strategy": strategy,
          "limits": {"max_trials": max_trials, "max_minutes": max_minutes}, "trial": 0, "accepted": []}
    _write_state(st)
    try:
        base_run = evaluate(best_cfg, suite, reps, settle_s)
    except Exception as exc:
        base_run = {"status": "error", "error": str(exc)}
    if base_run.get("status") != "ok":
        st.update(status="failed", error=f"ベースライン測定に失敗: {base_run.get('error')}")
        _write_state(st)
        return st
    best_run = base_run
    st["baseline_run"] = base_run["id"]
    seen = {cfgmod.fingerprint(best_cfg)}
    for key, value in _candidates(best_cfg, space, strategy, rng):
        if st["trial"] >= max_trials:
            st["stop_reason"] = "max_trials"; break
        if (time.monotonic() - t0) / 60 > max_minutes:
            st["stop_reason"] = "max_minutes"; break
        if STOP_FILE.exists():
            st["stop_reason"] = "user_stop"; break
        g = telemetry.gpu()
        if isinstance(g.get("temperature.gpu"), float) and g["temperature.gpu"] > gpu_temp_limit:
            st["stop_reason"] = f"gpu_temp {g['temperature.gpu']}C > {gpu_temp_limit}C"; break
        try:
            cand = _candidate_config(best_cfg, key, value)
        except cfgmod.ConfigError:
            continue
        fp = cfgmod.fingerprint(cand)
        if fp in seen:
            continue
        seen.add(fp)
        st["trial"] += 1
        st["current"] = {"key": key, "value": value}
        _write_state(st)
        rec = {"t": now_iso(), "trial": st["trial"], "key": key, "value": value, "fingerprint": fp,
               "incumbent_run": best_run["id"]}
        try:
            run = evaluate(cand, suite, reps, settle_s)
        except Exception as e:  # 不安定化はロールバック対象
            run = {"status": "error", "error": str(e)}
        rec["run"] = run.get("id"); rec["status"] = run.get("status")
        if run.get("status") == "ok":
            cmp = store.compare(best_run, run)
            gain = cmp["geomean_improvement_pct"]
            noise = max((m["noise_pct"] for m in cmp["metrics"].values()), default=0)
            rec.update(gain_pct=gain, noise_pct=noise)
            if gain is not None and gain > max(min_gain_pct, noise):
                best_cfg, best_run = cand, run
                rec["decision"] = "accept"
                st["accepted"].append({"key": key, "value": value, "gain_pct": gain})
                cfgmod.save_active(best_cfg, reason=f"optimize accept {key}={value} (+{gain}%)")
            else:
                rec["decision"] = "reject"
        else:
            rec["decision"] = "rollback"
            rec["error"] = run.get("error")
        with open(HISTORY, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False, default=str) + "\n")
        st["last"] = rec
        _write_state(st)
    else:
        st["stop_reason"] = "space_exhausted"
    cfgmod.save_profile("best", best_cfg)
    st.update(status="finished", finished_at=now_iso(), best_run=best_run["id"],
              total_improvement=store.compare(base_run, best_run))
    _write_state(st)
    # 最後の試行設定のまま VM が残らないよう停止する（最良設定は active / profiles/best.json）
    AospEmulator().stop()
    return st


def history(limit: int = 50) -> list[dict]:
    if not HISTORY.exists():
        return []
    rows = [json.loads(l) for l in HISTORY.read_text(encoding="utf-8").splitlines() if l.strip()]
    return rows[-limit:]
