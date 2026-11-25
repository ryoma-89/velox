"""Vulkan ホスト・ゲスト比較と Android 側の GPU 宣言の採取。"""
from __future__ import annotations

import json
import re
import tempfile
from pathlib import Path

from velox.backends.adb import Adb
from velox.core.util import CmdResult, now_iso, run

FEATURES = {
    "descriptorIndexing": "VK_EXT_descriptor_indexing",
    "bufferDeviceAddress": "VK_KHR_buffer_device_address",
    "synchronization2": "VK_KHR_synchronization2",
    "dynamicRendering": "VK_KHR_dynamic_rendering",
    "shaderFloat16": "VK_KHR_shader_float16_int8",
    "shaderInt8": "VK_KHR_shader_float16_int8",
    "timelineSemaphore": "VK_KHR_timeline_semaphore",
    "subgroupSizeControl": "VK_EXT_subgroup_size_control",
    "cooperativeMatrix": "VK_KHR_cooperative_matrix",
    "rayQuery": "VK_KHR_ray_query",
    "rayTracingPipeline": "VK_KHR_ray_tracing_pipeline",
    "accelerationStructure": "VK_KHR_acceleration_structure",
}

SUBGROUP_OPERATIONS = (
    "BASIC", "VOTE", "ARITHMETIC", "BALLOT", "SHUFFLE", "SHUFFLE_RELATIVE",
    "CLUSTERED", "QUAD",
)


def version(value: int | float | None) -> str | None:
    if value is None:
        return None
    packed = int(value)
    return f"{(packed >> 22) & 0x7f}.{(packed >> 12) & 0x3ff}.{packed & 0xfff}"


def _number(value: str | int | float | None) -> int | None:
    if value is None:
        return None
    return int(value, 0) if isinstance(value, str) else int(value)


def _error(result: CmdResult) -> str:
    return (result.err.strip() or result.out.strip())[:350] or f"exit {result.code}"


def parse_host(text: str) -> list[dict]:
    """vulkaninfo のデバイス単位のセクションを読む。"""
    devices = []
    for block in re.split(r"^GPU\d+:[ \t]*$", text, flags=re.M)[1:]:
        fields = dict(re.findall(r"^\t(\w+)[ \t]*=[ \t]*(.+)$", block, re.M))
        if "deviceName" not in fields or "apiVersion" not in fields:
            continue
        extensions = sorted(re.findall(r"^\t(VK_\w+)[ \t]*:[ \t]*extension revision", block, re.M))
        features = {}
        for section in re.findall(r"^VkPhysicalDevice\w*Features\w*:\n([^\n]*(?:\n\t[^\n]*)*)", block, re.M):
            features.update((name, value == "true") for name, value in re.findall(
                r"^\t(\w+)[ \t]*=[ \t]*(true|false)$", section, re.M))
        heaps = []
        for index, heap in re.findall(r"^\tmemoryHeaps\[(\d+)\]:\n((?:\t\t[^\n]*\n?)*)", block, re.M):
            values = dict(re.findall(r"^\t\t(size|budget|usage)[ \t]*=[ \t]*(\d+)", heap, re.M))
            heaps.append({"index": int(index), "size_bytes": _number(values.get("size")),
                          "budget_bytes": _number(values.get("budget")), "usage_bytes": _number(values.get("usage")),
                          "device_local": "MEMORY_HEAP_DEVICE_LOCAL_BIT" in heap})
        types = [{"index": int(index), "heap_index": int(heap), "property_flags": int(flags, 16)}
                 for index, heap, flags in re.findall(
                     r"^\tmemoryTypes\[(\d+)\]:\n\t\theapIndex\s*=\s*(\d+)\n"
                     r"\t\tpropertyFlags\s*=\s*(0x[0-9a-fA-F]+)", block, re.M)]
        operations = sorted(set(re.findall(r"^\t\tSUBGROUP_FEATURE_(\w+)_BIT[ \t]*$", block, re.M)))
        devices.append({
            "name": fields["deviceName"], "api_version": fields["apiVersion"].split()[0],
            "driver_version": fields.get("driverVersion"), "device_type": fields.get("deviceType"),
            "driver_conformance_version": fields.get("conformanceVersion"),
            "extensions": extensions, "features": {name: features.get(name) for name in FEATURES},
            "subgroup": {"size": _number(fields.get("subgroupSize")), "operations": operations},
            "memory_heaps": heaps, "memory_types": types,
        })
    return devices


