export type Locale = 'ja' | 'en';

export type ComparisonTarget =
  | 'official_sdk'
  | 'velox_past'
  | 'velox_latest'
  | 'windows_native'
  | 'mumu_player';

export type BenchmarkCategory =
  | 'cpu'
  | 'gpu'
  | 'ram'
  | 'storage'
  | 'arm64'
  | 'frame_time';

export type StatType = 'median' | 'mean' | 'max' | 'single' | 'p50' | 'p99';

export type Direction = 'higher' | 'lower';

export interface LocalizedText {
  ja: string;
  en: string;
}

/**
 * Structured single benchmark record (§13 — all 13 required fields).
 * Adding Phase 21+ results only requires appending records to benchmarks.json.
 */
export interface BenchmarkRecord {
  id: string;
  groupId: string;
  category: BenchmarkCategory;
  /** 1. ベンチマーク名 */
  benchmark: LocalizedText;
  /** 2. 開発段階 */
  phase: string;
  /** 3. バージョン */
  version: string;
  /** 4. 比較対象 */
  target: ComparisonTarget;
  /** Optional sub-configuration label (e.g. "6 vCPU", "20 vCPU", "Host GLES", "GuestAngle") */
  configLabel?: LocalizedText;
  /** 5. 測定値 (null if did not complete / unsupported) */
  value: number | null;
  /** 6. 単位 */
  unit: string;
  /** 7. 測定回数 */
  runs: number;
  /** 8. 中央値・平均・最大値の区別 */
  statType: StatType;
  /** 9. 測定環境 */
  environment: string;
  /** 10. 測定条件 */
  conditions: LocalizedText;
  /** 11. 出典 */
  source: string;
  /** 12. 制約 */
  constraints: LocalizedText;
  /** 13. 公開可否 */
  publishable: boolean;
  /** Higher or lower is better */
  direction: Direction;
  /** Whether this record is directly comparable (same hardware/image/core/flush rules) with the group's primary baseline */
  directlyComparable: boolean;
  /** Optional short note if conditions differ from other records in the same group */
  conditionDiffNote?: LocalizedText;
  /** Status when value is null (e.g., did_not_complete, unsupported) */
  statusNote?: LocalizedText;
}

export interface BenchmarkGroupMeta {
  groupId: string;
  category: BenchmarkCategory;
  title: LocalizedText;
  subtitle: LocalizedText;
  unit: string;
  direction: Direction;
  scopeNote: LocalizedText;
}

export interface NativeComparisonMetric {
  id: string;
  label: LocalizedText;
  nativePercent: number;
  veloxValue: string;
  nativeValue: string;
  phase: string;
  source: string;
  note: LocalizedText;
}

export interface BenchmarkDataset {
  schemaVersion: number;
  updatedAt: string;
  latestVersion: string;
  latestPhase: string;
  testEnvironment: {
    cpu: string;
    gpu: string;
    gpuDriver: string;
    ram: string;
    os: string;
    virtualization: string;
  };
  nativeComparison: NativeComparisonMetric[];
  groups: BenchmarkGroupMeta[];
  records: BenchmarkRecord[];
}
