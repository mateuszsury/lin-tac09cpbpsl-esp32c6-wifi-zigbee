from __future__ import annotations

import argparse
import pathlib
import sys
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ac_control  # noqa: E402


def options(**values: object) -> argparse.Namespace:
    defaults = {
        "power": None,
        "setpoint": None,
        "mode": None,
        "fan": None,
        "raw_mode_fan": None,
        "quiet": None,
        "units": None,
        "timer": None,
    }
    defaults.update(values)
    return argparse.Namespace(**defaults)


class AcControlTests(unittest.TestCase):
    def test_builds_complete_request(self) -> None:
        self.assertEqual(
            ac_control.build_control_body(
                options(power="on", setpoint=21, mode="cool", fan="low")
            ),
            {"power": True, "setpoint_c": 21, "mode": "cool", "fan": "low"},
        )

    def test_accepts_raw_code(self) -> None:
        self.assertEqual(
            ac_control.build_control_body(options(raw_mode_fan=0x31)),
            {"raw_mode_fan_code": 0x31},
        )

    def test_builds_verified_feature_flags(self) -> None:
        self.assertEqual(
            ac_control.build_control_body(
                options(quiet="on", units="fahrenheit", timer="off")
            ),
            {"quiet": True, "units_fahrenheit": True, "timer": False},
        )

    def test_rejects_invalid_temperature(self) -> None:
        with self.assertRaises(ValueError):
            ac_control.build_control_body(options(setpoint=33))

    def test_rejects_conflicting_raw_code(self) -> None:
        with self.assertRaises(ValueError):
            ac_control.build_control_body(options(mode="cool", raw_mode_fan=0x31))

    def test_rejects_empty_request(self) -> None:
        with self.assertRaises(ValueError):
            ac_control.build_control_body(options())

    def test_wait_requires_explicit_confirmation(self) -> None:
        responses = iter(
            [
                {"control": {"sequence": 7, "pending": True, "status": "pending"}},
                {"control": {"sequence": 7, "pending": False, "status": "confirmed"}},
            ]
        )
        with mock.patch.object(ac_control, "request_json", side_effect=lambda *_a, **_k: next(responses)), \
             mock.patch.object(ac_control.time, "sleep"):
            result = ac_control.wait_for_command("http://device", 7, 1.0)
        self.assertEqual(result["control"]["status"], "confirmed")

    def test_wait_rejects_timeout_outcome(self) -> None:
        response = {"control": {"sequence": 8, "pending": False, "status": "timed_out"}}
        with mock.patch.object(ac_control, "request_json", return_value=response):
            with self.assertRaisesRegex(RuntimeError, "command 8 timed_out"):
                ac_control.wait_for_command("http://device", 8, 1.0)


if __name__ == "__main__":
    unittest.main()
