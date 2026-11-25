import type { LocalizedText } from './schema';

export interface DirectComparisonRow {
  item: LocalizedText;
  officialSdk: LocalizedText;
  veloxSame6: LocalizedText;
  veloxFull20: LocalizedText;
  deltaNote: LocalizedText;
  source: string;
  honestCaveat?: LocalizedText;
}

export interface BreakthroughCard {
  id: string;
  metric: LocalizedText;
  headline: LocalizedText;
  before: LocalizedText;
  after: LocalizedText;
  target: LocalizedText;
  conditions: LocalizedText;
  mechanism: LocalizedText;
  constraint: LocalizedText;
  source: string;
}

export interface TechItem {
  name: LocalizedText;
  origin: 'upstream' | 'velox_original' | 'velox_patch';
  patchRef?: string;
  conventional: LocalizedText;
  veloxChange: LocalizedText;
  measuredEffect: LocalizedText;
}

export interface TechPillar {
  id: string;
  title: LocalizedText;
  summary: LocalizedText;
  items: TechItem[];
}

export interface TimelineEntry {
  phase: string;
  date: string;
  title: LocalizedText;
  summary: LocalizedText;
  metrics: LocalizedText[];
}

export interface MumuRow {
  metric: LocalizedText;
  veloxHost: string;
  veloxGa: string;
  mumu: string;
  winner: 'velox' | 'mumu' | 'split';
  note: LocalizedText;
}

