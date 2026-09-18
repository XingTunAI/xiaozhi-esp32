"""Compile-time fault scenarios execute the production send loop with a fake clock/TLS writer."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest


class TlsWritePolicyTests(unittest.TestCase):
    def test_deadline_partial_writes_and_retry_identity(self):
        compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('CXX/clang++/g++ is required')
        root = Path(__file__).resolve().parents[2]
        command = [compiler, '-std=c++17', '-fsyntax-only']
        if 'esp-clang' in compiler:
            command += ['--target=xtensa-esp-elf', '-mcpu=esp32s3']
        command += ['-I', str(root / 'local_components/esp-ml307/src/esp'),
                    str(Path(__file__).with_name('tls_write_policy_test.cc'))]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
