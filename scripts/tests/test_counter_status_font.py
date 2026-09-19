"""Catch runtime status text added without regenerating the display font."""
from pathlib import Path
import re
import unittest


class CounterStatusFontTests(unittest.TestCase):
    def test_recording_status_glyphs_are_packaged(self):
        board = Path(__file__).resolve().parents[2] / 'main/boards/waveshare/esp32-p4-wifi6-dev-kit-b'
        font = (board / 'counter_font_20.c').read_text(encoding='utf-8')
        start = int(re.search(r'\.range_start=(\d+)', font)[1])
        offsets = re.search(r'unicode_list\[\]=\{([^}]+)', font)[1]
        included = {chr(start + int(value)) for value in offsets.split(',')}
        source = (board / 'counter_ui.cc').read_text(encoding='utf-8')
        literals = re.findall(r'"(?:[^"\\]|\\.)*"', source)
        needed = {char for text in literals for char in text if ord(char) >= 128}
        self.assertFalse(needed - included, 'Missing screen glyphs: ' + ''.join(sorted(needed - included)))
