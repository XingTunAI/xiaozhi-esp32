"""Evaluate the actual firmware guard with C++ constexpr/static_assert tests."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest


class RecordingGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[2]
        cls.compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not cls.compiler:
            raise unittest.SkipTest("CXX/clang++/g++ is required for guard tests")

    def check_guard(self, scenario):
        command = [self.compiler, "-std=c++17", "-fsyntax-only", "-fno-rtti", "-fno-exceptions"]
        if "esp-clang" in self.compiler:
            command += ["--target=xtensa-esp-elf", "-mcpu=esp32s3"]
        command += ["-I", str(self.root / "main"), "-DTEST_CASE=" + str(scenario),
                    str(Path(__file__).with_name("voice_lab_recording_guard_test.cc"))]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return 0

    def test_boot_and_reconnect_require_idle(self):
        self.assertEqual(self.check_guard(0), 0)

    def test_duplicates_and_out_of_order_commands(self):
        self.assertEqual(self.check_guard(1), 0)

    def test_one_recording_at_a_time(self):
        self.assertEqual(self.check_guard(2), 0)

    def test_local_stop_and_second_test(self):
        self.assertEqual(self.check_guard(3), 0)

    def test_idle_idempotency_and_revision_boundary(self):
        self.assertEqual(self.check_guard(4), 0)

    def test_stop_received_during_startup(self):
        self.assertEqual(self.check_guard(5), 0)
