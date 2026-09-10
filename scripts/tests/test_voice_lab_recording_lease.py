"""Compile the actual monotonic lease and local-request rules as constexpr tests."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest


class RecordingLeaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[2]
        cls.compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not cls.compiler:
            raise unittest.SkipTest("CXX/clang++/g++ is required for lease tests")

    def check_lease(self, scenario):
        command = [self.compiler, "-std=c++17", "-fsyntax-only", "-fno-rtti", "-fno-exceptions"]
        if "esp-clang" in self.compiler:
            command += ["--target=xtensa-esp-elf", "-mcpu=esp32s3"]
        command += ["-I", str(self.root / "main"), "-DTEST_CASE=" + str(scenario),
                    str(Path(__file__).with_name("voice_lab_recording_lease_test.cc"))]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_local_request_matching_and_expiry(self):
        self.check_lease(0)

    def test_initial_lease_uses_receipt_time(self):
        self.check_lease(1)

    def test_renewal_uses_request_time_and_rejects_duplicate(self):
        self.check_lease(2)

    def test_late_wrong_and_unsolicited_renewals(self):
        self.check_lease(3)

    def test_absolute_maximum_cannot_be_extended(self):
        self.check_lease(4)

    def test_old_session_renewal_cannot_restart(self):
        self.check_lease(5)

    def test_duration_bounds_and_shorter_lease(self):
        self.check_lease(6)

    def test_unlimited_total_duration_renews_beyond_one_hour(self):
        self.check_lease(7)

    def test_unlimited_total_duration_still_expires_without_renewal(self):
        self.check_lease(8)

    def test_customer_maximum_returns_after_unlimited_session(self):
        self.check_lease(9)