export const OFFICIAL_VS_VELOX_20_ITEMS: DirectComparisonRow[] = [
  {
    item: { ja: '1. AnTuTu V11 Standard GPU', en: '1. AnTuTu V11 Standard GPU' },
    officialSdk: { ja: '未完走 (3D 負荷中タイムアウト)', en: 'Did not complete (3D load timeout)' },
    veloxSame6: { ja: '— (60Hz 20vCPU 基準で比較)', en: '— (measured on 20 vCPU 60Hz)' },
    veloxFull20: { ja: '655,918 pts (3/3 完走・正常判定)', en: '655,918 pts (3/3 complete, normal)' },
    deltaNote: { ja: '開発初期 235,973 比 2.78倍 (RT 新規対応含む)', en: '2.78x vs early Velox 235,973 (incl. new RT)' },
    source: 'final_antutu_ab.json / baselines/sdk_std.json',
    honestCaveat: { ja: '公式 SDK 6 vCPU は未完走。AnTuTu 総合/CPU/MEM は異常判定付き参考値。', en: 'Official SDK 6 vCPU timed out. AnTuTu Total/CPU/MEM carry abnormal flags.' },
  },
  {
    item: { ja: '2. Vulkan Draw Call (vk-render draws)', en: '2. Vulkan Draw Call (vk-render draws)' },
    officialSdk: { ja: '299.81 fps', en: '299.81 fps' },
    veloxSame6: { ja: '1,603.80 fps', en: '1,603.80 fps' },
    veloxFull20: { ja: '1,575.58–1,596.2 fps (最大 1,615.6)', en: '1,575.58–1,596.2 fps (peak 1,615.6)' },
    deltaNote: { ja: '同 6 vCPU 比 5.35倍 (ネイティブ比 93.9%)', en: '5.35x at equal 6 vCPU (93.9% native)' },
    source: 'sdk_vs_velox_direct.json / final_regression_phase20.json',
  },
  {
    item: { ja: '3. Vulkan 同期 (vk-render sync)', en: '3. Vulkan Sync (vk-render sync)' },
    officialSdk: { ja: '4,339.95 fps', en: '4,339.95 fps' },
    veloxSame6: { ja: '12,712.32 fps', en: '12,712.32 fps' },
    veloxFull20: { ja: '12,533–12,841.5 fps', en: '12,533–12,841.5 fps' },
    deltaNote: { ja: '同 6 vCPU 比 2.93倍 (最大ネイティブ比 88.4%)', en: '2.93x at equal 6 vCPU (up to 88.4% native)' },
    source: 'sdk_vs_velox_direct.json / step4_5',
  },
  {
    item: { ja: '4. Fence 待機・照会・リセット', en: '4. Fence Wait / Poll / Reset' },
    officialSdk: { ja: 'Wait 84.6 µs / Sig 37.4 µs / Reset 37.5 µs', en: 'Wait 84.6 µs / Sig 37.4 µs / Reset 37.5 µs' },
    veloxSame6: { ja: 'Wait 3.94 µs / Sig 0.20 µs / Reset 1.13 µs', en: 'Wait 3.94 µs / Sig 0.20 µs / Reset 1.13 µs' },
    veloxFull20: { ja: 'Wait 4.05 µs (min 3.91) / Sig 0.20 µs / Reset 1.13 µs', en: 'Wait 4.05 µs (min 3.91) / Sig 0.20 µs / Reset 1.13 µs' },
    deltaNote: { ja: '空 Submit+Wait 21.5倍 / 完了照会 190倍 / Reset 33.1倍短縮', en: '21.5x submit+wait / 190x signaled poll / 33.1x reset' },
    source: 'sdk_vs_velox_direct.json',
  },
  {
    item: { ja: '5. Descriptor Set 処理 (1 フレーム相当)', en: '5. Descriptor Set Processing (frame cycle)' },
    officialSdk: { ja: '1,652.8 µs (Reset 空: 38.0 µs)', en: '1,652.8 µs (empty reset: 38.0 µs)' },
    veloxSame6: { ja: '30.85 µs (Reset 空: 0.19 µs)', en: '30.85 µs (empty reset: 0.19 µs)' },
    veloxFull20: { ja: '31.24 µs (Reset 空: 0.19 µs)', en: '31.24 µs (empty reset: 0.19 µs)' },
    deltaNote: { ja: 'Reset+Alloc32+Update 53.6倍短縮', en: '53.6x faster Reset+Alloc32+Update' },
    source: 'sdk_vs_velox_direct.json',
  },
  {
    item: { ja: '6. GPU メモリアップロード (64 MiB Type 2)', en: '6. GPU Memory Upload (64 MiB Type 2)' },
    officialSdk: { ja: 'Direct 0.32 / E2E 0.31 GB/s', en: 'Direct 0.32 / E2E 0.31 GB/s' },
    veloxSame6: { ja: 'Direct 20.54 / E2E 14.14 GB/s', en: 'Direct 20.54 / E2E 14.14 GB/s' },
    veloxFull20: { ja: 'Direct 19.83 / E2E 13.35–14.13 GB/s', en: 'Direct 19.83 / E2E 13.35–14.13 GB/s' },
    deltaNote: { ja: 'Staging→VRAM E2E 41.5–45.6倍 (ネイティブ比 85.9%)', en: '41.5–45.6x Staging→VRAM E2E (85.9% native)' },
    source: 'sdk_vs_velox_direct.json / phase18/step6-gpu-memory.json',
    honestCaveat: { ja: '特定アップロード経路の改善であり GPU 全体の倍率ではない。', en: 'Specific to staging upload path, not overall GPU speed.' },
  },
  {
    item: { ja: '7. GPU 読み戻し (64 MiB Readback E2E)', en: '7. GPU Readback (64 MiB E2E)' },
    officialSdk: { ja: '11.33 GB/s', en: '11.33 GB/s' },
    veloxSame6: { ja: '11.16 GB/s', en: '11.16 GB/s' },
    veloxFull20: { ja: '10.74–11.24 GB/s', en: '10.74–11.24 GB/s' },
    deltaNote: { ja: '概ね同等（ネイティブ比 80.4%）', en: 'Broadly at parity (~80.4% native)' },
    source: 'sdk_vs_velox_direct.json / final_regression_phase20.json',
  },
  {
    item: { ja: '8. コマンド転送 RPC レイテンシ', en: '8. Command Transport RPC Latency' },
    officialSdk: { ja: '40.14 µs (pipe)', en: '40.14 µs (pipe)' },
    veloxSame6: { ja: '0.85 µs (ASG)', en: '0.85 µs (ASG)' },
    veloxFull20: { ja: '0.80 µs (ASG)', en: '0.80 µs (ASG)' },
    deltaNote: { ja: '同 6 vCPU 比 47.2倍 / 20 vCPU 比 50.1倍短縮', en: '47.2x faster (6 vCPU) / 50.1x faster (20 vCPU)' },
    source: 'sdk_vs_velox_direct.json',
  },
  {
    item: { ja: '9. CPU 整数演算 (1T)', en: '9. CPU Integer (1T)' },
    officialSdk: { ja: '1.432 Gops/s', en: '1.432 Gops/s' },
    veloxSame6: { ja: '1.439 Gops/s', en: '1.439 Gops/s' },
    veloxFull20: { ja: '1.437–1.444 Gops/s', en: '1.437–1.444 Gops/s' },
    deltaNote: { ja: '同等〜+0.8%（ネイティブ比 97.7%）', en: 'Parity to +0.8% (97.7% native)' },
    source: 'sdk_vs_velox_direct.json / step4_5',
  },
  {
    item: { ja: '10. マルチスレッド CPU 性能 (MT)', en: '10. Multi-Thread CPU Throughput (MT)' },
    officialSdk: { ja: '8.602 Gops/s (6 vCPU 上限)', en: '8.602 Gops/s (6 vCPU cap)' },
    veloxSame6: { ja: '8.611 Gops/s (6 vCPU)', en: '8.611 Gops/s (6 vCPU)' },
    veloxFull20: { ja: '23.03–23.35 Gops/s (20 vCPU)', en: '23.03–23.35 Gops/s (20 vCPU)' },
    deltaNote: { ja: '同コア同等 / 20 vCPU 解放で 2.71倍 (ネイティブ比 95.1%)', en: 'Parity at 6T / 2.71x at 20 vCPU (95.1% native)' },
    source: 'sdk_vs_velox_direct.json / step4_5',
  },
  {
    item: { ja: '11. RAM 帯域 (64M 1T / MT)', en: '11. RAM Bandwidth (64M 1T / MT)' },
    officialSdk: { ja: '15.77 / 38.33 GiB/s (6T)', en: '15.77 / 38.33 GiB/s (6T)' },
    veloxSame6: { ja: '19.07 / 39.25 GiB/s (6T)', en: '19.07 / 39.25 GiB/s (6T)' },
    veloxFull20: { ja: '19.58 (8P 19.76) / 40.83 GiB/s (20T)', en: '19.58 (8P 19.76) / 40.83 GiB/s (20T)' },
    deltaNote: { ja: '1T +20.9〜24.1% (Guest THP 効果、256M MT ネイティブ比 98.0%)', en: '1T +20.9–24.1% via Guest THP (256M MT 98.0% native)' },
    source: 'sdk_vs_velox_direct.json / step4_5',
  },
  {
    item: { ja: '12. RAM ランダム参照レイテンシ (64 MiB)', en: '12. RAM Random Latency (64 MiB)' },
    officialSdk: { ja: '118.74 ns', en: '118.74 ns' },
    veloxSame6: { ja: '111.05 ns', en: '111.05 ns' },
    veloxFull20: { ja: '108.77 ns', en: '108.77 ns' },
    deltaNote: { ja: '-6.5%〜-8.4% 短縮 (ネイティブ 101.60 ns)', en: '-6.5% to -8.4% faster (native 101.60 ns)' },
    source: 'sdk_vs_velox_direct.json',
  },
  {
    item: { ja: '13. ストレージ I/O (128M 順次 / 4K 永続)', en: '13. Storage I/O (128M Seq / 4K Durable)' },
    officialSdk: { ja: '803.6 MiB/s / 0.227 ms（※Flush 省略）', en: '803.6 MiB/s / 0.227 ms (*flush omitted)' },
    veloxSame6: { ja: '715.8 MiB/s / 4.971 ms (Flush 保証)', en: '715.8 MiB/s / 4.971 ms (flush verified)' },
    veloxFull20: { ja: '765.3 MiB/s (Raw 1,126.5) / 4.216 ms', en: '765.3 MiB/s (Raw 1,126.5) / 4.216 ms' },
    deltaNote: { ja: '直接倍率比較不可（公式 SDK はホスト Flush 省略）', en: 'No direct ratio (Official SDK omits host flush)' },
    source: 'sdk_vs_velox_direct.json / phase17/step4-storage-f2fs.json',
    honestCaveat: { ja: 'Velox は taskkill /F 強制終了後の SHA-256 一致を検証した永続 Flush を維持。F2FS 実験構成で永続 tx 2.00倍。', en: 'Velox enforces crash-safe FlushFileBuffers (SHA-256 verified after taskkill /F). Experimental F2FS doubles durable TPS.' },
  },
  {
    item: { ja: '14. フレームペーシング (60/90/120 fps 1% Low)', en: '14. Frame Pacing (60 / 90 / 120 fps 1% Low)' },
    officialSdk: { ja: '55.80 / 81.83 / 105.65 fps', en: '55.80 / 81.83 / 105.65 fps' },
    veloxSame6: { ja: '59.60 / 88.46 / 119.08 fps', en: '59.60 / 88.46 / 119.08 fps' },
    veloxFull20: { ja: '59.84 / 89.49 / 119.11 fps (最大 119.58)', en: '59.84 / 89.49 / 119.11 fps (up to 119.58)' },
    deltaNote: { ja: '120 fps 1% Low +13.46 fps (stutter 0、ネイティブ 119.94 fps 比 99.3%)', en: '120 fps 1% Low +13.46 fps (0 stutter, 99.3% of native 119.94)' },
    source: 'sdk_vs_velox_direct.json',
  },
  {
    item: { ja: '15. OpenGL ES 性能 (draws / tex_upload / sync / shader)', en: '15. OpenGL ES (draws / tex_upload / sync / shader)' },
    officialSdk: { ja: '247 / 218 / 1,591 fps / 6.49 ms', en: '247 / 218 / 1,591 fps / 6.49 ms' },
    veloxSame6: { ja: '338 / 391 / 32,505 fps / 0.27 ms', en: '338 / 391 / 32,505 fps / 0.27 ms' },
    veloxFull20: { ja: '337 / 403 / 35,414 fps / 0.28 ms', en: '337 / 403 / 35,414 fps / 0.28 ms' },
    deltaNote: { ja: 'draws +37% / tex +85% / sync 22.3倍 / shader 24.4倍高速', en: 'draws +37% / tex +85% / sync 22.3x / shader 24.4x faster' },
    source: 'sdk_vs_velox_direct.json',
  },
  {
    item: { ja: '16. ゲスト時刻取得 (clock_gettime)', en: '16. Guest Timekeeping (clock_gettime)' },
    officialSdk: { ja: '6,999.6 ns (HPET 既定)', en: '6,999.6 ns (default HPET)' },
    veloxSame6: { ja: '14.5 ns (WHPX TSC 修正)', en: '14.5 ns (WHPX TSC fix)' },
    veloxFull20: { ja: '14.5 ns (25.6 億回逆行 0)', en: '14.5 ns (0 regressions in 2.56B calls)' },
    deltaNote: { ja: '482.7倍短縮（公式 SDK で TSC 強制すると逆行 2 億回超）', en: '482.7x faster (forcing TSC on stock SDK causes >200M regressions)' },
    source: 'docs/BENCHMARK_RESULTS.md (§5 clockcheck)',
  },
  {
    item: { ja: '17. コールドブート時間 (Android 17)', en: '17. Cold Boot Time (Android 17)' },
    officialSdk: { ja: '14.67 s (3 回中央値)', en: '14.67 s (3-run median)' },
    veloxSame6: { ja: '14.70 s (3 回中央値)', en: '14.70 s (3-run median)' },
    veloxFull20: { ja: '15.81 s (3 回中央値)', en: '15.81 s (3-run median)' },
    deltaNote: { ja: 'ほぼ同等（20 vCPU 初期化で +1.1 s）', en: 'Broadly at parity (+1.1 s for 20 vCPU init)' },
    source: 'sdk_vs_velox_direct.json',
  },
  {
    item: { ja: '18. アプリ起動時間 (Settings コールド起動)', en: '18. App Cold Launch (Settings)' },
    officialSdk: { ja: '279 ms (3 回中央値)', en: '279 ms (3-run median)' },
    veloxSame6: { ja: '187 ms (3 回中央値)', en: '187 ms (3-run median)' },
    veloxFull20: { ja: '198 ms (Velox Launcher 時 177 ms)', en: '198 ms (177 ms with Velox Launcher)' },
    deltaNote: { ja: '-29%〜-33% 短縮（Velox Launcher 併用でさらに軽量）', en: '29%–33% faster (lighter still with Velox Launcher)' },
    source: 'sdk_vs_velox_direct.json / verification_preview02.json',
  },
  {
    item: { ja: '19. アイドル時ホスト QEMU CPU 使用率', en: '19. Idle Host QEMU CPU Usage' },
    officialSdk: { ja: '20.70% (1 コア=100% 換算、全体 1.0%)', en: '20.70% (1 core=100%, 1.0% of 20T)' },
    veloxSame6: { ja: '33.79% (高精度タイマー有効時)', en: '33.79% (with HighRes poll ON)' },
    veloxFull20: { ja: '28.32% (全体 1.4%、HR=0 で低下)', en: '28.32% (1.4% of 20T; lower w/ HR=0)' },
    deltaNote: { ja: '誠実なトレードオフ: 高精度 120fps ペーシングの代償として全体 CPU +0.4%', en: 'Honest trade-off: +0.4% total system CPU for 120 fps HighRes pacing' },
    source: 'sdk_vs_velox_direct.json',
    honestCaveat: { ja: 'VELOX_HIGHRES_POLL=0（CPU/ストレージ特化プロファイル）では増加しない。', en: 'Not incurred when VELOX_HIGHRES_POLL=0 (CPU/Storage profiles).' },
  },
  {
    item: { ja: '20. Vulkan 機能拡張・適合性 (dEQP)', en: '20. Vulkan Extensions & Conformance (dEQP)' },
    officialSdk: { ja: 'Vulkan 1.3 のみ（RT / CoopMat 非対応）', en: 'Vulkan 1.3 only (no RT / no CoopMat)' },
    veloxSame6: { ja: 'Ray Query + RT Pipeline + CoopMat + 実験 VK 1.4', en: 'Ray Query + RT Pipeline + CoopMat + exp. VK 1.4' },
    veloxFull20: { ja: 'dEQP 13 スイート計 23,262 Pass / 0 Fail', en: '13 dEQP suites: 23,262 Pass / 0 Fail' },
    deltaNote: { ja: 'ray_query 11,710 + rt_pipeline 5,665 + VK 1.4 分割 20 Pass', en: 'ray_query 11,710 + rt_pipeline 5,665 + VK 1.4 20 Pass' },
    source: 'final_regression_phase20.json',
  },
];

