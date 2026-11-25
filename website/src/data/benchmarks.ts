import rawData from './benchmarks.json';
import type { BenchmarkDataset, BenchmarkRecord, ComparisonTarget, Locale } from './schema';

export const dataset = rawData as BenchmarkDataset;

export const TARGET_LABELS: Record<ComparisonTarget, { ja: string; en: string; color: string }> = {
  official_sdk: { ja: 'Google公式 Android Emulator', en: 'Google Official Android Emulator', color: '#64748B' },
  velox_past: { ja: 'Velox 過去バージョン', en: 'Earlier Velox Versions', color: '#38BDF8' },
  velox_latest: { ja: '最新 Velox (Phase 20 / v0.3.0)', en: 'Latest Velox (Phase 20 / v0.3.0)', color: '#3B82F6' },
  windows_native: { ja: 'Windows ネイティブ', en: 'Windows Native', color: '#10B981' },
  mumu_player: { ja: 'MuMuPlayer 12.0', en: 'MuMuPlayer 12.0', color: '#F59E0B' },
};

export const CATEGORY_LABELS = {
  all: { ja: 'すべて', en: 'All' },
  gpu: { ja: 'GPU', en: 'GPU' },
  cpu: { ja: 'CPU', en: 'CPU' },
  ram: { ja: 'RAM', en: 'RAM' },
  storage: { ja: 'ストレージ', en: 'Storage' },
  arm64: { ja: 'ARM64', en: 'ARM64' },
  frame_time: { ja: 'フレームタイム', en: 'Frame Time' },
} as const;

export function publishableRecords(): BenchmarkRecord[] {
  return dataset.records.filter((r) => r.publishable);
}

export function formatStatType(statType: BenchmarkRecord['statType'], locale: Locale): string {
  const map: Record<BenchmarkRecord['statType'], { ja: string; en: string }> = {
    median: { ja: '中央値', en: 'Median' },
    mean: { ja: '平均', en: 'Mean' },
    max: { ja: '最大値', en: 'Peak/Max' },
    single: { ja: '単回', en: 'Single run' },
    p50: { ja: 'P50', en: 'P50' },
    p99: { ja: 'P99', en: 'P99' },
  };
  return map[statType][locale];
}
