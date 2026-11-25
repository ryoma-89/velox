"""Phase 20 multi-objective Pareto frontier and sensitivity analysis.

Reads measured Phase 20 benchmark JSON artifacts under benchmarks/results/phase20/
and computes:
  1. Normalized objective vectors across CPU 1T/MT, RAM 1T/MT, GPU Draws/Sync,
     and 120 fps 1% Low pacing for all 6 hybrid CPU topology configurations.
  2. Exact Pareto-optimal (non-dominated) sets and dominance explanations.
  3. Workload-weighted scores for the 5 Phase 20 Extreme profiles.
  4. Quantitative sensitivity analysis of each optimization lever (Patches 0031,
     0018, 0019, thread placement, and VELOX_HIGHRES_POLL trade-off).
"""
from __future__ import annotations

import json
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
P20 = ROOT / "benchmarks" / "results" / "phase20"
OUT = P20 / "multi_objective_pareto.json"

# Windows native RTX 5070 Ti vk-render reference (Phase 17/18/20 validated baseline)
NATIVE_VK_DRAWS_FPS = 1699.15
NATIVE_VK_SYNC_FPS = 14519.20


def is_dominated(a: dict[str, float], b: dict[str, float], keys: list[str]) -> bool:
    """Return True if vector `b` strictly dominates vector `a` on `keys`."""
    ge_all = all(b[k] >= a[k] for k in keys)
    gt_any = any(b[k] > a[k] for k in keys)
    return ge_all and gt_any


def pareto_front(items: list[dict], keys: list[str]) -> tuple[list[str], dict[str, list[str]]]:
    front: list[str] = []
    dominated_by: dict[str, list[str]] = {}
    for a in items:
        doms = [
            b["name"]
            for b in items
            if b["name"] != a["name"] and is_dominated(a["norm"], b["norm"], keys)
        ]
        if doms:
            dominated_by[a["name"]] = doms
        else:
            front.append(a["name"])
    return front, dominated_by