export const BREAKTHROUGH_CARDS: BreakthroughCard[] = [
  {
    id: 'bt-draws',
    metric: { ja: 'Vulkan Draw Call（同 6 vCPU 直接比較）', en: 'Vulkan Draw Call (Same 6 vCPU Direct)' },
    headline: { ja: '299.8 → 1,603.8 fps（5.35倍）', en: '299.8 → 1,603.8 fps (5.35x)' },
    before: { ja: '公式 SDK 37.3.10: 299.81 fps', en: 'Official SDK 37.3.10: 299.81 fps' },
    after: { ja: 'Velox 第 20 段階: 1,603.80 fps (20T 1,596.2 = ネイティブ比 93.9%)', en: 'Velox Phase 20: 1,603.80 fps (20T 1,596.2 = 93.9% native)' },
    target: { ja: 'Google 公式 Android Emulator 37.3.10 標準構成（6 vCPU）', en: 'Google Official Android Emulator 37.3.10 stock (6 vCPU)' },
    conditions: { ja: '同一 PC・同一 Android 17 イメージ・同一 6 vCPU・16 GiB RAM・3 回中央値・画像チェックサム一致', en: 'Same PC, same Android 17 image, same 6 vCPU, 16 GiB RAM, 3-run median, pixel checksum matched' },
    mechanism: { ja: 'ASG 共有メモリ転送 + 受信先読み (gfxstream 0018) + コマンド先送りランプ (Mesa 0025) + ロックフリー一括デコード (gfxstream 0031)', en: 'ASG shared memory + prefetch (gfxstream 0018) + guest stream ramp (Mesa 0025) + lock-free batched decode (gfxstream 0031)' },
    constraint: { ja: 'Draw Call 律速のオフスクリーン独立ベンチマークであり、実ゲーム全般の FPS 倍率を保証しません。', en: 'Draw-call-bound independent benchmark; does not guarantee identical FPS scaling across all real-world games.' },
    source: 'sdk_vs_velox_direct.json / final_regression_phase20.json',
  },
  {
    id: 'bt-antutu',
    metric: { ja: 'AnTuTu V11 Standard GPU スコア（正常判定）', en: 'AnTuTu V11 Standard GPU Score (Normal)' },
    headline: { ja: '235,973 → 655,918（2.78倍）', en: '235,973 → 655,918 (2.78x)' },
    before: { ja: '開発初期 Velox (20 vCPU, Pre-Batched): 235,973（RT 非対応）', en: 'Early Velox (20 vCPU, Pre-Batched): 235,973 (RT Not Supported)' },
    after: { ja: '第 20 段階統合 (60 Hz, 3/3 完走): 655,918（RT 114,685 + Cottage 387,807 + Coastline 153,426）', en: 'Phase 20 Integrated (60 Hz, 3/3 complete): 655,918 (RT 114,685 + Cottage 387,807 + Coastline 153,426)' },
    target: { ja: 'Velox 開発初期（注: 公式 SDK 6 vCPU 標準は 3D 負荷中タイムアウトで未完走）', en: 'Early Velox baseline (Note: Official SDK 6 vCPU timed out under 3D load)' },
    conditions: { ja: 'Android 17、20 vCPU、16 GiB RAM、900×1600 60 Hz、端末情報無偽装・APK 無改変', en: 'Android 17, 20 vCPU, 16 GiB RAM, 900x1600 60 Hz, no spoofing or APK modification' },
    mechanism: { ja: 'Batched Descriptor + ゲスト内 Descriptor Pool リセット + ASG + WHPX TSC 修正 + Ray Query/RT Pipeline 新規実装', en: 'Batched Descriptor + deferred pool reset + ASG + WHPX TSC fix + newly implemented Ray Query & RT Pipeline' },
    constraint: { ja: 'GPU・UX は正常判定だが、AnTuTu の総合・CPU・MEM には異常判定が付与される（参考値）。アプリ自身に約 60 fps 上限あり。', en: 'GPU & UX pass normal checks, while Total/CPU/MEM carry AnTuTu abnormal flags (reference only). App self-caps near 60 fps.' },
    source: 'antutu-v11.0.6-standard-run1.json / final_antutu_ab.json',
  },
  {
    id: 'bt-upload',
    metric: { ja: 'GPU メモリアップロード（64 MiB Type 2 経路）', en: 'GPU Memory Upload (64 MiB Type 2 Path)' },
    headline: { ja: '0.31 → 12.87–14.14 GB/s（41.5〜45.6倍）', en: '0.31 → 12.87–14.14 GB/s (41.5–45.6x)' },
    before: { ja: '公式 SDK / CACHED_UPLOAD=0: 0.31 GB/s', en: 'Official SDK / CACHED_UPLOAD=0: 0.31 GB/s' },
    after: { ja: 'Velox Phase 18: 12.87 GB/s (41.5x) / Phase 20: 14.13–14.14 GB/s (45.6x, ネイティブ比 85.9%)', en: 'Velox Phase 18: 12.87 GB/s (41.5x) / Phase 20: 14.13–14.14 GB/s (45.6x, 85.9% native)' },
    target: { ja: 'Google 公式 SDK 37.3.10 既定の Uncached Host-Visible 経路', en: 'Official SDK 37.3.10 default uncached host-visible path' },
    conditions: { ja: 'vk-mem_paths 64 MiB Type 2 Staging→VRAM DMA E2E、3 回中央値、全バイト検算通過', en: 'vk-mem_paths 64 MiB Type 2 Staging→VRAM DMA E2E, 3-run median, byte-verified' },
    mechanism: { ja: 'VELOX_VK_CACHED_UPLOAD=1 (gfxstream 0010) により WHPX 上での Write-Combining バースト分断 (gfxstream #198) を回避', en: 'VELOX_VK_CACHED_UPLOAD=1 (gfxstream 0010) avoids WHPX Write-Combining burst splitting (gfxstream #198)' },
    constraint: { ja: '特定アップロード経路に限定された倍率であり、GPU 全体やエミュレーター全体の高速化倍率ではない。ReBAR 直接書込は 0.18 GB/s のまま。', en: 'Scoped strictly to this upload path — NOT a whole-GPU or whole-emulator speedup. ReBAR direct writes remain 0.18 GB/s under WHPX.' },
    source: 'phase18/step6-gpu-memory.json / sdk_vs_velox_direct.json',
  },
  {
    id: 'bt-mtfence',
    metric: { ja: 'マルチスレッド Fence 照会競合（8T 同時）', en: 'Multi-Thread Fence Contention (8T Concurrent)' },
    headline: { ja: '195,609 → 1,458 ns（134.2倍短縮）', en: '195,609 → 1,458 ns (134.2x faster)' },
    before: { ja: 'Phase 15 soft18 競合時: 195,609.1 ns（公式 SDK: 16,629.8 ns）', en: 'Phase 15 soft18 spin-convoy: 195,609.1 ns (Official SDK: 16,629.8 ns)' },
    after: { ja: 'Phase 16+ Patch 0024 + soft18_rpe_high: 1,457.8 ns（16T では 763倍短縮）', en: 'Phase 16+ Patch 0024 + soft18_rpe_high: 1,457.8 ns (763x faster at 16T)' },
    target: { ja: 'Phase 15 soft18 の KI-43 スピン・コンボイ発生条件（公式 SDK 比でも 11.4倍高速）', en: 'Phase 15 soft18 KI-43 spin-convoy regression (also 11.4x faster than Official SDK)' },
    conditions: { ja: 'vk-syncbench fence_status_mt8、3 ラウンド中央値、8 スレッド同時未シグナル照会', en: 'vk-syncbench fence_status_mt8, 3-round median, 8 threads polling unsignaled fences' },
    mechanism: { ja: 'gfxstream 0024 による seqno 3 段階バックオフ待機 (512× pause → 128× yield → 1 ms CV) と RenderThread 優先度昇格', en: 'gfxstream 0024 3-tier seqno backoff (512x pause → 128x yield → 1 ms CV) + RenderThread priority boost' },
    constraint: { ja: 'マルチスレッド未シグナル Fence 競合マイクロベンチ特有の解消倍率であり、エミュレーター全体の速度倍率ではない。', en: 'Specific to multi-thread unsignaled fence contention — NOT a whole-emulator speedup.' },
    source: 'phase15/step6-ab-perf.json / phase16/final-v16g-syncbench.json',
  },
  {
    id: 'bt-pacing',
    metric: { ja: '120 fps Vulkan フレームペーシング 1% Low', en: '120 fps Vulkan Frame Pacing 1% Low' },
    headline: { ja: '105.65 → 119.11–119.58 fps（stutter 0）', en: '105.65 → 119.11–119.58 fps (0 stutter)' },
    before: { ja: '公式 SDK 37.3.10 (標準 g_poll): 105.65 fps（60/90 fps: 55.80 / 81.83）', en: 'Official SDK 37.3.10 (stock g_poll): 105.65 fps (60/90 fps: 55.80 / 81.83)' },
    after: { ja: 'Velox Phase 20 (HR=1, slack=500): 119.11–119.58 fps（ネイティブ 119.94 比 99.3–99.7%）', en: 'Velox Phase 20 (HR=1, slack=500): 119.11–119.58 fps (99.3–99.7% of native 119.94)' },
    target: { ja: '公式 SDK 37.3.10 標準（Windows QEMU 1 ms 切り上げ）', en: 'Official SDK 37.3.10 default (Windows QEMU 1 ms poll rounding)' },
    conditions: { ja: 'vk-game_demo 1080p (3,200 Draw + 1 MiB テクスチャ更新 + Compute SSBO)、チェックサム一致', en: 'vk-game_demo 1080p (3,200 draws + 1 MiB texture update + Compute SSBO), checksum verified' },
    mechanism: { ja: 'QEMU Patch 0013/0015 による CREATE_WAITABLE_TIMER_HIGH_RESOLUTION と 500 µs ハイブリッドスピン待機', en: 'QEMU Patches 0013/0015 using CREATE_WAITABLE_TIMER_HIGH_RESOLUTION + 500 µs hybrid slack wait' },
    constraint: { ja: 'オプトイン機能。アイドル時ホスト CPU が 20 コア全体比 +0.4〜0.6% 微増するトレードオフあり。', en: 'Opt-in setting; adds ~0.4–0.6% total 20-core host idle CPU usage.' },
    source: 'sdk_vs_velox_direct.json / phase18/step4-real-app-pacing.json',
  },
  {
    id: 'bt-f2fs',
    metric: { ja: 'F2FS による永続ストレージトランザクション（実験構成）', en: 'F2FS Durable Storage Transactions (Experimental)' },
    headline: { ja: '194.2 → 389.1 TPS（2.00倍）', en: '194.2 → 389.1 TPS (2.00x)' },
    before: { ja: 'ext4 (barrier=1): SQLite WAL+FULL 194.2 TPS / 小ファイル 196.6/s / 4K P50 5.00 ms', en: 'ext4 (barrier=1): SQLite WAL+FULL 194.2 TPS / small_create 196.6/s / 4K P50 5.00 ms' },
    after: { ja: 'F2FS (fsync_mode=posix): 389.1 TPS (2.00x) / 小ファイル 435.3/s (2.21x) / 4K P50 2.88 ms', en: 'F2FS (fsync_mode=posix): 389.1 TPS (2.00x) / small_create 435.3/s (2.21x) / 4K P50 2.88 ms' },
    target: { ja: '同一直接 raw virtio-blk (aio=native, cache=none, mq4) 上の ext4 永続構成', en: 'Durable ext4 on the exact same direct raw virtio-blk device (aio=native, cache=none, mq4)' },
    conditions: { ja: '3 回交互中央値。正常再起動＋taskkill /F 強制終了後の fsck.f2fs および 16 MiB SHA-256 整合検証 100% PASS', en: '3-run alternating median; 100% fsck.f2fs & 16 MiB SHA-256 pass after clean reboot and taskkill /F crash' },
    mechanism: { ja: 'ext4 JBD2 の 2x Flush 構造を回避し、F2FS のログ構造追記＋単一 Flush で永続化レイテンシを半減', en: 'Eliminates ext4 JBD2 double-flush barrier via F2FS log-structured single-flush fsync' },
    constraint: { ja: '実験構成限定。公式 Google APIs /vendor/etc/fstab は AVB/dm-verity 保護のため既定 AVD は ext4 を維持。コールド読込は ext4 が 16–45% 高速。', en: 'Experimental only. Official Google APIs AVDs keep ext4 due to AVB/dm-verity on /vendor/etc/fstab. ext4 leads cold reads by 16–45%.' },
    source: 'phase18/step7-f2fs-direct.json',
  },
];

