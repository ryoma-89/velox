# Velox

> **[English README (README.md)](README.md)**

**Velox** は、Windows 11（**WHPX / Hyper-V / VBS 有効環境**）＋ **gfxstream（Vulkan / OpenGL ES）** 上で Android エミュレーターを高速・安定かつ PC 向けに快適に動作させるためのオープンソース管理・最適化・デスクトップ環境基盤です。

Velox は再現性と誠実な検証を最優先し、Windows のセキュリティ機能（VBS/HVCI）の無効化、Android セキュリティ保護（SELinux/AVB/Scudo）の緩和、危険な書き込みキャッシュ（`cache=unsafe`）、GPU 名・端末情報・ベンチマークスコアの偽装は一切行いません。

---

## 主な特徴

- **ソースパッチによる多層性能最適化（QEMU + WHPX + gfxstream + Mesa）**:
  - WHPX 最大 20 vCPU コールドブートとハイブリッド CPU 向け多目的パレート最適配置（`20_PE_rpe`: 全 vCPU 開放＋`render:PE` / `prefetch:P`）。
  - Vulkan ホットパス最適化（gfxstream Patch 0031）、適応型 ASG ポーリング＆先読み（Patches 0022–0026）、WHPX ロックレス `pre_run`/`halt` 高速パス（QEMU Patch 0018）、Win32 AIO 再利用バウンスバッファプール（QEMU Patch 0019）、高精度 Waitable Timer フレームペーシング（QEMU Patches 0013・0015）。
  - 既定 Vulkan 1.3（Ray Query / Ray Tracing Pipeline / Cooperative Matrix 対応、任意オプトイン Vulkan 1.4 試験対応）。
  - Host GLES 3.1 高速化（ゼロコピー `InputBuffer`・遅延シャドウバッファ・シェーダー変換キャッシュ）＋ Android API 34+ 向け任意 `GuestAngle`（GLES 3.2）。
- **PC 向け Android デスクトップ環境**:
  - ゼロ外部依存の軽量ネイティブ Java ホームアプリ **Velox Launcher**（`android/velox-launcher/`、約 54 KB、`INTERNET` 権限なし、背面 CPU 0.0%）。
  - PC ネイティブマウス（ホバー・右クリック・ホイール・ドラッグ＆ドロップ）、リモートデスクトップ／合成マウス対応（QEMU Patch 0016）、`F11` 全画面・ホーム復帰キー・クリップボード共有切替・1:1 入力限定ゲームモードキーマッパー（QEMU Patch 0017；連射マクロや自動照準は設計上禁止）。
  - Android 12（API 31）＋ Android 17（API 37）マルチバージョン管理、動的解像度・DPI 切替、Freeform マルチウィンドウ統合。
- **ハードウェア非依存 CLI ＆ 軽量 GUI（`velox/`）**:
  - Python 3.12 標準ライブラリのみで動作。ホスト自動診断・推奨設定生成（`emu recommend`）・プロファイル管理・テレメトリー・ネイティブ Windows ランチャー（`launcher/`）を搭載。

---

## 独立ベンチマーク結果（第 20 段階）

### 測定環境と条件

以下の実測値はすべて、**Windows 11 標準セキュリティ有効（VBS/HVCI 有効）**・オーバークロックなし・チェックサム完全検算済みの環境で取得したものです。

- **CPU**: Intel Core Ultra 7 265KF（8 P コア＋12 E コア、20 論理 CPU）
- **GPU**: NVIDIA GeForce RTX 5070 Ti 16 GB（ドライバ 610.88）
- **RAM**: DDR5-6000 96 GB（ゲスト RAM 16 GiB、ゲスト THP `always_nodefrag`）
- **ホスト OS / 仮想化**: Windows 11、WHPX + Hyper-V + VBS 有効
- **ゲスト OS**: 公式 Android 17（API 37、Google APIs、`x86_64`）
- **構成**: Velox 第 20 段階統合構成（gfxstream Patch 0031 + QEMU Patch 0018/0019 + `20_PE_rpe` 配置）

### Windows ネイティブ比較サマリー

