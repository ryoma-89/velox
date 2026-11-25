"""AnTuTu 実行支援。

方針（docs/BENCHMARK.md）:
- APK は配布元と AnTuTu の発行者署名を確認できるものを使う。入手元と
  SHA-256 を記録する。AnTuTu 本体の改変・検出回避・端末情報の偽装は行わない。
- 実行開始はアプリ UI の操作が必要なため、現段階では「起動 → ユーザーまたは
  UI 自動操作が開始 → 結果画面のテキストを uiautomator で読み取る」半自動方式。
- スコアは結果画面から読み取った値のみを記録し、推定値を作らない。
"""
from __future__ import annotations

from collections import Counter
import hashlib
import re
import time
import uuid
from xml.etree import ElementTree

from velox.backends.adb import Adb
from velox.backends.aosp import AospEmulator
from velox.bench import store
from velox.core.util import now_iso

PACKAGES = ("com.antutu.ABenchMark",)
TEST_ACTIVITY = "com.android.module.app.ui.test.activity.ActivityScoreBench"
RESULT_ACTIVITY = "com.android.module.app.ui.test.activity.ActivityTestResult"
VIEW = "com.antutu.ABenchMark:id/"
THREE_D = {
    # The Standard APK also has activities used by the UX tests.
    "com.antutu.benchmark.full/com.epicgames.unreal.": "standard",
    "com.antutu.benchmark.full.lite/": "lite",
}
SECTIONS = ("cpu", "gpu", "mem", "ux")
NUMBER = re.compile(r"[0-9]+|[0-9]{1,3}(?:,[0-9]{3})+")


class AntutuNotReady(RuntimeError):
    pass


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def installed(adb: Adb) -> dict | None:
    for pkg in PACKAGES:
        out = adb.shell(f"dumpsys package {pkg} | grep -E 'versionName|primaryCpuAbi' | head -3").out
        if "versionName" in out:
            ver = re.search(r"versionName=(\S+)", out)
            abi = re.search(r"primaryCpuAbi=(\S+)", out)
            return {"package": pkg, "version": ver.group(1) if ver else None,
                    "primary_abi": abi.group(1) if abi else None}
    return None


def structured_scores(hierarchy: ElementTree.Element) -> dict | None:
    """V11 の画像アイコン付きカードを、表示される順序で読む。"""
    result_list = hierarchy.find(f".//node[@resource-id='{VIEW}RecyclerView']")
    if result_list is None:
        return None
    headers, sections = [], []
    for row in result_list:
        ids = {node.get("resource-id"): node.get("text", "").strip() for node in row.iter("node")}
        if VIEW + "TextViewDeviceName" in ids:
            records = headers
        elif VIEW + "ImageViewScoreIcon" in ids:
            records = sections
        else:
            continue
        abnormal = VIEW + "TextViewSuspiciousScoreValue" in ids
        key = "TextViewSuspiciousScoreValue" if abnormal else "TextViewScoreValue"
        value = ids.get(VIEW + key, "")
        if not NUMBER.fullmatch(value):
            return None
        records.append((int(value.replace(",", "")), abnormal))
    if len(headers) != 1 or len(sections) != len(SECTIONS):
        return None
    scores = dict(zip(("total", *SECTIONS), headers + sections))
    values = {key: value for key, (value, _) in scores.items()}
    if values["total"] <= 10000 or values["total"] != sum(values[key] for key in SECTIONS):
        return None
    return {**values, "abnormal": [key for key, (_, flagged) in scores.items() if flagged]}


def read_scores(adb: Adb) -> dict | None:
    """結果画面の UI ダンプから総合・CPU・GPU・MEM・UX スコアを抽出する。"""
    path = f"/sdcard/velox-antutu-{uuid.uuid4().hex}.xml"
    result = adb.shell(f"trap 'rm -f {path}' EXIT; uiautomator dump {path} >/dev/null 2>&1 && cat {path}",
                       timeout=30)
    if not result.ok:
        return None
    try:
        hierarchy = ElementTree.fromstring(result.out)
    except ElementTree.ParseError:
        return None
    texts = [node.attrib.get("text", "").strip() for node in hierarchy.iter("node")]
    if parsed := structured_scores(hierarchy):
        return {**parsed, "raw_texts": texts[:80]}
    labels = [(i, text.lower()) for i, text in enumerate(texts) if text.lower() in SECTIONS]
    if Counter(label for _, label in labels) != Counter(SECTIONS):
        return None
    scores = {}
    numbers = [(i, int(text.replace(",", ""))) for i, text in enumerate(texts)
               if NUMBER.fullmatch(text)]
    used = set()
    for ((first, label), (last, _)) in zip(labels, labels[1:] + [(len(texts), "")]):
        match = next(((i, value) for i, value in numbers if first < i < last), None)
        if match is None:
            return None
        used.add(match[0])
        scores[label] = match[1]
    total = sum(scores.values())
    if total <= 10000 or not any(value == total and i not in used for i, value in numbers):
        return None
    abnormal = ["unknown"] if any("score is abnormal" in text.lower() for text in texts) else []
    return {"total": total, **scores, "abnormal": abnormal, "raw_texts": texts[:80]}


def run(reps: int = 1, tag: str = "", poll_timeout: float = 1800) -> dict:
    emu = AospEmulator()
    st = emu.status()
    adb = Adb(st["runtime"]["serial"])
    info = installed(adb)
    if not info:
        raise AntutuNotReady("AnTuTu がインストールされていません。公式サイトから入手した APK を "
                             "`emu install <apk> --source <URL>` で導入してください。")
    results = []
    started_at = now_iso()
    for i in range(reps):
        adb.shell(f"monkey -p {info['package']} -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1")
        t0 = time.monotonic()
        score = None
        started = False
        variants = set()
        while time.monotonic() - t0 < poll_timeout:
            time.sleep(15)
            activity = adb.shell("dumpsys activity activities | grep topResumedActivity", timeout=15).out
            variants.update(variant for component, variant in THREE_D.items() if f" {component}" in activity)
            if TEST_ACTIVITY in activity or variants:
                started = True
            if not started or f" {info['package']}/{RESULT_ACTIVITY}" not in activity:
                continue
            score = read_scores(adb)
            if score:
                break
        results.append({"rep": i, "score": score, "seconds": round(time.monotonic() - t0),
                        "3d_started": sorted(variants)})
        if not score:
            break
    totals = [r["score"]["total"] for r in results
              if r["score"] and r["score"]["gpu"] > 0 and not r["score"]["abnormal"]]
    status = "incomplete"
    if len(totals) == reps:
        status = "ok"
    elif len(results) == reps and all(r["score"] for r in results):
        status = "invalid"
    run_rec = {"id": store.new_id(), "suite": "antutu", "tag": tag, "started_at": started_at,
               "env": {"antutu": info, "emulator_version": emu.sdk.emulator_version(), "guest": st.get("guest"),
                       "runtime": {k: st["runtime"].get(k) for k in ("command", "affinity", "emulator_patch")}},
               "status": status,
               "metrics": {"antutu_total": {"unit": "pt", "higher_is_better": True, "values": totals,
                                            "stats": store.stats(totals)}},
               "details": results}
    store.append(run_rec)
    return run_rec
