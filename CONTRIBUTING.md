# Contributing to Velox

Thank you for your interest in contributing to Velox!

## Core Engineering Principles

1. **Honest Measurement & Verification**:
   - Every performance claim must be backed by reproducible alternating A/B measurements with output/checksum verification.
   - Never fabricate or extrapolate unmeasured numbers, and never present synthetic benchmark ratios as universal real-world game FPS.
2. **No Security or Integrity Compromises**:
   - Do not submit changes that weaken Windows security (VBS/HVCI), Android security (SELinux/AVB/Scudo), storage durability (`cache=unsafe`), or spoof GPU/device identities, bypass anti-cheat/DRM/Play integrity, or add benchmark-specific shortcuts.
   - Game Mode input mapping is strictly limited to 1:1 input translation (no macros, rapid-fire, auto-aim, or recoil compensation).
3. **Zero External Python Dependencies (`velox/`)**:
   - Keep `velox/` compatible with Python 3.12 standard library only unless explicitly approved via an issue.

## Licensing of Contributions

Velox is multi-licensed (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)):

- Contributions to **Velox original code** (`velox/`, `android/`, `launcher/`, `scripts/`, `benchmarks/`, `tests/`, `docs/`) are submitted under the **Apache License 2.0** (matching [LICENSE](LICENSE)).
- Contributions to **upstream patches** under `patches/` **must** comply with the respective upstream license:
  - `patches/emulator/` → **GPL-2.0-only / GPL-2.0-or-later**
  - `patches/gfxstream/` & `patches/aemu/` → **Apache-2.0**
  - `patches/mesa3d/` → **MIT**
- Never commit third-party binaries, APKs, Android SDK files, system images, AVDs, disk images, signing keys, or credentials.

## Pull Request Checklist

Before opening a Pull Request:

1. Run unit tests:
   ```bash
   python -m unittest discover -s tests -v
   ```
2. Run the repository audit script:
   ```bash
   python scripts/audit_public_release.py --mode public --root .
   ```
3. Ensure all profile JSON files pass schema validation and any modified `.patch` files are clean unified diffs.
4. Clearly describe your test environment, changes, and verification steps in the PR template.

See [docs/PR_INTEGRATION.md](docs/PR_INTEGRATION.md) for how maintainers review, test on hardware, and integrate accepted PRs while preserving contributor authorship and copyright notices.