def main() -> None:
    s45 = json.loads((P20 / "step4_5_whpx_cpu_mem_storage.json").read_text(encoding="utf-8"))
    vk31 = json.loads((P20 / "vk_patch0031_v2_ab.json").read_text(encoding="utf-8"))

    nat = s45["native"]
    nat_int_1t = nat["cpu_clang"]["int_1t_gops"]
    nat_int_mt = nat["cpu_clang"]["int_mt_gops"]
    nat_ram_1t = nat["ram"]["64m_1t"]["copy_gib_s"]
    nat_ram_20t = nat["ram"]["256m_20t"]["copy_gib_s"]
    nat_rand_ns = nat["ram"]["64m_1t"]["random_load_ns"]

    configs = []
    for r in s45["topology_matrix"]:
        raw = {
            "cpu_1t_gops": r["cpu"]["int_1t_gops"],
            "cpu_mt_gops": r["cpu"]["int_mt_gops"],
            "ram_1t_gibs": r["ram_64m_1t"]["copy_gib_s"],
            "ram_rand_ns": r["ram_64m_1t"]["random_load_ns"],
            "ram_mt_gibs": r["ram_256m_all"]["copy_gib_s"],
            "gpu_draws_fps": r["render_draws"]["fps"],
            "gpu_sync_fps": r["render_sync"]["fps"],
            "pacing_120_1pct": r["game_120"]["fps_1pct_low"],
        }
        norm = {
            "cpu_1t": round(raw["cpu_1t_gops"] / nat_int_1t, 4),
            "cpu_mt": round(raw["cpu_mt_gops"] / nat_int_mt, 4),
            "ram_1t": round(raw["ram_1t_gibs"] / nat_ram_1t, 4),
            "ram_lat_eff": round(nat_rand_ns / raw["ram_rand_ns"], 4),
            "ram_mt": round(raw["ram_mt_gibs"] / nat_ram_20t, 4),
            "gpu_draws": round(raw["gpu_draws_fps"] / NATIVE_VK_DRAWS_FPS, 4),
            "gpu_sync": round(raw["gpu_sync_fps"] / NATIVE_VK_SYNC_FPS, 4),
            "pacing_120": round(raw["pacing_120_1pct"] / 120.0, 4),
        }
        configs.append(
            {
                "name": r["name"],
                "cores": r["cores"],
                "affinity": r["affinity"],
                "thread_placement": r["thread_placement"],
                "raw": raw,
                "norm": norm,
            }
        )

    all_keys = ["cpu_1t", "cpu_mt", "ram_1t", "ram_mt", "gpu_draws", "gpu_sync", "pacing_120"]
    core_keys = ["cpu_mt", "gpu_sync", "pacing_120"]
    mem_keys = ["ram_1t", "ram_lat_eff", "ram_mt", "cpu_mt"]

    front_all, dom_all = pareto_front(configs, all_keys)
    front_core, dom_core = pareto_front(configs, core_keys)
    front_mem, dom_mem = pareto_front(configs, mem_keys)

    weights = {
        "integrated": {
            "cpu_1t": 0.12,
            "cpu_mt": 0.22,
            "ram_1t": 0.10,
            "ram_mt": 0.12,
            "gpu_draws": 0.16,
            "gpu_sync": 0.14,
            "pacing_120": 0.14,
        },
        "gpu": {
            "cpu_1t": 0.05,
            "cpu_mt": 0.05,
            "ram_1t": 0.05,
            "ram_mt": 0.05,
            "gpu_draws": 0.30,
            "gpu_sync": 0.25,
            "pacing_120": 0.25,
        },
        "cpu": {
            "cpu_1t": 0.35,
            "cpu_mt": 0.50,
            "ram_1t": 0.05,
            "ram_mt": 0.10,
            "gpu_draws": 0.0,
            "gpu_sync": 0.0,
            "pacing_120": 0.0,
        },
        "memory": {
            "cpu_1t": 0.05,
            "cpu_mt": 0.05,
            "ram_1t": 0.35,
            "ram_lat_eff": 0.25,
            "ram_mt": 0.30,
        },
    }

    profile_rankings = {}
    for pname, w in weights.items():
        scored = []
        for c in configs:
            score = sum(c["norm"][k] * wt for k, wt in w.items())
            scored.append({"config": c["name"], "weighted_score": round(score, 4)})
        scored.sort(key=lambda x: x["weighted_score"], reverse=True)
        profile_rankings[pname] = scored

    # Storage Pareto (HIGHRES_POLL=0 vs 1, aio=native+pool vs threads)
    storage_points = []
    for r in s45["storage_patch0019_ab"]:
        s256 = r["storage_256m"]
        fw = r["fs_workload_data"]
        storage_points.append(
            {
                "label": r["label"],
                "seq_write_med_mibs": round(statistics.median([x["seq_write_and_flush_mib_s"] for x in s256]), 1),
                "fsync_p50_ms": round(statistics.median([x["random_durable_4k_write_p50_ms"] for x in s256]), 3),
                "small_create_per_s": fw["small_create_per_s"],
                "durable_tx_per_s": fw["durable_tx_per_s"],
                "app_32m_install_mibs": fw["app_32m_install_mbs"],
            }
        )

    # Sensitivity deltas
    c_map = {c["name"]: c for c in configs}
    c_20pe = c_map["20_PE_rpe"]["raw"]
    c_20auto = c_map["20_auto"]["raw"]
    c_18soft = c_map["18_soft_rpe"]["raw"]

    # Patch 0018 A/B mean delta
    p18_off = [r for r in s45["whpx_patch0018_ab"] if r["mode"] == "prerun_off"]
    p18_on = [r for r in s45["whpx_patch0018_ab"] if r["mode"] == "prerun_on"]
    p18_delta = {
        "int_mt_pct": round(
            (statistics.mean(r["cpu"]["int_mt_gops"] for r in p18_on)
             / statistics.mean(r["cpu"]["int_mt_gops"] for r in p18_off) - 1.0) * 100.0,
            2,
        ),
        "fp_mt_pct": round(
            (statistics.mean(r["cpu"]["fp_mt_gops"] for r in p18_on)
             / statistics.mean(r["cpu"]["fp_mt_gops"] for r in p18_off) - 1.0) * 100.0,
            2,
        ),
        "vk_sync_pct": round(
            (statistics.mean(r["render_sync"]["fps"] for r in p18_on)
             / statistics.mean(r["render_sync"]["fps"] for r in p18_off) - 1.0) * 100.0,
            2,
        ),
        "vk_draws_pct": round(
            (statistics.mean(r["render_draws"]["fps"] for r in p18_on)
             / statistics.mean(r["render_draws"]["fps"] for r in p18_off) - 1.0) * 100.0,
            2,
        ),
    }

    # Patch 0031 v2 A/B summary
    def _vk31_med(runs: list[dict], key: str, sub: str) -> float:
        vals = [r[key][sub] for r in runs if key in r and sub in r[key]]
        return statistics.median(vals) if vals else 0.0

    vk_a = [r for r in vk31.get("runs", []) if r.get("tag", "").startswith("A")]
    vk_b = [r for r in vk31.get("runs", []) if r.get("tag", "").startswith("B")]
    p31_summary = {}
    if vk_a and vk_b:
        da, db = _vk31_med(vk_a, "draws", "fps"), _vk31_med(vk_b, "draws", "fps")
        sa, sb = _vk31_med(vk_a, "sync", "fps"), _vk31_med(vk_b, "sync", "fps")
        p31_summary = {
            "draws_a_med": round(da, 2),
            "draws_b_med": round(db, 2),
            "draws_pct": round((db / da - 1.0) * 100.0, 2) if da else None,
            "sync_a_med": round(sa, 2),
            "sync_b_med": round(sb, 2),
            "sync_pct": round((sb / sa - 1.0) * 100.0, 2) if sa else None,
        }

    report = {
        "native_references": {
            "cpu_int_1t_gops": nat_int_1t,
            "cpu_int_mt_gops": nat_int_mt,
            "ram_64m_1t_gibs": nat_ram_1t,
            "ram_64m_1t_rand_ns": nat_rand_ns,
            "ram_256m_20t_gibs": nat_ram_20t,
            "vk_render_draws_fps": NATIVE_VK_DRAWS_FPS,
            "vk_render_sync_fps": NATIVE_VK_SYNC_FPS,
            "storage_seq_write_med_mibs": round(
                statistics.median([x["seq_write_and_flush_mib_s"] for x in nat["storage_256m"]]), 1
            ),
            "storage_fsync_p50_ms": round(
                statistics.median([x["random_durable_4k_write_p50_ms"] for x in nat["storage_256m"]]), 3
            ),
        },
        "topology_configs": configs,
        "pareto": {
            "all_7_objectives": {"frontier": front_all, "dominated_by": dom_all},
            "core_3d_cpu_sync_pacing": {"frontier": front_core, "dominated_by": dom_core},
            "memory_and_cpu": {"frontier": front_mem, "dominated_by": dom_mem},
        },
        "profile_rankings": profile_rankings,
        "storage_tradeoff": storage_points,
        "sensitivity": {
            "patch_0031_vk_hotpath": p31_summary,
            "patch_0018_whpx_prerun": p18_delta,
            "topology_20PE_rpe_vs_20auto": {
                "vk_sync_pct": round((c_20pe["gpu_sync_fps"] / c_20auto["gpu_sync_fps"] - 1.0) * 100.0, 2),
                "vk_draws_pct": round((c_20pe["gpu_draws_fps"] / c_20auto["gpu_draws_fps"] - 1.0) * 100.0, 2),
                "pacing_120_1pct_delta_fps": round(c_20pe["pacing_120_1pct"] - c_20auto["pacing_120_1pct"], 2),
                "ram_mt_pct": round((c_20pe["ram_mt_gibs"] / c_20auto["ram_mt_gibs"] - 1.0) * 100.0, 2),
            },
            "topology_20PE_rpe_vs_18soft_rpe": {
                "cpu_mt_pct": round((c_20pe["cpu_mt_gops"] / c_18soft["cpu_mt_gops"] - 1.0) * 100.0, 2),
                "vk_sync_pct": round((c_20pe["gpu_sync_fps"] / c_18soft["gpu_sync_fps"] - 1.0) * 100.0, 2),
                "vk_draws_pct": round((c_20pe["gpu_draws_fps"] / c_18soft["gpu_draws_fps"] - 1.0) * 100.0, 2),
                "pacing_120_1pct_delta_fps": round(c_20pe["pacing_120_1pct"] - c_18soft["pacing_120_1pct"], 2),
                "ram_mt_pct": round((c_20pe["ram_mt_gibs"] / c_18soft["ram_mt_gibs"] - 1.0) * 100.0, 2),
            },
        },
    }

    OUT.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Wrote {OUT}")
    print("Pareto (core CPU_MT + GPU_Sync + 120fps_1%Low):", report["pareto"]["core_3d_cpu_sync_pacing"])
    print("Pareto (all 7 objectives):", report["pareto"]["all_7_objectives"])
    for pname, ranks in profile_rankings.items():
        print(f"  {pname:12s} #1 = {ranks[0]['config']} ({ranks[0]['weighted_score']}) | #2 = {ranks[1]['config']} ({ranks[1]['weighted_score']})")


if __name__ == "__main__":
    main()