def _host() -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        target = Path(tmp) / "vulkaninfo.txt"
        result = run(["vulkaninfo", "--output", str(target)], timeout=60)
        if not result.ok:
            return {"error": _error(result), "devices": []}
        if not target.is_file():
            return {"error": "vulkaninfo の出力ファイルが生成されませんでした", "devices": []}
        devices = parse_host(target.read_text(encoding="utf-8", errors="replace"))
    if not devices:
        return {"error": "vulkaninfo から Vulkan デバイスを解析できませんでした", "devices": []}
    return {"devices": devices, "warnings": result.err.strip().splitlines()[:10]}


def _guest_features(device: dict) -> dict:
    values = {}
    for name, group in device.items():
        if not isinstance(group, dict):
            continue
        if name.startswith(("core", "VK_")):
            groups = (v for k, v in group.items() if "features" in k.lower())
        elif "features" in name.lower():
            groups = (group,)
        else:
            continue
        for features in groups:
            if isinstance(features, dict):
                values.update((k, bool(v)) for k, v in features.items() if v in (0, 1))
    return {name: values.get(name) for name in FEATURES}


def parse_guest(data: dict | list) -> list[dict]:
    """Android vkjson のバージョン・フラグ・64 bit サイズを正規化する。"""
    devices = []
    raw = data if isinstance(data, list) else data["devices"]
    for device in raw:
        properties = device["properties"]
        memory = device.get("memory", {})
        driver = device.get("driverProperties", {}).get("conformanceVersion")
        subgroup = device.get("subgroupProperties", device.get("core11", {}).get("properties", {}))
        ops = _number(subgroup.get("supportedOperations", subgroup.get("subgroupSupportedOperations")))
        devices.append({
            "name": properties["deviceName"], "api_version": version(properties.get("apiVersion")),
            "driver_version": _number(properties.get("driverVersion")), "device_type": _number(properties.get("deviceType")),
            "driver_conformance_version": driver,
            "extensions": sorted({extension["extensionName"] for extension in device.get("extensions", [])}),
            "features": _guest_features(device),
            "subgroup": {"size": _number(subgroup.get("subgroupSize")), "operation_flags": ops,
                         "operations": [name for bit, name in enumerate(SUBGROUP_OPERATIONS)
                                        if ops & (1 << bit)] if ops is not None else None},
            "memory_heaps": [{"index": i, "size_bytes": _number(h.get("size")),
                              "device_local": bool(int(h["flags"]) & 1), "budget_bytes": None, "usage_bytes": None}
                             for i, h in enumerate(memory.get("memoryHeaps", []))],
            "memory_types": [{"index": i, "heap_index": int(t["heapIndex"]), "property_flags": int(t["propertyFlags"])}
                             for i, t in enumerate(memory.get("memoryTypes", []))],
        })
    return devices


def _guest(adb: Adb) -> dict:
    result = adb.shell("cmd gpu vkjson", timeout=60)
    data = {"devices": []}
    if result.ok:
        try:
            data["devices"] = parse_guest(json.loads(result.out))
        except (KeyError, ValueError, TypeError) as exc:
            data["error"] = f"vkjson を解析できませんでした: {exc}"
    else:
        data["error"] = _error(result)
    if not data["devices"] and "error" not in data:
        data["error"] = "Vulkan デバイスが列挙されませんでした"

    profiles = adb.shell("cmd gpu vkprofiles", timeout=60)
    try:
        entries = json.loads(profiles.out) if profiles.ok else None
    except json.JSONDecodeError:
        entries = None
    data["android_profiles"] = (
        {name: {"supported": result == "SUPPORTED", "result": result} for name, result in entries.items()}
        if isinstance(entries, dict) and entries else {"error": _error(profiles)})

    features = adb.shell("pm list features", timeout=25)
    sf = adb.shell("dumpsys SurfaceFlinger | grep -m 1 'GLES:'", timeout=25)
    env = adb.shell("getprop ro.opengles.version; getprop ro.build.version.release; "
                    "grep '^MemTotal:' /proc/meminfo; getconf PAGE_SIZE", timeout=15)
    declared = dict((line.removeprefix("feature:").partition("=")[0],
                     line.partition("=")[2] or True)
                    for line in features.out.splitlines() if line.startswith("feature:")
                    and any(name in line for name in ("vulkan", "opengles", "reqGlEsVersion")))
    v = declared.get("android.hardware.vulkan.version")
    packed = re.fullmatch(r"(\d*)\n([^\n]*)\nMemTotal:\s*(\d+)\s+kB\n(\d+)", "\n".join(env.out.splitlines()))
    data["android"] = {
        "features": declared if features.ok else None,
        "declared_vulkan_version": version(int(v)) if isinstance(v, str) and v.isdecimal() else None,
        "gles_surfaceflinger": sf.out.strip() or None,
        "gles_declared": f"{int(packed[1]) >> 16}.{int(packed[1]) & 0xffff}" if packed and packed[1] else None,
        "release": packed[2] if packed else None,
        "memtotal_kib": int(packed[3]) if packed else None,
        "page_size_bytes": int(packed[4]) if packed else None,
    }
    return data


