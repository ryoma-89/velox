"""`emu` CLI。GUI と同じバックエンド（velox.*）を直接呼ぶ薄い層。

終了コード:
  0 成功 / 1 実行時エラー / 2 引数・設定エラー / 3 前提未充足（未起動・未導入）
  4 ベンチマーク失敗・未完走
--json 指定時は結果を JSON で標準出力に出す（AI エージェント向け）。
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

from velox.core import config as cfgmod
from velox.core import paths
from velox.core.util import dumps, setup_stdio

EXIT_OK, EXIT_ERR, EXIT_USAGE, EXIT_NOT_READY, EXIT_BENCH = 0, 1, 2, 3, 4


class CliExit(Exception):
    def __init__(self, code: int, msg: str, data=None):
        super().__init__(msg)
        self.code, self.msg, self.data = code, msg, data


def out(args, data, human: str | None = None) -> None:
    if args.json:
        print(dumps(data))
    else:
        print(human if human is not None else dumps(data))


# ---- コマンド実装 ----------------------------------------------------------

def cmd_diagnose(a):
    from velox.hw.diagnose import run_diagnose
    r = run_diagnose()
    paths.ensure()
    (paths.RESULTS / "diagnose-latest.json").write_text(dumps(r), encoding="utf-8")
    if a.json:
        return out(a, r)
    t = r["cpu"]["topology"]
    lines = [f"CPU   : {r['cpu']['name']}  P={len(t['p_cores'])} {t['p_cores']}  E={len(t['e_cores'])}",
             f"GPU   : {', '.join(g['name'] + ' / driver ' + g['driver_version'] for g in r['gpu']) or '-'}",
             f"Vulkan: {r['vulkan'].get('instance_version')}  拡張: {', '.join(r['vulkan'].get('notable_device_extensions', []))}",
             f"Win   : hypervisor={r['windows']['hypervisor_present']} VBS={r['windows']['vbs_status']} {r['windows']['vbs_services_running']}",
             f"SDK   : {r['sdk']['root'] if r['sdk'] else '-'}  emulator={r['sdk'] and r['sdk']['emulator_version']}",
             f"Images: {[i['sysdir'] for i in (r['sdk'] or {}).get('system_images', [])]}",
             "警告:"] + [f"  - {w}" for w in r["warnings"]]
    out(a, r, "\n".join(lines))


def _emu():
    from velox.backends.aosp import AospEmulator, BackendError
    try:
        return AospEmulator()
    except BackendError as e:
        raise CliExit(EXIT_NOT_READY, str(e))


def cmd_start(a):
    from velox.backends.aosp import BackendError
    cfg = cfgmod.load_profile(a.profile) if a.profile else cfgmod.load_active()
    if a.headless:
        cfg["boot"]["window"] = False
    try:
        info = _emu().start(cfg, wait=not a.no_wait, timeout=a.timeout)
    except BackendError as e:
        raise CliExit(EXIT_NOT_READY if "既に起動" in str(e) or "イメージ" in str(e) else EXIT_ERR, str(e))
    if "boot_error" in info:
        raise CliExit(EXIT_ERR, info["boot_error"], info)
    out(a, info, f"起動しました: {info['serial']} pid={info['pid']} affinity={info['affinity']} "
                 f"boot={info.get('boot_seconds_total', '-')}s")


def cmd_stop(a):
    r = _emu().stop()
    out(a, r, "停止しました" if r["stopped"] else f"停止できませんでした: {r}")
    if not r["stopped"]:
        raise CliExit(EXIT_ERR, "stop failed", r)


def cmd_status(a):
    st = _emu().status()
    if a.json:
        return out(a, st)
    if not st["running"]:
        return out(a, st, "停止中")
    g = st.get("guest", {})
    out(a, st, f"稼働中 pids={st['vm_pids']} boot_completed={st.get('boot_completed')}\n"
               f"Android {g.get('props', {}).get('ro.build.version.release')} ABI={g.get('props', {}).get('ro.product.cpu.abilist')}\n"
               f"Renderer: {g.get('surfaceflinger')}\nソフトウェア描画疑い: {g.get('software_rendering_suspected')}"
               + _frame_rate_text(st.get("frame_rate")))


def _frame_rate_text(fr: dict | None) -> str:
    """設定値（configured）と実際の状態（observed）を分けて表示する。"""
    if not fr:
        return ""
    c, o = fr.get("configured", {}), fr.get("observed", {})
    want = "端末既定（ゲームは 60 fps）" if c.get("render_hz") is None else f"{c.get('render_hz')} Hz"
    return (f"\nフレームレート: 設定 {c.get('mode')}（描画 {want}、起動時の適用 {fr.get('applied_at_start')}）"
            f"\n  実際: 表示モード {o.get('display_mode_hz')} Hz、描画 {o.get('render_hz')} Hz、"
            f"ゲーム向け上書き {o.get('game_overrides') or 'なし'}、"
            f"peak/min {o.get('peak_refresh_rate')}/{o.get('min_refresh_rate')}")


def cmd_gpu_diagnose(a):
    from velox.backends.adb import Adb, AdbError
    from velox.hw import gpu

    runtime = {}
    if not a.host_only:
        emu = _emu()
        runtime = emu.runtime() or {}
        if not runtime.get("serial") or not emu.vm_pids():
            raise CliExit(EXIT_NOT_READY, "Velox を起動するか --host-only を指定してください")
        try:
            adb = Adb(runtime["serial"])
        except AdbError as exc:
            raise CliExit(EXIT_NOT_READY, str(exc)) from exc
        if not adb.boot_completed():
            raise CliExit(EXIT_NOT_READY, "Android の起動完了を確認できません")
    else:
        adb = None
    report = gpu.diagnose(adb, runtime)
    directory = a.output_dir
    try:
        directory.mkdir(parents=True, exist_ok=True)
        json_path, md_path = directory / "gpu.json", directory / "gpu.md"
        json_path.write_text(dumps(report) + "\n", encoding="utf-8")
        md_path.write_text(gpu.markdown(report), encoding="utf-8")
    except OSError as exc:
        raise CliExit(EXIT_ERR, f"診断の保存に失敗しました: {exc}") from exc
    errors = [side["error"] for side in (report["host"], report["guest"]) if side and side.get("error")]
    if errors:
        raise CliExit(EXIT_ERR, "\n".join(errors), report)
    out(a, report, f"GPU 診断を保存しました\n{json_path}\n{md_path}")


def cmd_install(a):
    from velox.backends.adb import Adb
    from velox.bench.antutu import sha256
    st = _emu().status()
    if not st.get("boot_completed"):
        raise CliExit(EXIT_NOT_READY, "エミュレーターが起動していません")
    r = Adb(st["runtime"]["serial"]).install(a.apk)
    rec = {"apk": a.apk, "sha256": sha256(a.apk), "source": a.source, "result": (r.out + r.err).strip()}
    paths.ensure()
    with open(paths.RESULTS / "apk-provenance.jsonl", "a", encoding="utf-8") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    if "Success" not in rec["result"]:
        raise CliExit(EXIT_ERR, rec["result"], rec)
    out(a, rec, f"インストール成功 sha256={rec['sha256'][:16]}…")


def cmd_config(a):
    cfg = cfgmod.load_active()
    if a.action == "get":
        return out(a, cfgmod.get_key(cfg, a.key) if a.key else cfg)
    if a.action == "set":
        new = cfgmod.set_key(cfg, a.key, cfgmod.parse_value(a.value))
        fp = cfgmod.save_active(new, reason=f"config set {a.key}={a.value}")
        return out(a, {"key": a.key, "value": cfgmod.get_key(new, a.key), "fingerprint": fp},
                   f"{a.key} = {cfgmod.get_key(new, a.key)!r}（次回起動時に反映）")
    if a.action == "reset":
        fp = cfgmod.save_active(cfgmod.DEFAULT, reason="config reset")
        return out(a, {"fingerprint": fp}, "既定値に戻しました")


def cmd_benchmark(a):
    from velox.bench import runner, store
    if a.action == "run":
        try:
            r = runner.run_suite(a.suite, reps=a.reps, warmup=a.warmup, tag=a.tag or "")
        except runner.RunAborted as e:
            raise CliExit(EXIT_NOT_READY, str(e))
        except Exception as e:
            if type(e).__name__ == "AntutuNotReady":
                raise CliExit(EXIT_NOT_READY, str(e))
            raise
        if a.json:
            out(a, {k: v for k, v in r.items() if k != "profile"})
        else:
            print(f"run {r['id']} [{r['suite']}] status={r['status']} ({r.get('duration_s', '-')}s)")
            for n, m in r.get("metrics", {}).items():
                s = m["stats"]
                print(f"  {n:16s} median={s.get('median')} {m['unit']}  mean={s.get('mean')} sd={s.get('stdev')} cv={s.get('cv_pct')}%")
        if r["status"] != "ok":
            raise CliExit(EXIT_BENCH, r.get("error", r["status"]), None)
    elif a.action == "history":
        rows = store.load(limit=a.limit, suite=a.suite)
        data = [{"id": r["id"], "suite": r["suite"], "at": r["started_at"], "status": r["status"], "tag": r.get("tag"),
                 "fingerprint": r.get("profile_fingerprint"),
                 "medians": {n: m["stats"].get("median") for n, m in r.get("metrics", {}).items()}} for r in rows]
        out(a, data, "\n".join(f"{d['id']} {d['at']} {d['suite']:6s} {d['status']:10s} {d['fingerprint']} {d['medians']}" for d in data) or "(履歴なし)")
    elif a.action == "compare":
        ra, rb = store.get(a.a), store.get(a.b)
        if not ra or not rb:
            raise CliExit(EXIT_USAGE, "run id が見つかりません")
        if ra.get("suite") == rb.get("suite") == "antutu":
            versions = [run.get("env", {}).get("antutu", {}).get("version") for run in (ra, rb)]
            variants = [set(variant for rep in run.get("details", []) for variant in rep.get("3d_started", []))
                        for run in (ra, rb)]
            if (not versions[0] or versions[0] != versions[1] or len(variants[0]) != 1
                    or variants[0] != variants[1] or any(run["status"] != "ok" for run in (ra, rb))):
                raise CliExit(EXIT_USAGE, "AnTuTu は完走した同じバージョン・同じ 3D 版の測定だけ比較できます")
        c = store.compare(ra, rb)
        if not c["metrics"]:
            raise CliExit(EXIT_USAGE, "比較可能な測定値がありません")
        out(a, c, "\n".join([f"{n:16s} {m['before']} → {m['after']} {m['unit']}  {m['improvement_pct']:+.2f}% (noise {m['noise_pct']}%)"
                             for n, m in c["metrics"].items()] + [f"幾何平均: {c['geomean_improvement_pct']:+.2f}%"]))


def cmd_optimize(a):
    from velox.optimize import search
    if a.action == "start":
        st = search.read_state()
        if st.get("status") == "running" and _pid_alive(st.get("pid")):
            raise CliExit(EXIT_ERR, f"最適化は既に実行中です (pid={st['pid']})")
        paths.ensure()
        log = open(paths.LOGS / f"optimize-{time.strftime('%Y%m%d-%H%M%S')}.log", "wb")
        argv = [sys.executable, "-m", "velox", "optimize", "run", "--suite", a.suite, "--reps", str(a.reps),
                "--max-trials", str(a.max_trials), "--max-minutes", str(a.max_minutes),
                "--min-gain", str(a.min_gain), "--strategy", a.strategy]
        flags = 0x00000008 | 0x00000200 if sys.platform == "win32" else 0  # DETACHED | NEW_GROUP
        p = subprocess.Popen(argv, cwd=str(paths.ROOT), stdout=log, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, creationflags=flags)
        out(a, {"pid": p.pid, "log": log.name}, f"最適化を開始しました pid={p.pid} log={log.name}")
    elif a.action == "run":
        st = search.optimize(suite=a.suite, reps=a.reps, max_trials=a.max_trials, max_minutes=a.max_minutes,
                             min_gain_pct=a.min_gain, strategy=a.strategy)
        out(a, st)
        if st["status"] != "finished":
            raise CliExit(EXIT_BENCH, st.get("error", "optimize failed"))
    elif a.action == "stop":
        search.request_stop()
        out(a, {"requested": True}, "停止を要求しました（現在の試行完了後に停止）")
    elif a.action == "status":
        out(a, search.read_state())
    elif a.action == "history":
        out(a, search.history(a.limit))


def _pid_alive(pid) -> bool:
    if not pid:
        return False
    from velox.hw.process import snapshot
    return any(p == pid for p, _pp, _n in snapshot())


def cmd_profile(a):
    if a.action == "save":
        p = cfgmod.save_profile(a.name, cfgmod.load_active())
        out(a, {"saved": str(p)}, f"保存: {p}")
    elif a.action == "load":
        cfg = cfgmod.load_profile(a.name)
        fp = cfgmod.save_active(cfg, reason=f"profile load {a.name}")
        out(a, {"loaded": a.name, "fingerprint": fp}, f"読み込み: {a.name}")
    elif a.action == "list":
        out(a, cfgmod.list_profiles(), "\n".join(cfgmod.list_profiles()) or "(なし)")


def cmd_telemetry(a):
    from velox import telemetry
    if a.threads:
        # 役割ごとのスレッドの CPU 使用率と affinity（第 13 段階、ソース版のホストが名前を付ける）
        from velox.backends.aosp import AospEmulator
        from velox.hw import thread_roles as threads
        pids = AospEmulator().vm_pids()
        if not pids:
            raise SystemExit(3)
        res = threads.role_summary(pids[0], a.interval)
        lines = [f"{r:10s} threads={v['threads']:3d} cpu={v['cpu_pct']:6.1f}% busiest={v['busiest_pct']:5.1f}% "
                 f"affinity={','.join(v['affinity'])}" for r, v in sorted(res["roles"].items())]
        out(a, res, "\n".join(lines))
        return
    n = 0
    while True:
        s = telemetry.sample(a.interval)
        if a.json:
            print(json.dumps(s, ensure_ascii=False), flush=True)
        else:
            c, g, m = s.get("cpu", {}), s.get("gpu", {}), s.get("memory", {})
            print(f"{s['t']} CPU {c.get('total')}% (P {c.get('p_avg')}% / E {c.get('e_avg')}%)  "
                  f"GPU {g.get('utilization.gpu')}% {g.get('temperature.gpu')}C {g.get('power.draw')}W "
                  f"VRAM {g.get('memory.used')}MB  RAM {m.get('used_mb')}MB", flush=True)
        n += 1
        if not a.watch or (a.count and n >= a.count):
            break


def cmd_rollback(a):
    cfg = cfgmod.rollback(a.steps)
    out(a, {"fingerprint": cfgmod.fingerprint(cfg)}, f"{a.steps} 段階前の設定に戻しました ({cfgmod.fingerprint(cfg)})")


def cmd_sdk(a):
    from velox.backends.sdk import find_sdk
    sdk = find_sdk()
    if not sdk:
        raise CliExit(EXIT_NOT_READY, "Android SDK が見つかりません")
    data = {"root": str(sdk.root), "emulator": sdk.emulator_version(), "platform_tools": sdk.platform_tools_version(),
            "system_images": sdk.system_images()}
    out(a, data)


def cmd_recommend(a):
    from velox.optimize import recommend
    base = cfgmod.load_active() if a.from_active else None
    rec = recommend.recommend_config(mode=a.mode, base=base, api_level=a.api_level)
    if a.apply:
        rec["fingerprint"] = cfgmod.save_active(rec["config"], reason=f"recommend {a.mode}")
    out(a, rec)


def cmd_gui(a):
    from velox.gui import launch
    rc = launch(initial_profile=a.profile)
    if rc != 0:
        raise CliExit(EXIT_ERR, "GUI が異常終了しました")


# ---- 引数定義 --------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="emu", description="Velox Android エミュレーター管理・最適化 CLI")
    p.add_argument("--json", action="store_true", help="JSON で出力")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("diagnose", help="ホスト環境診断").set_defaults(fn=cmd_diagnose)
    s = sub.add_parser("start", help="エミュレーター起動"); s.set_defaults(fn=cmd_start)
    s.add_argument("--profile"); s.add_argument("--no-wait", action="store_true")
    s.add_argument("--headless", action="store_true"); s.add_argument("--timeout", type=float, default=420)
    sub.add_parser("stop", help="停止").set_defaults(fn=cmd_stop)
    sub.add_parser("status", help="状態").set_defaults(fn=cmd_status)
    s = sub.add_parser("gpu-diagnose", help="ホストと Android の GPU 機能を比較")
    s.add_argument("--host-only", action="store_true", help="Windows ホストのみ採取")
    s.add_argument("--output-dir", type=Path, default=paths.STATE / "gpu")
    s.set_defaults(fn=cmd_gpu_diagnose)
    s = sub.add_parser("install", help="APK インストール（入手元を記録）"); s.set_defaults(fn=cmd_install)
    s.add_argument("apk"); s.add_argument("--source", required=True, help="APK の入手元 URL")

    s = sub.add_parser("config", help="設定"); s.set_defaults(fn=cmd_config)
    cs = s.add_subparsers(dest="action", required=True)
    g = cs.add_parser("get"); g.add_argument("key", nargs="?")
    g = cs.add_parser("set"); g.add_argument("key"); g.add_argument("value")
    cs.add_parser("reset")

    s = sub.add_parser("benchmark", help="ベンチマーク"); s.set_defaults(fn=cmd_benchmark)
    bs = s.add_subparsers(dest="action", required=True)
    g = bs.add_parser("run"); g.add_argument("--suite", default="micro", choices=["quick", "micro", "antutu"])
    g.add_argument("--reps", type=int, default=3); g.add_argument("--warmup", type=int, default=1); g.add_argument("--tag")
    g = bs.add_parser("history"); g.add_argument("--limit", type=int, default=20)
    g.add_argument("--suite")
    g = bs.add_parser("compare"); g.add_argument("a"); g.add_argument("b")

    s = sub.add_parser("optimize", help="自動最適化"); s.set_defaults(fn=cmd_optimize)
    os_ = s.add_subparsers(dest="action", required=True)
    for name in ("start", "run"):
        g = os_.add_parser(name)
        g.add_argument("--suite", default="quick", choices=["quick", "micro"])
        g.add_argument("--reps", type=int, default=3); g.add_argument("--max-trials", type=int, default=20)
        g.add_argument("--max-minutes", type=float, default=180); g.add_argument("--min-gain", type=float, default=2.0)
        g.add_argument("--strategy", default="coordinate", choices=["coordinate", "random"])
    os_.add_parser("stop"); os_.add_parser("status")
    g = os_.add_parser("history"); g.add_argument("--limit", type=int, default=50)

    s = sub.add_parser("profile", help="設定プロファイル"); s.set_defaults(fn=cmd_profile)
    ps = s.add_subparsers(dest="action", required=True)
    ps.add_parser("save").add_argument("name"); ps.add_parser("load").add_argument("name"); ps.add_parser("list")

    s = sub.add_parser("telemetry", help="ホストテレメトリー"); s.set_defaults(fn=cmd_telemetry)
    s.add_argument("--watch", action="store_true"); s.add_argument("--interval", type=float, default=1.0)
    s.add_argument("--count", type=int, default=0)
    s.add_argument("--threads", action="store_true", help="エミュレーターのスレッドを役割ごとに集計する")
    s = sub.add_parser("rollback", help="設定を前の状態に戻す"); s.set_defaults(fn=cmd_rollback)
    s.add_argument("--steps", type=int, default=1)
    s = sub.add_parser("sdk", help="SDK / システムイメージ情報"); s.set_defaults(fn=cmd_sdk)
    s = sub.add_parser("recommend", help="ハードウェア非依存の推奨プロファイル生成"); s.set_defaults(fn=cmd_recommend)
    s.add_argument("--mode", default="gaming", choices=["gaming", "compute", "balanced", "low_power"])
    s.add_argument("--api-level", type=int, default=None)
    s.add_argument("--from-active", action="store_true")
    s.add_argument("--apply", action="store_true")
    s = sub.add_parser("gui", help="軽量コントロールパネル GUI を起動"); s.set_defaults(fn=cmd_gui)
    s.add_argument("--profile", default=None)
    return p


def main(argv: list[str] | None = None) -> int:
    setup_stdio()
    parser = build_parser()
    try:
        a = parser.parse_args(argv)
    except SystemExit as e:
        return EXIT_USAGE if e.code else EXIT_OK
    try:
        a.fn(a)
        return EXIT_OK
    except CliExit as e:
        if a.json:
            print(dumps({"error": e.msg, "code": e.code, "data": e.data}))
        else:
            print(f"エラー: {e.msg}", file=sys.stderr)
        return e.code
    except cfgmod.ConfigError as e:
        print(dumps({"error": str(e), "code": EXIT_USAGE}) if a.json else f"設定エラー: {e}", file=sys.stdout if a.json else sys.stderr)
        return EXIT_USAGE
    except KeyboardInterrupt:
        return 130
