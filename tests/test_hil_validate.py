from __future__ import annotations

import pathlib
import sys
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import hil_validate  # noqa: E402


def state(*, profile="mitm", main=True, panel=True, available=True,
          main_frames=10, panel_frames=10, checksum=0, framing=0, power=False,
          panel_emulated=False):
    return {
        "profile": profile,
        "link": {"main": main, "panel": panel},
        "control": {"available": available, "pending": False, "status": "none"},
        "state": {"power": power, "mode": "cool", "fan": "low", "setpoint_c": 18,
                  "panel_emulated": panel_emulated},
        "counters": {"main_frames": main_frames, "panel_frames": panel_frames,
                     "checksum_errors": checksum, "framing_errors": framing},
    }


class HilValidateTests(unittest.TestCase):
    def test_safe_passive_fails_before_any_control(self):
        with self.assertRaisesRegex(hil_validate.HilGateError, "active bridge profile"):
            hil_validate.require_bidirectional_mitm(state(profile="safe-passive", available=False))

    def test_tap_inject_is_an_active_physical_bridge_profile(self):
        hil_validate.require_bidirectional_mitm(state(profile="tap-inject"))

    def test_hybrid_is_an_active_physical_bridge_profile(self):
        hil_validate.require_bidirectional_mitm(state(profile="hybrid-bridge"))

    def test_mitm_nts_is_an_active_physical_bridge_profile(self):
        hil_validate.require_bidirectional_mitm(state(profile="mitm-nts"))

    def test_acceptance_matrix_contains_only_supported_controls(self):
        requests = [request for request, _expected in hil_validate.supported_scenarios()]
        self.assertFalse(any("timer" in request for request in requests))
        self.assertTrue(any(request.get("quiet") is True for request in requests))
        self.assertTrue(any(request.get("units_fahrenheit") is True for request in requests))

    def test_missing_panel_fails_closed(self):
        with self.assertRaisesRegex(hil_validate.HilGateError, "panel link"):
            hil_validate.require_bidirectional_mitm(state(panel=False, available=False))

    def test_emulated_panel_cannot_pass_final_hil_gate(self):
        with self.assertRaisesRegex(hil_validate.HilGateError, "emulated"):
            hil_validate.require_bidirectional_mitm(state(panel_emulated=True))

    def test_transparent_growth_requires_both_directions(self):
        before = state(main_frames=10, panel_frames=10)
        after = state(main_frames=20, panel_frames=10)
        with self.assertRaisesRegex(hil_validate.HilGateError, "panel_frames"):
            hil_validate.require_transparent_growth(before, after)

    def test_transparent_growth_rejects_new_errors(self):
        before = state(main_frames=10, panel_frames=10)
        after = state(main_frames=20, panel_frames=20, checksum=1)
        with self.assertRaisesRegex(hil_validate.HilGateError, "checksum_errors"):
            hil_validate.require_transparent_growth(before, after)

    def test_issue_requires_confirmation(self):
        config = hil_validate.HilConfig("http://device", "token", "http://ha", "ha-token",
                                        "sensor.power", 5, 2, 10, 10, 1)
        confirmed = state(power=True)
        confirmed["control"].update({"sequence": 4, "status": "confirmed"})
        with mock.patch.object(hil_validate.ac_control, "request_json",
                               side_effect=[state(), {"sequence": 4}]), \
             mock.patch.object(hil_validate.ac_control, "wait_for_command",
                               return_value=confirmed):
            result = hil_validate.issue_confirmed(config, {"power": True})
        self.assertEqual(result["control"]["status"], "confirmed")

    def test_power_unit_kw_is_normalized(self):
        config = hil_validate.HilConfig("http://device", "token", "http://ha", "ha-token",
                                        "sensor.power", 5, 2, 10, 10, 1)
        with mock.patch.object(hil_validate, "request_ha_json", return_value={
            "state": "0.125", "attributes": {"unit_of_measurement": "kW"}
        }):
            self.assertEqual(hil_validate.read_power_watts(config), 125.0)

    def test_unattended_run_requires_initial_off_state(self):
        config = hil_validate.HilConfig("http://device", "token", "http://ha", "ha-token",
                                        "sensor.power", 5, 2, 10, 10, 0)
        with mock.patch.object(hil_validate.ac_control, "request_json",
                               return_value=state(power=True)), \
             mock.patch.object(hil_validate, "read_power_watts", return_value=20.0):
            with self.assertRaisesRegex(hil_validate.HilGateError, "must be OFF"):
                hil_validate.run_acceptance(config, 1)

    def test_best_effort_shutdown_confirms_off_and_power(self):
        config = hil_validate.HilConfig("http://device", "token", "http://ha", "ha-token",
                                        "sensor.power", 5, 2, 10, 10, 1)
        stopped = state(power=False)
        stopped["control"].update({"sequence": 9, "status": "confirmed"})
        with mock.patch.object(hil_validate.ac_control, "request_json",
                               return_value=state(power=True)), \
             mock.patch.object(hil_validate, "issue_confirmed", return_value=stopped) as issue, \
             mock.patch.object(hil_validate, "wait_for_power", return_value=1.0) as power:
            hil_validate.best_effort_shutdown(config)
        issue.assert_called_once_with(config, {"power": False})
        power.assert_called_once_with(config, maximum=2)

    def test_power_request_arms_shutdown_before_confirmation(self):
        config = hil_validate.HilConfig("http://device", "token", "http://ha", "ha-token",
                                        "sensor.power", 5, 2, 10, 10, 0)
        initial = state(main_frames=10, panel_frames=10, power=False)
        observed = state(main_frames=20, panel_frames=20, power=False)
        with mock.patch.object(hil_validate.ac_control, "request_json",
                               side_effect=[initial, observed]), \
             mock.patch.object(hil_validate, "read_power_watts", return_value=1.0), \
             mock.patch.object(hil_validate.time, "sleep"), \
             mock.patch.object(hil_validate, "issue_confirmed",
                               side_effect=hil_validate.HilGateError("confirmation timeout")), \
             mock.patch.object(hil_validate, "best_effort_shutdown") as shutdown:
            with self.assertRaisesRegex(hil_validate.HilGateError, "confirmation timeout"):
                hil_validate.run_acceptance(config, 1)
        shutdown.assert_called_once_with(config)


if __name__ == "__main__":
    unittest.main()