export const TECH_PILLARS: TechPillar[] = [
  {
    id: 'cpu-virt',
    title: { ja: 'CPU・仮想化（WHPX / QEMU）', en: 'CPU & Virtualization (WHPX / QEMU)' },
    summary: {
      ja: '上流 AOSP QEMU + Windows Hypervisor Platform (WHPX) を土台に、コア数制限解除・TSC 安定化・ロックレス高速経路・P/E ハイブリッド配置を独自パッチ（emulator 19 本）で実装しています。',
      en: 'Built on upstream AOSP QEMU + Windows Hypervisor Platform (WHPX), adding 19 independent patches for core-cap removal, TSC stability, lockless fast paths, and hybrid P/E scheduling.',
    },
    items: [
      {
        name: { ja: 'WHPX 20 vCPU 解放とコールドブート修正', en: 'WHPX 20 vCPU Unlock & Cold-Boot Fix' },
        origin: 'velox_patch',
        patchRef: 'patches/emulator/0001',
        conventional: { ja: '公式 SDK 37.3.10 は WHPX 実行時の vCPU を最大 6 コアにハードコード制限。', en: 'Official SDK 37.3.10 hard-caps WHPX guests at 6 vCPUs.' },
        veloxChange: { ja: 'WHPX 初期化時の 6 コア上限を解除し、APIC/SMP 構成を 20 論理 CPU まで正規起動可能に修正。', en: 'Lifts the 6-core WHPX cap and fixes SMP/APIC cold boot up to 20 logical CPUs.' },
        measuredEffect: { ja: 'CPU 整数 MT が 8.60 → 23.35 Gops/s（2.71倍、ネイティブ比 95.1%）へ到達。', en: 'CPU integer MT scales from 8.60 to 23.35 Gops/s (2.71x, 95.1% native).' },
      },
      {
        name: { ja: 'WHPX TSC 時刻同期の修正', en: 'WHPX TSC Timekeeping Fix' },
        origin: 'velox_patch',
        patchRef: 'patches/emulator/0002',
        conventional: { ja: '既定は HPET（clock_gettime 1 回 7.0 µs）。TSC 強制時は vCPU 間で 2 億回超の時刻逆行が発生。', en: 'Defaults to HPET (7.0 µs/call); forcing TSC causes >200M cross-vCPU clock regressions.' },
        veloxChange: { ja: 'WHPX の vCPU 間 TSC オフセットを同期させ、watchdog 有効のまま安定した TSC クロックソースを実現。', en: 'Synchronizes per-vCPU WHPX TSC offsets so Linux clocksource=tsc stays monotonic with watchdog enabled.' },
        measuredEffect: { ja: 'clock_gettime が 7,000 ns → 14.5 ns（482.7倍短縮）、120 秒・25.6 億回で逆行 0 件。', en: 'clock_gettime drops from 7,000 ns to 14.5 ns (482.7x faster) with 0 regressions across 2.56B reads.' },
      },
      {
        name: { ja: 'QEMU BQL 削減 & ロックレス WHPX 高速経路', en: 'QEMU BQL Reduction & Lockless WHPX Fast Path' },
        origin: 'velox_patch',
        patchRef: 'patches/emulator/0003, 0018',
        conventional: { ja: 'vCPU が MMIO や whpx_vcpu_pre_run() / halt のたびに QEMU 全体ロック (BQL) を取得し競合。', en: 'Every MMIO and whpx_vcpu_pre_run()/halt acquires QEMU Big Lock (BQL), stalling concurrent vCPUs.' },
        veloxChange: { ja: 'address-space ping 中の BQL 解放に加え、Patch 0018 で Dekker 順序付けロックレス pre_run と halt 即復帰を導入。', en: 'Releases BQL during address-space pings and adds Dekker-ordered lockless pre_run & fast halt return (Patch 0018).' },
        measuredEffect: { ja: 'vk-render sync +2.51%、fence_reset -5.8%、CPU MT +0.62% / FP MT +1.19%、最小 submit_empty_wait 3.914 µs。', en: 'vk-render sync +2.51%, fence_reset -5.8%, CPU MT +0.62% / FP MT +1.19%, min submit_empty_wait 3.914 µs.' },
      },
      {
        name: { ja: 'Intel P/E コア自動判別と 20_PE_rpe スケジューリング', en: 'Intel P/E Core Topology & 20_PE_rpe Scheduling' },
        origin: 'velox_original',
        patchRef: 'velox/hw/cpu_topology.py + patches/emulator/0010',
        conventional: { ja: 'OS 自動配置ではゲスト高負荷時に RenderThread が vCPU と競合し draws が -27% 低下。', en: 'Default OS placement suffers -27% draws drop when heavy guest CPU contends with RenderThread.' },
        veloxChange: { ja: 'Win32 EfficiencyClass で P/E コアを型番非依存に判別し、20_PE_rpe（render: PE 高優先 / prefetch: P）＋ EcoQoS 無効化を適用。', en: 'Detects P/E cores generically via Win32 EfficiencyClass; applies 20_PE_rpe placement + EcoQoS disable.' },
        measuredEffect: { ja: '6 トポロジ×7 軸パレート解析で唯一の最適解。高負荷 draws +39.8%（1% Low +108%）。', en: 'Sole Pareto-optimal topology across 6 layouts x 7 axes; heavy-load draws +39.8% (1% Low +108%).' },
      },
      {
        name: { ja: '高精度タイマー待機 (High-Res Poll)', en: 'High-Resolution Timer Wait' },
        origin: 'velox_patch',
        patchRef: 'patches/emulator/0013, 0015',
        conventional: { ja: 'Windows 版 QEMU の qemu_poll_ns が 1 ms 単位に切り上げられ、120 fps (8.33 ms) で 1% Low が 105.7 fps へ低下。', en: 'Windows QEMU rounds qemu_poll_ns up to 1 ms, dragging 120 fps 1% Low down to 105.7 fps.' },
        veloxChange: { ja: 'CREATE_WAITABLE_TIMER_HIGH_RESOLUTION とオプトイン 500 µs ハイブリッドスピン待機を実装。', en: 'Uses CREATE_WAITABLE_TIMER_HIGH_RESOLUTION with opt-in 500 µs hybrid slack polling.' },
        measuredEffect: { ja: '120 fps 1% Low 105.65 → 119.11–119.58 fps（stutter 0、P99 超過 -70%、CV 3.6–4.0x 縮小）。', en: '120 fps 1% Low jumps from 105.65 to 119.11–119.58 fps (0 stutters, -70% P99 overshoot).' },
      },
    ],
  },
  {
    id: 'gpu-virt',
    title: { ja: 'GPU・グラフィックス仮想化（gfxstream / Mesa）', en: 'GPU Virtualization (gfxstream / Mesa)' },
    summary: {
      ja: '上流 AOSP gfxstream と Mesa Vulkan ゲストドライバに対し、計 64 本のパッチ（gfxstream 31 + mesa3d 30 + aemu 3）で通信・同期・デコード・メモリ転送・Ray Tracing を根本的に刷新しています。',
      en: '64 patches across upstream gfxstream (31), mesa3d (30), and aemu (3) overhaul command transport, synchronization, host decoding, GPU uploads, and hardware Ray Tracing.',
    },
    items: [
      {
        name: { ja: 'ASG 共有メモリ転送・受信先読み・コマンド先送りランプ', en: 'ASG Shared Memory, Prefetch & Guest Stream Ramp' },
        origin: 'velox_patch',
        patchRef: 'patches/gfxstream/0005, 0008, 0018 + patches/mesa3d/0018, 0025',
        conventional: { ja: 'goldfish pipe 転送は同期往復 40 µs。上流 ASG は終了時ハング (KI-25) と受信待ち遅延を抱えていた。', en: 'Goldfish pipe incurs 40 µs round-trips; upstream ASG suffered teardown hangs (KI-25) and idle wakeup stalls.' },
        veloxChange: { ja: 'ASG 終了デッドロックを修正し、適応型スピン (500 µs)・専用受信先読みスレッド・ゲスト側段階的コマンド先送り (2→128 KiB) を実装。', en: 'Fixes ASG teardown deadlocks, adds 500 µs adaptive spin, dedicated prefetch thread, and progressive guest stream ramp (2→128 KiB).' },
        measuredEffect: { ja: '同期 RPC 40.14 → 0.80 µs（50.1倍短縮）、先読み単独で draws +21.2%、ランプ併用で +9.5%。', en: 'Sync RPC drops 40.14 → 0.80 µs (50.1x); prefetch adds +21.2% draws, stream ramp adds +9.5%.' },
      },
      {
        name: { ja: 'Descriptor 最適化 & Fence 共有メモリテーブル', en: 'Descriptor Optimization & Fence Shared Memory Table' },
        origin: 'velox_patch',
        patchRef: 'patches/gfxstream/0001, 0021, 0024, 0026 + patches/mesa3d/0002, 0027, 0029',
        conventional: { ja: 'vkResetDescriptorPool / vkGetFenceStatus が毎度ホスト同期 RPC を発生させ、RHIThread の 70% 超を占有。', en: 'vkResetDescriptorPool and vkGetFenceStatus triggered synchronous host RPCs consuming >70% of RHIThread.' },
        veloxChange: { ja: 'VulkanBatchedDescriptorSetUpdate 既定化＋マルチスレッド参照カウント修正、ゲスト内 Descriptor Pool リセット、共有メモリ Fence 完了表、3 段階バックオフ待機を実装。', en: 'Enables Batched Descriptors + thread-safe refcount fix, local pool reset, shared-memory fence table, and 3-tier backoff.' },
        measuredEffect: { ja: 'Descriptor 1 フレームサイクル 1,653 → 30.9 µs（53.6倍）、完了 Fence 照会 37.4 → 0.20 µs（190倍）、MT 競合 134.2倍解消。', en: 'Descriptor cycle 1,653 → 30.9 µs (53.6x), signaled fence 37.4 → 0.20 µs (190x), MT contention 134.2x faster.' },
      },
      {
        name: { ja: 'コマンドデコーダー高速化 (HybridEntityManager)', en: 'Command Decoder Hotpath (HybridEntityManager)' },
        origin: 'velox_patch',
        patchRef: 'patches/gfxstream/0020, 0031',
        conventional: { ja: 'ホスト VkDecoder が描画 API ごとに unordered_map 検索とミューテックス取得を繰り返し、Draw Call の最大律速に。', en: 'Host VkDecoder performed per-call unordered_map lookups and mutex locks, becoming the primary draw-call bottleneck.' },
        veloxChange: { ja: 'Patch 0031 で HybridEntityManager<131072>・ロックフリー一括 BindDescriptorSets・PushConstants キャッシュ・O(1) QueueSubmit を実装。', en: 'Patch 0031 introduces HybridEntityManager<131072>, lock-free batched BindDescriptorSets, PushConstants cache, and O(1) QueueSubmit.' },
        measuredEffect: { ja: 'vk-render draws +8.6%（1,596.2 / 最大 1,615.6 fps = ネイティブ比 93.9%）、sync +2.9%、submit_empty_wait 3.99 µs。', en: 'vk-render draws +8.6% (1,596.2 / peak 1,615.6 fps = 93.9% native), sync +2.9%, submit_empty_wait 3.99 µs.' },
      },
      {
        name: { ja: 'Cached Staging GPU メモリアップロード', en: 'Cached Staging GPU Memory Upload' },
        origin: 'velox_patch',
        patchRef: 'patches/gfxstream/0010',
        conventional: { ja: 'WHPX の EPT 制約で Uncached Host-Visible への CPU 書込が 0.31 GB/s に激減 (gfxstream #198)。', en: 'WHPX EPT splits Write-Combining bursts, collapsing uncached host-visible writes to 0.31 GB/s (gfxstream #198).' },
        veloxChange: { ja: 'VELOX_VK_CACHED_UPLOAD=1 により Staging 割当をキャッシュ型ホスト可視メモリへ最適化（全バイト検算通過）。', en: 'VELOX_VK_CACHED_UPLOAD=1 maps staging uploads onto cached host-visible memory (byte-verified).' },
        measuredEffect: { ja: '64 MiB Type 2 Upload E2E が 0.31 → 12.87–14.14 GB/s（41.5–45.6倍、ネイティブ比 85.9%）。', en: '64 MiB Type 2 Upload E2E improves from 0.31 to 12.87–14.14 GB/s (41.5–45.6x, 85.9% native).' },
      },
      {
        name: { ja: 'Ray Query / Ray Tracing Pipeline / Cooperative Matrix / GLES 最適化', en: 'Ray Query, RT Pipeline, Cooperative Matrix & GLES' },
        origin: 'velox_patch',
        patchRef: 'patches/gfxstream/0011–0017, 0028 + patches/mesa3d/0020–0024, 0030',
        conventional: { ja: '公式 SDK は Vulkan 1.3 のみ（RT・CoopMat 非対応）。Host GLES もテクスチャ転送とシェーダーコンパイルに重複負荷。', en: 'Official SDK lacks RT and Cooperative Matrix; Host GLES has redundant buffer copies and shader compile stalls.' },
        veloxChange: { ja: 'Acceleration Structure 寿命管理・RT Pipeline・Cooperative Matrix・Vulkan 1.4 maintenance5 適合・Host GLES Patch 0028（ゼロコピー入力＋シェーダーキャッシュ）を実装。', en: 'Implements AS lifetime, RT Pipeline, Cooperative Matrix, VK 1.4 maintenance5, and Host GLES Patch 0028 (zero-copy input + shader cache).' },
        measuredEffect: { ja: 'dEQP 計 23,262 Pass / 0 Fail（RQ 11,710 + RT 5,665）、RT 実描画ネイティブ比 95–98%、CoopMat 100%、GLES tex_upload +38.5%（公式比 +85%）。', en: '23,262 dEQP Pass / 0 Fail (RQ 11,710 + RT 5,665), RT 95–98% native, CoopMat 100%, GLES tex_upload +38.5% (+85% vs SDK).' },
      },
    ],
  },
  {
    id: 'mem-storage',
    title: { ja: 'メモリ・ストレージ（THP / Win32 AIO / virtio-blk）', en: 'Memory & Storage (THP / Win32 AIO / virtio-blk)' },
    summary: {
      ja: 'データ破損を招く危険なキャッシュ回避（cache=unsafe）は一切使わず、強制終了耐性（Flush 保証）を維持したままメモリ帯域と I/O 効率を引き上げています。',
      en: 'Avoids unsafe write-cache shortcuts entirely, improving memory throughput and I/O efficiency while strictly preserving crash-safe host flushes.',
    },
    items: [
      {
        name: { ja: 'Guest THP (always_nodefrag) 自動適用', en: 'Automatic Guest THP (always_nodefrag)' },
        origin: 'velox_original',
        patchRef: 'velox/backends/aosp.py (D-044)',
        conventional: { ja: 'ゲスト既定の 4 KiB ページでは WHPX の 2 次元 EPT ページウォークにより 1T RAM 帯域が 15.8 GiB/s に低下。', en: 'Default 4 KiB guest pages incur 2D EPT page walks, capping 1T RAM bandwidth at 15.8 GiB/s.' },
        veloxChange: { ja: '起動後にゲスト THP を always / defrag=never に安全適用し、RSS 増加なしで 2 MiB ページを活用。', en: 'Safely enables guest THP always with defrag=never post-boot for 2 MiB pages with zero RSS bloat.' },
        measuredEffect: { ja: '64 MiB 1T 15.77 → 19.58 GiB/s (+24.1%、8P で 95.2% native)、ランダム遅延 -8.4%、確保 2.17倍高速。', en: '64 MiB 1T jumps 15.77 → 19.58 GiB/s (+24.1%, 95.2% native on 8P), random latency -8.4%, alloc 2.17x faster.' },
      },
      {
        name: { ja: 'Win32 AIO + virtio-blk マルチキュー + バウンスバッファプール', en: 'Win32 AIO, Multi-Queue virtio-blk & Bounce Buffer Pool' },
        origin: 'velox_patch',
        patchRef: 'patches/emulator/0012, 0019',
        conventional: { ja: '標準ブロック経路は単一キュー・毎回 VirtualAlloc 確保によるオーバーヘッドが存在。', en: 'Default block path uses single-queue I/O and repeated VirtualAlloc bounce-buffer allocations.' },
        veloxChange: { ja: 'aio=native + virtio-blk num-queues=4 に加え、Patch 0019 で 16 スロット再利用アライン済みバウンスバッファプールを導入。', en: 'Enables aio=native + virtio-blk num-queues=4 plus Patch 0019 16-slot reusable aligned bounce buffer pool.' },
        measuredEffect: { ja: 'Raw 順次書込+Flush 1,126.5 MiB/s (58.6% native)、small_create +9.4% (vs threads)、taskkill /F 強制終了 SHA-256 完全一致。', en: 'Raw seq write+flush 1,126.5 MiB/s (58.6% native), small_create +9.4% vs threads, taskkill /F SHA-256 verified.' },
      },
      {
        name: { ja: 'F2FS 永続トランザクション検証（実験構成）', en: 'F2FS Durable Transaction Study (Experimental)' },
        origin: 'velox_original',
        patchRef: 'docs/research/phase18-f2fs.md (D-053)',
        conventional: { ja: 'ext4 の JBD2 ジャーナルは fsync 1 回につき 2 回のホスト Flush を発行し、4K 永続書込が ~5.0 ms に律速。', en: 'ext4 JBD2 issues two host flushes per fsync, creating a ~5.0 ms 4K durable write floor.' },
        veloxChange: { ja: '隔離 raw virtio-blk 上で F2FS (fsync_mode=posix) を検証。公式 AVD は AVB/dm-verity 保護のため ext4 を無改変維持。', en: 'Evaluated F2FS (fsync_mode=posix) on isolated raw virtio-blk while preserving unmodified ext4 on official AVB-protected AVDs.' },
        measuredEffect: { ja: 'SQLite WAL+FULL 194.2 → 389.1 TPS (2.00倍)、小ファイル 2.21倍、強制終了整合 100% PASS。', en: 'SQLite WAL+FULL 194.2 → 389.1 TPS (2.00x), small_create 2.21x, 100% crash-integrity PASS.' },
      },
    ],
  },
  {
    id: 'desktop-ux',
    title: { ja: '実用機能・PC 向け Android デスクトップ環境', en: 'Practical PC Android Desktop Environment' },
    summary: {
      ja: '公式 Google APIs システムイメージ（Android 12 / Android 17）を無改変のまま活かしつつ、PC の大画面・マウス・キーボード・RDP 環境で快適に使える独自 UI と管理基盤を構築しています。',
      en: 'Preserves unmodified official Google APIs Android 12 & 17 images while delivering a native desktop launcher, PC input fixes, game keymapping, and lightweight Windows management.',
    },
    items: [
      {
        name: { ja: 'Android 12 / 17 マルチバージョン & ARM64 変換対応', en: 'Android 12 / 17 Multi-Version & ARM64 Support' },
        origin: 'upstream',
        conventional: { ja: '上流 Google APIs イメージが Android 12/17 と Berberis ベース ARM64 変換 (libndk_translation) を提供。', en: 'Upstream Google APIs images provide Android 12/17 and Berberis-based ARM64 translation (libndk_translation).' },
        veloxChange: { ja: 'Velox バックエンドで API レベル別ガード・複数端末プロファイル管理・ARM64 変換性能の継続検証を統合。', en: 'Velox adds per-API guards, multi-device profiles, and verified ARM64 translation benchmarking.' },
        measuredEffect: { ja: 'Android 17 で NEON SIMD 64.41 GiB/s (98.8% vs x86_64)、4T Atomic 83.5 Mops/s (MuMu 比 1.58倍)、GLES/Vulkan 100%。', en: 'Android 17 achieves NEON SIMD 64.41 GiB/s (98.8% vs x86_64), 4T Atomic 83.5 Mops/s (1.58x MuMu), GLES/Vulkan 100%.' },
      },
      {
        name: { ja: 'Velox Launcher（ネイティブ Android PC ホームアプリ）', en: 'Velox Launcher (Native Android PC Home App)' },
        origin: 'velox_original',
        patchRef: 'android/velox-launcher/',
        conventional: { ja: '標準 NexusLauncher はスマホ縦画面前提で、常駐メモリ 115.8 MB PSS・タスクバーやアプリ検索なし。', en: 'Stock NexusLauncher assumes portrait phones, uses 115.8 MB PSS, and lacks a desktop taskbar or app menu.' },
        veloxChange: { ja: 'Kotlin/Java ネイティブでタスクバー・検索/分類付きアプリメニュー・右クリック・クイック設定・Freeform マルチウィンドウ対応ホームを開発。', en: 'Built native Android launcher with taskbar, searchable categorized app menu, right-click, quick settings, and Freeform multi-window.' },
        measuredEffect: { ja: 'メモリ 115.8 → 19.6–65.5 MB PSS、アイドル CPU 0.0%、Settings 起動 324 → 177 ms。', en: 'Memory drops 115.8 → 19.6–65.5 MB PSS, 0.0% idle CPU, Settings launch 324 → 177 ms.' },
      },
      {
        name: { ja: 'PC 向け入力修正（RDP・合成マウス）& ゲームモード 1:1 キーマップ', en: 'PC Input Fix (RDP / Synthesized Mouse) & Game Mode Keymap' },
        origin: 'velox_patch',
        patchRef: 'patches/emulator/0016, 0017',
        conventional: { ja: 'Qt 6 が RDP やタッチ経由の合成マウスイベントを無条件破棄し、画面操作不能になる問題 (KI-44) が発生。', en: 'Upstream Qt 6 dropped synthesized mouse events from RDP/touch-to-mouse paths, freezing UI interaction (KI-44).' },
        veloxChange: { ja: 'Patch 0016 で実タッチ/ペン近接時のみ重複抑止するよう修正。Patch 0017 で F12 ホーム復帰と 1:1 WASD ゲームキーマップを追加。', en: 'Patch 0016 suppresses synthesized duplicates only during active native touch/pen; Patch 0017 adds F12 Home & 1:1 WASD game keymapping.' },
        measuredEffect: { ja: 'RDP・通常マウス・画面回転連動・WASD ゲーム操作のすべてを実機イベントレベルで検証通過。', en: 'Verified across RDP, desktop mouse, rotation sync, and WASD keymap injection at input-event level.' },
      },
      {
        name: { ja: '軽量 Windows GUI (emu gui) & CLI・自動推奨', en: 'Lightweight Windows GUI (emu gui), CLI & Auto-Recommend' },
        origin: 'velox_original',
        patchRef: 'velox/gui/ + velox/cli.py + velox/optimize/recommend.py',
        conventional: { ja: '手作業でのコマンドラインフラグ管理とハードウェア別チューニングが必要。', en: 'Required manual emulator CLI flags and ad-hoc per-machine tuning.' },
        veloxChange: { ja: 'Python 3.12 標準ライブラリのみで完結する共通バックエンド・ダークテーマ GUI・ハードウェア自動推奨・APK D&D・共有フォルダ連携を提供。', en: 'Zero-dependency Python 3.12 shared backend powering dark-theme GUI, hardware auto-recommend, APK drag-drop, and shared folders.' },
        measuredEffect: { ja: 'ユニットテスト 66/66 OK、監視時 CPU 負荷極小、初回起動から最適プロファイルを自動生成。', en: '66/66 unit tests passing, minimal monitoring overhead, and automatic optimal profile generation.' },
      },
    ],
  },
];

