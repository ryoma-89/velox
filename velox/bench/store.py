"""ベンチマーク結果の保存（JSONL、Git で差分管理できる形式）と統計。"""
from __future__ import annotations

import json
import math
import statistics
import uuid

from velox.core import paths

RUNS = paths.RESULTS / "runs.jsonl"


def stats(values: list[float]) -> dict:
    v = [x for x in values if x is not None]
    if not v:
        return {"n": 0}
    return {"n": len(v), "mean": round(statistics.fmean(v), 3), "median": round(statistics.median(v), 3),
            "max": max(v), "min": min(v), "stdev": round(statistics.stdev(v), 3) if len(v) > 1 else 0.0,
            "cv_pct": round(100 * statistics.stdev(v) / statistics.fmean(v), 2) if len(v) > 1 and statistics.fmean(v) else 0.0}


def new_id() -> str:
    return uuid.uuid4().hex[:10]


def append(run: dict) -> None:
    paths.ensure()
    with open(RUNS, "a", encoding="utf-8") as f:
        f.write(json.dumps(run, ensure_ascii=False, default=str) + "\n")


def load(limit: int | None = None, suite: str | None = None) -> list[dict]:
    if not RUNS.exists():
        return []
    rows = [json.loads(l) for l in RUNS.read_text(encoding="utf-8").splitlines() if l.strip()]
    if suite:
        rows = [r for r in rows if r.get("suite") == suite]
    return rows[-limit:] if limit else rows


def get(run_id: str) -> dict | None:
    for r in load():
        if r["id"].startswith(run_id):
            return r
    return None


def compare(a: dict, b: dict) -> dict:
    """a→b の変化率。higher_is_better を考慮した改善率（+ が改善）と幾何平均。"""
    out, ratios = {}, []
    for name, mb in b.get("metrics", {}).items():
        ma = a.get("metrics", {}).get(name)
        if not ma or not ma["stats"].get("median") or not mb["stats"].get("median"):
            continue
        r = mb["stats"]["median"] / ma["stats"]["median"]
        if not mb["higher_is_better"]:
            r = 1 / r
        ratios.append(r)
        out[name] = {"before": ma["stats"]["median"], "after": mb["stats"]["median"], "unit": mb["unit"],
                     "improvement_pct": round(100 * (r - 1), 2),
                     "noise_pct": max(ma["stats"].get("cv_pct", 0), mb["stats"].get("cv_pct", 0))}
    geo = math.exp(sum(math.log(r) for r in ratios) / len(ratios)) if ratios else None
    return {"metrics": out, "geomean_improvement_pct": round(100 * (geo - 1), 2) if geo else None}
