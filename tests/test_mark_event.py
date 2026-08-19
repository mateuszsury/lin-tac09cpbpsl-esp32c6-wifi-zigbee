from __future__ import annotations

import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import mark_event  # noqa: E402


class MarkerTests(unittest.TestCase):
    def test_accepts_short_safe_label(self) -> None:
        self.assertEqual(mark_event.validate_label("TEMP+ test 2"), "TEMP+ test 2")

    def test_rejects_delimiters_and_controls(self) -> None:
        for label in ("", "bad,label", 'bad"label', "bad\\label", "bad\nlabel"):
            with self.assertRaises(ValueError):
                mark_event.validate_label(label)

    def test_rejects_oversized_utf8_label(self) -> None:
        with self.assertRaises(ValueError):
            mark_event.validate_label("é" * 48)


if __name__ == "__main__":
    unittest.main()
