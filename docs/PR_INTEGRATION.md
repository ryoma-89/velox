# External Pull Request Integration Workflow

Velox maintains a strict separation between the public OSS repository (`velox`) and the private engineering repository (`velox-dev`). External contributions are welcome and are integrated through a rigorous, contributor-preserving pipeline.

## Review & Integration Steps

1. **Automated Public CI (GitHub-Hosted Runners Only)**:
   - Every PR automatically runs unit tests (`unittest`), syntax checks (`compileall`), profile JSON schema validation, patch syntax validation, secret/PII scanning, license verification, and Markdown link checks on `ubuntu-latest` and `windows-latest`.
   - Maintainer hardware is **never** registered as a self-hosted runner for public PRs.
2. **Maintainer Code & Security Review**:
   - Verify no security regressions (VBS/HVCI, SELinux/AVB/Scudo, storage flush durability, path traversal, or benchmark/GPU spoofing).
3. **License Verification**:
   - Confirm Velox-original code matches Apache License 2.0 and any `patches/` edits comply with the exact upstream license (`GPL-2.0` for QEMU, `Apache-2.0` for gfxstream/aemu, `MIT` for Mesa).
   - Verify copyright headers and attribution are intact.
4. **Isolated Intake into `velox-dev`**:
   - Maintainers import the PR patch onto a dedicated branch (`pr/<number>-<slug>`) in `velox-dev` using `git am --3way`, preserving the contributor's exact Git `Author`, timestamp, commit message, and copyright notices.
5. **Local Build, Unit Tests & Hardware Regression**:
   - Build affected components (if any patches changed), run `python -m unittest discover -s tests` and `scripts/audit_public_release.py`, and execute real-device regression (dEQP + alternating A/B benchmarks if performance-related) on an isolated clone AVD.
6. **Merge & Next Public Release Sync**:
   - Merge the verified branch into `velox-dev` (`develop` → `main`), update `CHANGELOG.md`, and sync back to `velox` via the allowlist release pipeline (`prepare_public_release.py` → `audit_public_release.py` → `publish_release.py`).
