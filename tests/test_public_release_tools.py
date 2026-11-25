"""Unit tests for public release audit, allowlist extraction, and publish safety gates."""
from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "scripts"))

import audit_public_release as auditor

try:
    import publish_release as publisher
except ImportError:
    publisher = None


class PublicReleaseAuditTests(unittest.TestCase):
    def test_detects_secret_and_prohibited_extension(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # Construct synthetic token via concatenation so static scanners & GitHub push protection do not flag the test itself.
            fake_tok = "ghp" + "_" + "1234567890abcdefghij123456"
            (root / "secret.txt").write_text(f"token = {fake_tok}\n", encoding="utf-8")
            (root / "bad.apk").write_bytes(b"PK\x03\x04")
            (root / "key.jks").write_bytes(b"fake")
            res = auditor.audit_repository(root, mode="dev")
            self.assertFalse(res["passed"])
            checks = {v["check"] for v in res["violations"]}
            self.assertIn("secret", checks)
            self.assertIn("prohibited_file", checks)

    def test_detects_pii_only_in_public_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_path = "C:\\Users\\" + "Ryoma\\foo"
            (root / "note.md").write_text(f"Path: {fake_path}\n", encoding="utf-8")
            res_pub = auditor.audit_repository(root, mode="public")
            self.assertFalse(res_pub["passed"])
            self.assertTrue(any(v["check"] == "pii_or_local_path" for v in res_pub["violations"]))

    @unittest.skipIf(publisher is None, "publish_release.py is dev-repo only")
    def test_publish_blocks_mirror_and_force_push(self):
        with self.assertRaises(RuntimeError):
            publisher.run_git(REPO_ROOT, "push", "--mirror", "origin")
        with self.assertRaises(RuntimeError):
            publisher.run_git(REPO_ROOT, "push", "--force", "origin", "main")

    def test_current_repo_audit_passes(self):
        mode = "public" if (REPO_ROOT / "THIRD_PARTY_NOTICES.md").is_file() else "dev"
        res = auditor.audit_repository(REPO_ROOT, mode=mode)
        self.assertTrue(res["passed"], f"Audit ({mode}) violations: {res['violations'][:5]}")
        self.assertGreaterEqual(res["patch_count"], 80)


if __name__ == "__main__":
    unittest.main()
