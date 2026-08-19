from __future__ import annotations

import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import analyze_protocol  # noqa: E402


PANEL = bytes.fromhex("54440B113100120000400000000079")
MAIN = bytes.fromhex("54441211310012000000002C2C00400A00000000006A")


class AnalyzeProtocolTests(unittest.TestCase):
    def test_extracts_noise_and_concatenated_frames(self) -> None:
        stream = b"\x00\xff" + PANEL + MAIN + b"\xaa"
        self.assertEqual(analyze_protocol.complete_frames(stream), [PANEL, MAIN])

    def test_rejects_truncated_frame(self) -> None:
        self.assertEqual(analyze_protocol.complete_frames(PANEL[:-1]), [])

    def test_classifies_by_wire_frame_not_stale_log_label(self) -> None:
        records = [
            {
                "host_time": "now",
                "text": "UART,t_us=1,direction=panel_to_main,gpio=6,baud=9600,"
                f"format=8N1,len=22,hex={MAIN.hex().upper()}",
            }
        ]
        frames = analyze_protocol.uart_frames(records)
        self.assertEqual(frames["panel_to_main"], [])
        self.assertEqual(frames["main_to_panel"], [("now", MAIN)])

    def test_current_passive_capture_format(self) -> None:
        records = [
            {
                "host_time": "panel",
                "text": f"PASSIVE,t_us=1,gpio=7,len=15,hex={PANEL.hex().upper()}",
            },
            {
                "host_time": "main",
                "text": f"PASSIVE,t_us=2,gpio=4,len=22,hex={MAIN.hex().upper()}",
            },
        ]
        frames = analyze_protocol.framed_log_frames(records)
        self.assertEqual(frames["panel_to_main"], [("panel", PANEL)])
        self.assertEqual(frames["main_to_panel"], [("main", MAIN)])

    def test_checksum_target(self) -> None:
        self.assertEqual(analyze_protocol.xor_bytes(PANEL), 0x10)
        self.assertEqual(analyze_protocol.xor_bytes(MAIN), 0x10)


if __name__ == "__main__":
    unittest.main()
