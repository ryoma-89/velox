## Summary
<!-- Describe what this PR changes and why. -->

## Type of Change
- [ ] Bug fix
- [ ] New feature
- [ ] Performance optimization (attach alternating A/B methodology & hardware conditions)
- [ ] Documentation / CI / Tooling

## Verification Checklist
- [ ] `python -m unittest discover -s tests -v` passes
- [ ] `python scripts/audit_public_release.py --mode public --root .` passes with 0 violations
- [ ] No third-party binaries, APKs, SDK images, AVDs, disk images, keys, or secrets included
- [ ] Security & integrity boundaries preserved (VBS/HVCI, SELinux/AVB/Scudo, durable storage, no spoofing)
- [ ] License compliance confirmed (Velox original code: Apache-2.0; `patches/` match upstream project license — see `THIRD_PARTY_NOTICES.md`)