def _compare(host: dict, guest: dict) -> list[dict]:
    pairs = []
    for h in host["devices"]:
        for g in guest["devices"]:
            host_ext, guest_ext = set(h["extensions"]), set(g["extensions"])
            pairs.append({
                "host": h["name"], "guest": g["name"],
                "host_only_extensions": sorted(host_ext - guest_ext),
                "guest_only_extensions": sorted(guest_ext - host_ext),
                "shared_extensions": sorted(host_ext & guest_ext),
                "features": {key: {"host": h["features"][key], "guest": g["features"][key],
                                   "host_extension": ext in host_ext, "guest_extension": ext in guest_ext}
                             for key, ext in FEATURES.items()},
            })
    return pairs


def diagnose(adb: Adb | None, runtime: dict | None = None) -> dict:
    host = _host()
    guest = _guest(adb) if adb else None
    return {"schema_version": 1, "collected_at": now_iso(), "host": host, "guest": guest,
            "emulator_command": (runtime or {}).get("command"), "emulator_affinity": (runtime or {}).get("affinity"),
            "comparisons": _compare(host, guest) if guest else [],
            "validation": {"command_execution": "not_tested", "cts_deqp": "not_tested"}}


def markdown(report: dict) -> str:
    def state(value):
        return {True: "対応", False: "非対応", None: "未取得"}[value]

    lines = ["# GPU 診断", "", f"採取日時 {report['collected_at']}", "",
             "ドライバーの照会値と Android 標準のプロファイル照会結果です。"
             " GPU コマンドの実行・結果検証および CTS / dEQP はこの診断の対象外です。", ""]
    for label, side in (("Windows ホスト", report["host"]), ("Android ゲスト", report["guest"])):
        lines.extend([f"## {label}", ""])
        if not side:
            lines.extend(["未採取", ""])
            continue
        if side.get("error"):
            lines.extend([side["error"], ""])
        for device in side["devices"]:
            lines.extend([f"### {device['name']}", "", f"Vulkan {device['api_version']} ／ 拡張 "
                          f"{len(device['extensions'])} 件", "",
                          f"サブグループサイズ {device['subgroup']['size']} ／ "
                          + (", ".join(device["subgroup"]["operations"] or []) or "列挙値なし"), "",
                          "| Heap | 容量 (bytes) | 予算 (bytes) | Device local |",
                          "|---:|---:|---:|---|"])
            for h in device["memory_heaps"]:
                lines.append(f"| {h['index']} | {h['size_bytes']} | "
                             f"{h['budget_bytes'] if h['budget_bytes'] is not None else '未取得'} | {state(h['device_local'])} |")
            lines.extend(["", "| Memory type | Heap | Property flags |", "|---:|---:|---:|"])
            for t in device["memory_types"]:
                lines.append(f"| {t['index']} | {t['heap_index']} | `0x{t['property_flags']:x}` |")
            lines.append("")
        if side.get("android"):
            a = side["android"]
            lines.extend([f"- Android {a['release']} ／ RAM {a['memtotal_kib']} KiB ／ ページ {a['page_size_bytes']} B",
                          f"- システム宣言 Vulkan {a['declared_vulkan_version']} ／ GLES {a['gles_declared']}",
                          f"- SurfaceFlinger `{a['gles_surfaceflinger'] or '未取得'}`", ""])
            for name, p in side["android_profiles"].items():
                status = f"{state(p['supported'])} ／ {p['result']}" if isinstance(p, dict) else p
                lines.append(f"- {name} ／ {status}")
            lines.append("")
    for pair in report["comparisons"]:
        lines.extend([f"## {pair['host']} と {pair['guest']}", "",
                      "| 機能 | ホスト feature | ゲスト feature | ホスト拡張 | ゲスト拡張 |",
                      "|---|---|---|---|---|"])
        for name, f in pair["features"].items():
            lines.append(f"| `{name}` | {state(f['host'])} | {state(f['guest'])} | "
                         f"{state(f['host_extension'])} | {state(f['guest_extension'])} |")
        lines.append("")
    return "\n".join(lines)
