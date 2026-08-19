from __future__ import annotations

import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import export_capture  # noqa: E402


class ExportCaptureTests(unittest.TestCase):
    def test_parses_and_validates_monotonic_archive(self) -> None:
        records = export_capture.parse_ndjson(
            '{"sequence":4,"device_time_us":10,"text":"CAPTURE,event=boot"}\n'
            '{"sequence":7,"device_time_us":20,"text":"FRAME,hex=5444"}\n'
        )
        export_capture.validate_records(records, after=3)
        self.assertEqual([record["sequence"] for record in records], [4, 7])

    def test_rejects_invalid_json(self) -> None:
        with self.assertRaisesRegex(ValueError, "line 1"):
            export_capture.parse_ndjson("not-json\n")

    def test_rejects_non_monotonic_or_invalid_records(self) -> None:
        bad_records = [
            {"sequence": 2, "device_time_us": 10, "text": "first"},
            {"sequence": 2, "device_time_us": 11, "text": "duplicate"},
        ]
        with self.assertRaisesRegex(ValueError, "non-monotonic"):
            export_capture.validate_records(bad_records)
        with self.assertRaisesRegex(ValueError, "device_time_us"):
            export_capture.validate_records(
                [{"sequence": 1, "device_time_us": -1, "text": "bad"}]
            )


if __name__ == "__main__":
    unittest.main()