| ワークロード / 指標 | Windows ネイティブ | Velox（第 20 段階） | 対ネイティブ比 |
|---|---:|---:|---:|
| **CPU 整数 1T**（`cpu-bench`, Clang） | 1.4779 Gops/s | **1.4442 Gops/s** | **97.7%** |
| **CPU 整数 MT (20T)**（`cpu-bench`, Clang） | 24.2103 Gops/s | **23.0290 Gops/s** | **95.1%** |
| **RAM コピー MT (256 MiB, 20T)**（`memory-bench`） | 38.39 GiB/s | **37.60 GiB/s** | **98.0%** |
| **Vulkan Draw Call**（`vk-render draws`, 1080p） | 1,699.15 fps | **1,596.2 fps**（最大 1,615.6） | **93.9%** |
| **Vulkan 同期**（`vk-render sync`, 1080p） | 14,519.20 fps | **最大 12,841.5 fps**（`submit_empty_wait` 3.99 µs / 最小 3.91 µs） | **最大 88.4%** |
| **GPU 律速描画**（`vk-game_demo` / GPU→GPU） | 119.94 fps (120fps 1%L) / 396 GB/s | **118.93–119.58 fps**（stutter 0）/ **405.2 GB/s** | **99.9%** |
| **GPU アップロード（64 MiB Staging→VRAM E2E）** | 16.45 GB/s | **14.13 GB/s** | **85.9%** |
| **GPU 読み戻し（64 MiB VRAM→Staging E2E）** | 13.99 GB/s | **11.24 GB/s** | **80.4%** |

> **実ゲーム性能に関する注意**: 上記の独立ベンチマークはエミュレーター各層（CPU・RAM・Vulkan 発行／同期・DMA・フレームペーシング）の性能特性を定量化するものであり、**あらゆる実ゲームの性能がネイティブ比同等になることを保証するものではありません**。実アプリでは ARM64 バイナリ変換（特にスカラー FP32/FP64）、シェーダーコンパイル、アセット I/O、GLES 経路の制約等が影響します。詳細は [docs/BENCHMARKS.md](docs/BENCHMARKS.md) および [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md) を参照してください。

### 適合性試験・AnTuTu 参考計測
- **公式 Android CTS dEQP**: 13 スイート計 **23,262 Pass / 0 Fail / 0 Crash / 0 Timeout**（`dEQP-VK.ray_query.*` **11,710 Pass**、`dEQP-VK.ray_tracing_pipeline.*` **5,665 Pass**、Vulkan 同期／Fence／Timeline／Semaphore／Sync2、Vulkan 1.4 任意 20 Pass、GLES3 同期／バッファ／テクスチャ；別途 `buffer.map.*` 400 Pass）。
- **AnTuTu V11.0.6 Standard（60 Hz・3 ペア交互 A/B・無改変 APK）**:
  - **正常判定カテゴリ**: GPU **655,918**（Ray Tracing **114,685** / Cottage **387,807** / Coastline **153,426**・上位 43%）、UX **1,315,277**（上位 99%）。
  - **異常判定付き参考値**（※スマートフォン想定上限超過・MEM 既知要因 KI-38 を含むためアプリ側が異常判定を表示）: 総合 **4,962,380**（最大 4,980,761）、CPU **2,163,077**、MEM **829,930**。異常判定付き参考値と正常判定スコアは常に区別して扱います。

---

## ドキュメント一覧

- [インストール手順](docs/INSTALL.md)
- [ソースからのビルド手順](docs/BUILD.md)
- [ベンチマーク詳細・測定条件](docs/BENCHMARKS.md)
- [既知の制約](docs/KNOWN_LIMITATIONS.md)
- [外部 Pull Request 取り込み手順](docs/PR_INTEGRATION.md)
- [コントリビューションガイド](CONTRIBUTING.md)
- [セキュリティポリシー](SECURITY.md)
- [変更履歴 (CHANGELOG)](CHANGELOG.md)
- [ロードマップ (ROADMAP)](ROADMAP.md)

---

## ライセンス

Velox は **マルチライセンス構成** を採用しており、上流パッチへ一律に Apache License 2.0 を適用しません。

- **Velox 独自コード**（`velox/`, `android/velox-launcher/`, `launcher/`, `scripts/`, `benchmarks/`, `tests/`, `docs/`）: **Apache License 2.0**（詳細は [LICENSE](LICENSE)）
- **上流パッチ群**（`patches/`）: 各上流プロジェクトのライセンスに従います — **GPL-2.0**（`patches/emulator/`）、**Apache-2.0**（`patches/gfxstream/`, `patches/aemu/`）、**MIT**（`patches/mesa3d/`）

詳細は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) および [`LICENSES/`](LICENSES/) を参照してください。
