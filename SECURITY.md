# Security Policy

## Security Boundaries

Velox is designed to run Android emulators on Windows **without** disabling OS or guest protections:

- Windows **VBS / HVCI / Hyper-V** remain enabled.
- Android **SELinux / AVB dm-verity / Scudo guard pages** remain unmodified.
- Storage durability honors guest flushes (`cache=unsafe` is prohibited).
- Windows file sharing is strictly limited to an explicitly designated `shared/` folder with path-traversal rejection.
- `Velox Launcher` requests **zero** `INTERNET` permissions and runs no background telemetry or ads.
- CI runs exclusively on GitHub-hosted runners; untrusted pull requests are never executed on maintainer hardware.

## Reporting a Vulnerability

If you discover a security issue (such as a host/guest boundary escape, path traversal, arbitrary command execution, credential leak, or unsafe default):

1. **Do not** open a public issue with exploit details.
2. Use GitHub **Private Vulnerability Reporting** (Security Advisories) on this repository to report the issue privately to the maintainers.
3. Include a minimal reproduction, affected component/version, and suggested mitigation if known.

We will acknowledge reports promptly and coordinate a fix and disclosure timeline before publishing details.
