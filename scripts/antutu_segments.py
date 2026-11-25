"""AnTuTu V11 Standard の GPU フェーズを、録画デモと採点区間に分ける。

GPU フェーズには、得点に関係しない録画デモ（「Prerecorded demonstration | Non-real-time results」）の
動画再生が含まれる。動画の長さは一定なので、フェーズ全体の秒数は描画性能の指標として不十分。
採点区間（縮小画像がタイル状に並ぶ画面）は次のように求める。

- デモの開始: ResourceManager に動画デコーダー（hw-video-codec）が登録された時刻
- デモの終了: MediaCodec が 3 秒間活動しないと記録する `removeResource(... battery/hw-video-codec)` の 3 秒前
- Cottage の採点区間 = 1 本目のデモ終了 → 2 本目のデモ開始
- Coastline の採点区間 = 2 本目のデモ終了 → GPU フェーズ終了

ResourceManager の時刻は秒単位なので、各区間に ±1 秒の誤差がある。
`dumpsys media.resource_manager` は直近のイベントを保持するので、連続した測定の後に 1 回採れば足りる。

使い方: python -m scripts.antutu_segments [--resource-manager rm.txt] RUN.json [RUN.json ...]
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime
import json
from pathlib import Path
import re
import statistics

EVENT = re.compile(r"(\d\d-\d\d \d\d:\d\d:\d\d) (addResource|removeResource)\(pid (\d+), uid (\d+) clientId (\d+)"
                   r"(?:, resources (\S+))?")
BATTERY_TIMEOUT_S = 3.0  # MediaCodec の BatteryChecker（kBatteryStatsTimeoutUs）
BOUNDARY_UNCERTAINTY_S = 1.0


def video_decoder_events(text: str, year: int) -> list[dict]:
    """プロセスごとに登録と無活動を対応付ける。履歴は逆順や複数回の採取を含み得る。"""
    changes = defaultdict(set)
    for stamp, kind, pid, uid, client, resource in EVENT.findall(text):
        key = (pid, uid, client)
        if kind == "addResource" and resource.startswith("non-secure-codec/hw-video-codec"):
            changes[key].add((stamp, "add"))
        elif kind == "removeResource" and resource.startswith("battery/hw-video-codec"):
            changes[key].add((stamp, "inactive"))

    decoders = []
    for events in changes.values():
        current = None
        for stamp, event in sorted(events):
            when = datetime.strptime(f"{year}-{stamp}", "%Y-%m-%d %H:%M:%S").timestamp()
            if event == "add":
                current = {"added": when}
                decoders.append(current)
            elif current is not None:
                current["inactive"] = when
    return decoders


def histogram_summary(hist: Counter[int]) -> dict:
    total = sum(hist.values())
    summary = {"frames": total}
    if not total:
        return summary
    seen = 0
    for ms, count in sorted(hist.items()):
        seen += count
        for p in (50, 95, 99):
            if seen * 100 >= p * total:
                summary.setdefault(f"p{p}_ms", ms)
    return summary


def scored_windows(run: dict, start: float, end: float) -> list[dict]:
    prev_end = None
    selected = []
    for sample in run["samples"]:
        if "window_start" not in sample:
            continue
        earliest = min(prev_end, sample["window_start"]) if prev_end is not None else None
        prev_end = sample["t"]
        if (earliest is not None and sample.get("fps")
                and earliest >= start + BOUNDARY_UNCERTAINTY_S
                and sample["t"] <= end - BOUNDARY_UNCERTAINTY_S):
            selected.append(sample)
    return selected


def segments(run: dict, decoders: list[dict]) -> dict:
    if run.get("gpu_start_s") is None:
        return {"demos": []}
    start = run["wall_start_unix_s"] + run["gpu_start_s"]
    end = run["wall_start_unix_s"] + run["gpu_end_s"] if run.get("gpu_end_s") is not None else None
    limit = end if end is not None else run["wall_start_unix_s"] + run.get("wall_seconds", 0)
    demos = sorted(((v["added"], v.get("inactive")) for v in decoders
                    if start - BOUNDARY_UNCERTAINTY_S <= v["added"] <= limit),
                   key=lambda item: item[0])
    result = {"demos": [{"start_s": round(a - start, 1),
                         "end_s": round(b - BATTERY_TIMEOUT_S - start, 1) if b else None} for a, b in demos]}
    if len(demos) == 2 and demos[0][1] and demos[1][1] and end is not None:
        cottage = (demos[0][1] - BATTERY_TIMEOUT_S, demos[1][0])
        coastline = (demos[1][1] - BATTERY_TIMEOUT_S, end)
        if not (demos[0][0] <= cottage[0] < cottage[1] <= coastline[0] < coastline[1]):
            return result
        result["scored_s"] = {"cottage": round(cottage[1] - cottage[0], 1),
                              "coastline": round(coastline[1] - coastline[0], 1)}
        for name, (a, b) in (("cottage", cottage), ("coastline", coastline)):
            samples = scored_windows(run, a - run["wall_start_unix_s"], b - run["wall_start_unix_s"])
            fps = [s["fps"] for s in samples]
            result.setdefault("scored_fps_median", {})[name] = round(statistics.median(fps), 1) if fps else None
            frame_times: Counter[int] = Counter()
            for sample in samples:
                if sample.get("layers"):
                    histogram = sample["layers"][0].get("present_to_present_ms", {})
                    frame_times.update({int(ms): count for ms, count in histogram.items()})
            window_summary = {"windows": len(samples), "frame_time": histogram_summary(frame_times)}
            for key, get in (
                ("gpu_util_pct", lambda sample: sample.get("gpu", {}).get("utilization.gpu")),
                ("gpu_w", lambda sample: sample.get("gpu", {}).get("power.draw")),
                ("gpu_mem_mib", lambda sample: sample.get("gpu", {}).get("memory.used")),
                ("cpu_pct", lambda sample: sample.get("cpu", {}).get("total")),
            ):
                values = [value for sample in samples if isinstance(value := get(sample), (int, float))]
                if values:
                    window_summary[key] = round(statistics.median(values), 2)
            result.setdefault("scored_samples", {})[name] = window_summary
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-manager", type=Path,
                        help="旧試行の dumpsys media.resource_manager を明示指定")
    parser.add_argument("runs", nargs="+", type=Path)
    a = parser.parse_args()
    out = {}
    for path in a.runs:
        run = json.loads(path.read_text(encoding="utf-8"))
        year = datetime.fromtimestamp(run["wall_start_unix_s"]).year
        resources = a.resource_manager or (Path(run["resource_manager"]) if run.get("resource_manager") else None)
        decoders = video_decoder_events(resources.read_text(encoding="utf-8", errors="replace"),
                                        year) if resources else []
        out[str(path)] = {"label": run.get("label"), "status": run.get("status"),
                          "gpu_seconds": run.get("gpu_seconds"),
                          **segments(run, decoders)}
    print(json.dumps(out, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