export const MUMU_COMPARISON_ROWS: MumuRow[] = [
  {
    metric: { ja: 'Vulkan API / Ray Tracing / CoopMat', en: 'Vulkan API / Ray Tracing / CoopMat' },
    veloxHost: 'Vulkan 1.3 (1.4 実験) + RT + CoopMat',
    veloxGa: '同左 (Vulkan 経由)',
    mumu: '非対応 (0 Vulkan devices)',
    winner: 'velox',
    note: { ja: 'MuMuPlayer 12.0 実測環境は OpenGL ES 3.2 のみ公開', en: 'Tested MuMuPlayer 12.0 exposes OpenGL ES 3.2 only (0 Vulkan devices)' },
  },
  {
    metric: { ja: 'GLES Draw Call (draws)', en: 'GLES Draw Call (draws)' },
    veloxHost: '343.7 fps (P20: 338.3)',
    veloxGa: '587.5 fps (別経路)',
    mumu: '218.8 fps',
    winner: 'velox',
    note: { ja: 'Host GLES でも 1.57倍、GuestAngle で 2.69倍（単一構成同時達成ではない）', en: '1.57x on Host GLES, 2.69x on GuestAngle (separate configs)' },
  },
  {
    metric: { ja: 'GLES GPU 同期 (sync_fence)', en: 'GLES GPU Sync (sync_fence)' },
    veloxHost: '37,491 fps (P20: 35,414)',
    veloxGa: '11,903 fps (別経路)',
    mumu: '9,143 fps',
    winner: 'velox',
    note: { ja: 'Host GLES 経路で 4.1倍高速', en: '4.1x faster on Host GLES' },
  },
  {
    metric: { ja: 'GLES Compute Shader (compute)', en: 'GLES Compute Shader (compute)' },
    veloxHost: '35,195 disp/s (P20: 35,096)',
    veloxGa: '10,676 disp/s (別経路)',
    mumu: '8,660 disp/s',
    winner: 'velox',
    note: { ja: 'Host GLES 経路で 4.06倍高速（チェックサム一致）', en: '4.06x faster on Host GLES (checksum verified)' },
  },
  {
    metric: { ja: 'GLES Fill Rate (fill)', en: 'GLES Fill Rate (fill)' },
    veloxHost: '29,209 fps (P20: 27,938)',
    veloxGa: '10,832 fps (別経路)',
    mumu: '8,575 fps',
    winner: 'velox',
    note: { ja: 'Host GLES 経路で 3.4倍高速', en: '3.4x faster on Host GLES' },
  },
  {
    metric: { ja: 'GLES テクスチャ転送 (tex_upload)', en: 'GLES Texture Upload (tex_upload)' },
    veloxHost: '370.6 fps (P20: 402.7)',
    veloxGa: '965.9 fps (別経路)',
    mumu: '651.5 fps',
    winner: 'split',
    note: { ja: 'Host GLES は MuMu に劣るが、GuestAngle は MuMu を 1.48倍上回る', en: 'MuMu leads Host GLES; GuestAngle leads MuMu by 1.48x' },
  },
  {
    metric: { ja: 'GLES 読み戻し (readback)', en: 'GLES Readback (readback)' },
    veloxHost: '2,906 fps',
    veloxGa: '5,079 fps (別経路)',
    mumu: '1,487 fps',
    winner: 'velox',
    note: { ja: 'Host GLES で 1.95倍、GuestAngle で 3.42倍', en: '1.95x on Host GLES, 3.42x on GuestAngle' },
  },
  {
    metric: { ja: 'GLES 120 fps ペーシング 1% Low', en: 'GLES 120 fps Pacing 1% Low' },
    veloxHost: '117.41 fps',
    veloxGa: '115.19 fps',
    mumu: '119.46 fps',
    winner: 'mumu',
    note: { ja: 'MuMuPlayer が優位（60 fps も 59.95 vs 59.44 fps）', en: 'MuMuPlayer leads (also 59.95 vs 59.44 fps at 60 fps)' },
  },
  {
    metric: { ja: 'ARM64 変換: 4T Atomic / memcpy64m', en: 'ARM64 Translation: 4T Atomic / memcpy64m' },
    veloxHost: '83.5 Mops / 370.3 GiB/s (A17)',
    veloxGa: '—',
    mumu: '52.9 Mops / 324.3 GiB/s (A12)',
    winner: 'velox',
    note: { ja: 'Android 17 Berberis が 4T Atomic で 1.58倍優位', en: 'Android 17 Berberis leads 4T Atomic by 1.58x' },
  },
  {
    metric: { ja: 'ARM64 変換: スカラー FP32 / FP64 / SHA-256', en: 'ARM64 Translation: Scalar FP32 / FP64 / SHA-256' },
    veloxHost: '2.26 / 2.85 Gflops / 287 MiB/s',
    veloxGa: '—',
    mumu: '6.77 / 7.98 Gflops / 338 MiB/s',
    winner: 'mumu',
    note: { ja: 'MuMuPlayer 独自変換器 libnb.so がスカラー浮動小数点で 2.8〜3.0倍優位', en: 'MuMuPlayer libnb.so leads scalar FP32/FP64 by 2.8–3.0x' },
  },
  {
    metric: { ja: 'コールドブート時間', en: 'Cold Boot Time' },
    veloxHost: '14.7–15.8 s (A17) / 18.2 s (A12)',
    veloxGa: '—',
    mumu: '9.38 s (Android 12)',
    winner: 'mumu',
    note: { ja: '起動速度は MuMuPlayer が約 5–9 秒高速', en: 'MuMuPlayer cold-boots ~5–9s faster' },
  },
];

