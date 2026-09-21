import os
from pathlib import Path
import shutil
import subprocess
import unittest


class ScaleStateTests(unittest.TestCase):
    def test_scale_behaviour(self):
        compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('CXX/clang++/g++ is required')
        root = Path(__file__).resolve().parents[2]
        command = [compiler, '-std=c++17', '-fsyntax-only']
        if 'esp-clang' in compiler:
            command += ['--target=xtensa-esp-elf', '-mcpu=esp32s3']
        command += ['-I', str(root / 'main'), str(Path(__file__).with_name('scale_state_test.cc'))]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