export const DEVELOPMENT_TIMELINE: TimelineEntry[] = [
  {
    phase: 'Phase 1–2',
    date: '2026-11-20',
    title: { ja: 'AOSP Android Emulator 採用 & GPU 同期ボトルネックの発見', en: 'AOSP Adoption & GPU Sync Bottleneck Discovery' },
    summary: {
      ja: 'WHPX + VBS 有効環境で AOSP Android Emulator (SDK 37.3.10) を基盤選定。プロファイリングにより、AnTuTu 3D 中のホスト GPU 使用率がわずか 3% で、ゲスト RHIThread の 84% が同期 RPC に費やされていることを特定。',
      en: 'Selected AOSP Android Emulator + WHPX under VBS. Profiling revealed only 3% host GPU usage during 3D workloads, with 84% of guest RHIThread spent in synchronous host RPCs.',
    },
    metrics: [
      { ja: 'VulkanBatchedDescriptorSetUpdate 導入で AnTuTu GPU 235,973 → 381,930', en: 'Batched Descriptors lifted AnTuTu GPU 235,973 → 381,930' },
      { ja: '公式 SDK の 6 vCPU 上限 (KI-9) と HPET 遅延を特定', en: 'Identified official 6 vCPU cap (KI-9) and HPET overhead' },
    ],
  },
  {
    phase: 'Phase 3–5',
    date: '2026-11-21–22',
    title: { ja: 'ソースビルド基盤・ASG 通信最適化・WHPX TSC 修正', en: 'Source Build Pipeline, ASG Transport & WHPX TSC Fix' },
    summary: {
      ja: 'QEMU / gfxstream / Mesa の Windows ソースビルド環境を構築。20 vCPU 解放 (Patch 0001)、WHPX TSC 同期 (Patch 0002)、ゲスト内 Descriptor Pool リセット、ASG 終了デッドロック修正とスピン待機を導入。',
      en: 'Established full Windows source builds for QEMU, gfxstream, and Mesa. Shipped 20 vCPU unlock, WHPX TSC fix, local descriptor pool reset, and ASG transport stabilization.',
    },
    metrics: [
      { ja: 'clock_gettime 7,000 ns → 14.5 ns（25.6 億回逆行 0）', en: 'clock_gettime 7,000 ns → 14.5 ns (0 regressions in 2.56B reads)' },
      { ja: 'AnTuTu V11 GPU 484,134（Phase 4 ASG 基準）', en: 'AnTuTu V11 GPU reached 484,134 (Phase 4 ASG)' },
    ],
  },
  {
    phase: 'Phase 6–10',
    date: '2026-11-22',
    title: { ja: 'Ray Query・Ray Tracing Pipeline・Cooperative Matrix 対応', en: 'Hardware Ray Query, RT Pipeline & Cooperative Matrix' },
    summary: {
      ja: 'ホスト RTX GPU のレイトレーシングと行列演算拡張を gfxstream / Mesa ゲストドライバへ完全実装。KI-31 クラッシュの根本修正を経て dEQP 適合試験をゼロエラー完走。',
      en: 'Implemented guest Vulkan Ray Query, Ray Tracing Pipeline, and Cooperative Matrix across gfxstream and Mesa, passing dEQP conformance with zero failures after resolving KI-31.',
    },
    metrics: [
      { ja: 'dEQP ray_query 11,710 Pass + rt_pipeline 5,665 Pass (Fail 0)', en: 'dEQP ray_query 11,710 Pass + rt_pipeline 5,665 Pass (0 Fail)' },
      { ja: 'AnTuTu Seasons: Ray Tracing 初完走 → GPU 635,902', en: 'AnTuTu Seasons: Ray Tracing enabled → GPU 635,902' },
    ],
  },
  {
    phase: 'Phase 11–14',
    date: '2026-11-23',
    title: { ja: '受信先読み・コマンドランプ・Fence 共有メモリテーブル', en: 'ASG Prefetch, Stream Ramp & Shared-Memory Fence Table' },
    summary: {
      ja: '専用受信先読みスレッド (gfxstream 0018)、ゲスト先送りランプ (Mesa 0025)、デコーダー固定費削減 (gfxstream 0020)、および共有メモリ Fence 完了表 (gfxstream 0021 / Mesa 0027) を実装。',
      en: 'Added dedicated ASG prefetch thread, progressive guest command streaming ramp, decoder overhead reduction, and shared-memory fence completion table.',
    },
    metrics: [
      { ja: 'vk-render draws ネイティブ比 60.8% → 80.1%', en: 'vk-render draws jumped from 60.8% to 80.1% of native' },
      { ja: '空き時間後の完了 Fence 照会 33 µs → 4.0 µs', en: 'Post-idle signaled fence poll cut from 33 µs to 4.0 µs' },
    ],
  },
  {
    phase: 'Phase 15–16',
    date: '2026-11-23–24',
    title: { ja: 'MT Fence 競合 134倍解消・適応型スピン・Guest THP 正式採用', en: '134x MT Fence Fix, Adaptive Spin & Guest THP' },
    summary: {
      ja: 'KI-42 デッドロックと KI-43 マルチスレッド Fence スピン・コンボイを gfxstream 0022–0025 で根本修正。Guest THP (always_nodefrag) を正式統合し RAM 帯域を大幅向上。',
      en: 'Resolved KI-42 deadlock and KI-43 MT fence spin-convoy via gfxstream 0022–0025; integrated Guest THP (always_nodefrag) for near-native RAM bandwidth.',
    },
    metrics: [
      { ja: 'fence_status_mt8 195,609 → 1,458 ns（134.2倍改善、16T 763倍）', en: 'fence_status_mt8 195,609 → 1,458 ns (134.2x faster; 763x at 16T)' },
      { ja: 'RAM 1T コピー ネイティブ比 86.9–91.5%、MT 97.6%+', en: 'RAM 1T copy 86.9–91.5% native, MT 97.6%+' },
    ],
  },
  {
    phase: 'Phase 17–18',
    date: '2026-11-24',
    title: { ja: '高精度タイマー・Cached GPU Upload・Vulkan 1.4・F2FS 実証', en: 'High-Res Timer, Cached GPU Upload, Vulkan 1.4 & F2FS' },
    summary: {
      ja: 'Windows 高精度タイマー待機 (QEMU 0013/0015)、Cached Staging GPU アップロード (gfxstream 0010)、Vulkan 1.4 適合修正 (Mesa 0030)、および直接 raw virtio-blk 上での F2FS 永続性・速度比較を完遂。',
      en: 'Delivered high-res timer pacing, Cached Staging GPU uploads, Vulkan 1.4 maintenance5 conformance, and crash-tested raw virtio-blk F2FS evaluation.',
    },
    metrics: [
      { ja: '120 fps 1% Low 106.4 → 119.4 fps (stutter 0)', en: '120 fps 1% Low 106.4 → 119.4 fps (0 stutters)' },
      { ja: '64 MiB GPU Upload 0.31 → 12.87 GB/s (41.5x) / F2FS SQLite 2.00x', en: '64 MiB GPU Upload 0.31 → 12.87 GB/s (41.5x) / F2FS SQLite 2.00x' },
    ],
  },
  {
    phase: 'Phase 19 & Preview 0.1–0.2',
    date: '2026-11-25',
    title: { ja: 'GLES 最適化・Android 12/ARM64・商用比較・PC 向け Velox Launcher', en: 'GLES Optimization, Android 12/ARM64, MuMu Comparison & Velox Launcher' },
    summary: {
      ja: 'Host GLES Patch 0028、Android 12 + 17 複数世代対応、MuMuPlayer 12.0 との誠実な同一 PC 比較、RDP 入力修正 (Patch 0016)、ゲームキーマップ (Patch 0017)、および独自 Velox Launcher・軽量 Windows GUI を実装。',
      en: 'Shipped Host GLES Patch 0028, Android 12+17 support, transparent MuMuPlayer 12.0 comparison, RDP input fix, game keymap, native Velox Launcher, and dark-theme Windows GUI.',
    },
    metrics: [
      { ja: 'GLES tex_upload +38.5% / ARM64 NEON 94.8–98.8%', en: 'GLES tex_upload +38.5% / ARM64 NEON 94.8–98.8%' },
      { ja: 'Velox Launcher 19.6–65.5 MB PSS・アイドル CPU 0.0%', en: 'Velox Launcher 19.6–65.5 MB PSS, 0.0% idle CPU' },
    ],
  },
  {
    phase: 'Phase 20 & v0.3.0 OSS',
    date: '2026-11-25–26',
    title: { ja: 'Extreme Performance & GitHub オープンソース一般公開', en: 'Extreme Performance & Public GitHub OSS Release' },
    summary: {
      ja: 'gfxstream Patch 0031、WHPX ロックレス Patch 0018、Win32 AIO バウンスバッファプール Patch 0019、パレート最適 20_PE_rpe を統合。開発用 (velox-dev) と公開用 (velox) を分離し、監査ツール・CI・Apache-2.0 公開体制を確立。',
      en: 'Integrated gfxstream Patch 0031, lockless WHPX Patch 0018, Win32 AIO pool Patch 0019, and Pareto-optimal 20_PE_rpe. Separated dev/public repositories with automated auditing and published Velox v0.3.0 as OSS.',
    },
    metrics: [
      { ja: 'Draw Call 93.9% / CPU 1T 97.7% / RAM MT 98.0% / AnTuTu GPU 655,918', en: 'Draw Call 93.9% / CPU 1T 97.7% / RAM MT 98.0% / AnTuTu GPU 655,918' },
      { ja: 'dEQP 23,262 Pass / 0 Fail・公開監査 & マルチ OS CI 通過', en: 'dEQP 23,262 Pass / 0 Fail; public audit & multi-OS CI passing' },
    ],
  },
];
