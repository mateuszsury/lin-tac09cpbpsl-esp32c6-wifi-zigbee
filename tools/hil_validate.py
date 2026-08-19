#!/usr/bin/env python3
"""Fail-closed physical acceptance runner for the Klima WiFi MITM.

The runner never enables MITM and never bypasses firmware guards.  It only
issues commands after the installed image reports a fresh, bidirectional link
and control availability.  Home Assistant is used as an independent power
meter, not as proof of UART delivery.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass

import ac_control
from http_utils import normalize_http_base_url, urlopen_http


class HilGateError(RuntimeError):
    """A mandatory acceptance gate was not satisfied."""


@dataclass(frozen=True)
class HilConfig:
    base_url: str
    device_token: str
    ha_url: str
    ha_token: str
    power_entity: str
    on_min_watts: float
    off_max_watts: float
    command_timeout: float
    power_timeout: float
    link_observation: float


def request_ha_json(config: HilConfig, path: str) -> dict:
    request = urllib.request.Request(
        config.ha_url.rstrip("/") + path,
        headers={"Authorization": f"Bearer {config.ha_token}", "Accept": "application/json"},
    )
    try:
        with urlopen_http(request, timeout=10.0) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        raise HilGateError(f"Home Assistant HTTP {error.code}: {detail[:160]}") from error


def read_power_watts(config: HilConfig) -> float:
    state = request_ha_json(config, f"/api/states/{config.power_entity}")
    try:
        value = float(state["state"])
    except (KeyError, TypeError, ValueError) as error:
        raise HilGateError(
            f"power entity {config.power_entity!r} has non-numeric state"
        ) from error
    unit = str(state.get("attributes", {}).get("unit_of_measurement", "W")).strip().lower()
    if unit == "kw":
        value *= 1000.0
    elif unit not in {"w", "watt", "watts"}:
        raise HilGateError(f"unsupported power unit {unit!r} for {config.power_entity}")
    return value


def require_bidirectional_mitm(state: dict) -> None:
    profile = state.get("profile")
    link = state.get("link", {})
    control = state.get("control", {})
    decoded = state.get("state", {})
    reasons: list[str] = []
    if profile not in {"mitm", "mitm-nts", "tap-inject", "hybrid-bridge"}:
        reasons.append(f"profile={profile!r}, expected an active bridge profile")
    if not link.get("main"):
        reasons.append("main link is not fresh")
    if not link.get("panel"):
        reasons.append("panel link is not fresh")
    if decoded.get("panel_emulated"):
        reasons.append("panel link is emulated, not physical")
    if not control.get("available"):
        reasons.append("firmware control gate is closed")
    if control.get("pending"):
        reasons.append("another command is pending")
    if reasons:
        raise HilGateError("; ".join(reasons))


def require_transparent_growth(before: dict, after: dict) -> None:
    require_bidirectional_mitm(after)
    first = before.get("counters", {})
    second = after.get("counters", {})
    for name in ("main_frames", "panel_frames"):
        if int(second.get(name, 0)) <= int(first.get(name, 0)):
            raise HilGateError(f"{name} did not increase during link observation")
    for name in ("checksum_errors", "framing_errors"):
        if int(second.get(name, 0)) != int(first.get(name, 0)):
            raise HilGateError(f"{name} increased during transparent observation")


def wait_for_power(config: HilConfig, *, minimum: float | None = None,
                   maximum: float | None = None) -> float:
    deadline = time.monotonic() + config.power_timeout
    last = read_power_watts(config)
    while time.monotonic() < deadline:
        if minimum is not None and last >= minimum:
            return last
        if maximum is not None and last <= maximum:
            return last
        time.sleep(1.0)
        last = read_power_watts(config)
    relation = f">= {minimum:g} W" if minimum is not None else f"<= {maximum:g} W"
    raise HilGateError(f"power did not become {relation}; last={last:g} W")


def issue_confirmed(config: HilConfig, body: dict) -> dict:
    require_bidirectional_mitm(ac_control.request_json(config.base_url, "/api/ac-state"))
    accepted = ac_control.request_json(
        config.base_url,
        "/api/control",
        token=config.device_token,
        body=body,
    )
    sequence = int(accepted["sequence"])
    confirmed = ac_control.wait_for_command(config.base_url, sequence, config.command_timeout)
    if confirmed.get("control", {}).get("status") != "confirmed":
        raise HilGateError(f"sequence {sequence} lacked explicit confirmation")
    return confirmed


def assert_state(state: dict, expected: dict) -> None:
    actual = state.get("state", {})
    aliases = {
        "setpoint_c": "setpoint_c", "power": "power", "mode": "mode", "fan": "fan",
        "quiet": "quiet", "units_fahrenheit": "units_fahrenheit", "timer": "timer",
    }
    differences = [
        f"{key}: expected {value!r}, got {actual.get(aliases[key])!r}"
        for key, value in expected.items()
        if actual.get(aliases[key]) != value
    ]
    if differences:
        raise HilGateError("confirmed response mismatch: " + "; ".join(differences))


def best_effort_shutdown(config: HilConfig) -> None:
    """Leave the compressor command OFF after any partially completed run."""
    current = ac_control.request_json(config.base_url, "/api/ac-state")
    require_bidirectional_mitm(current)
    stopped = issue_confirmed(config, {"power": False})
    assert_state(stopped, {"power": False})
    wait_for_power(config, maximum=config.off_max_watts)


def supported_scenarios() -> list[tuple[dict, dict]]:
    """Return only commands whose complete wire encoding is HIL-proven."""
    return [
        ({"power": True, "mode": "cool", "fan": "low", "setpoint_c": 18},
         {"power": True, "mode": "cool", "fan": "low", "setpoint_c": 18}),
        ({"setpoint_c": 19}, {"setpoint_c": 19}),
        ({"setpoint_c": 18}, {"setpoint_c": 18}),
        ({"fan": "high"}, {"fan": "high"}),
        ({"fan": "low"}, {"fan": "low"}),
        ({"mode": "fan"}, {"mode": "fan"}),
        ({"mode": "dry"}, {"mode": "dry"}),
        ({"mode": "cool", "fan": "low", "setpoint_c": 18},
         {"mode": "cool", "fan": "low", "setpoint_c": 18}),
        ({"quiet": True}, {"quiet": True}),
        ({"quiet": False}, {"quiet": False}),
        ({"units_fahrenheit": True}, {"units_fahrenheit": True}),
        ({"units_fahrenheit": False}, {"units_fahrenheit": False}),
    ]


def run_acceptance(config: HilConfig, repeats: int) -> dict:
    started = time.time()
    initial = ac_control.request_json(config.base_url, "/api/ac-state")
    require_bidirectional_mitm(initial)
    initial_power = read_power_watts(config)
    if initial.get("state", {}).get("power"):
        raise HilGateError("initial appliance state must be OFF before unattended HIL")
    if initial_power > config.off_max_watts:
        raise HilGateError(
            f"initial power is {initial_power:g} W, expected <= {config.off_max_watts:g} W while OFF"
        )
    time.sleep(config.link_observation)
    observed = ac_control.request_json(config.base_url, "/api/ac-state")
    require_transparent_growth(initial, observed)

    commands: list[dict] = []
    powered_by_runner = False
    primary_error: BaseException | None = None
    try:
        for attempt in range(1, repeats + 1):
            for body, expected in supported_scenarios():
                if body.get("power") is True:
                    # The request may reach the appliance even if confirmation
                    # later times out, so cleanup must already be armed.
                    powered_by_runner = True
                state = issue_confirmed(config, body)
                assert_state(state, expected)
                commands.append({"attempt": attempt, "request": body,
                                 "sequence": state["control"]["sequence"], "result": "confirmed"})

            power_on = wait_for_power(config, minimum=config.on_min_watts)
            state = issue_confirmed(config, {"power": False})
            assert_state(state, {"power": False})
            powered_by_runner = False
            power_off = wait_for_power(config, maximum=config.off_max_watts)
            commands.append({"attempt": attempt, "request": {"power": False},
                             "sequence": state["control"]["sequence"], "result": "confirmed",
                             "power_before_off_w": power_on, "power_after_off_w": power_off})
    except BaseException as error:  # also clean up after Ctrl-C/SystemExit
        primary_error = error
    finally:
        cleanup_error: Exception | None = None
        if powered_by_runner:
            try:
                best_effort_shutdown(config)
            except Exception as error:  # surface an unsafe final state explicitly
                cleanup_error = error
        if primary_error is not None:
            if cleanup_error is not None:
                raise HilGateError(
                    f"{primary_error}; emergency shutdown also failed: {cleanup_error}"
                ) from primary_error
            raise primary_error

    final = ac_control.request_json(config.base_url, "/api/ac-state")
    require_transparent_growth(observed, final)
    if final.get("state", {}).get("power"):
        raise HilGateError("final appliance state is not OFF")
    return {
        "ok": True,
        "started_epoch": started,
        "duration_s": round(time.time() - started, 3),
        "repeats": repeats,
        "initial_power_w": initial_power,
        "commands": commands,
        "final": final,
    }


def build_config(args: argparse.Namespace) -> HilConfig:
    device = ac_control.load_device(args.device)
    base_url = args.base_url or device.get("base_url")
    device_token = device.get("token")
    ha_token = os.environ.get("HA_TOKEN") or os.environ.get("HOME_ASSISTANT_TOKEN")
    ha_url = args.ha_url or os.environ.get("HA_URL") or os.environ.get(
        "HOME_ASSISTANT_URL"
    )
    missing = [name for name, value in (
        ("base_url", base_url), ("device token", device_token),
        ("HA URL", ha_url), ("HA token", ha_token)
    ) if not value]
    if missing:
        raise HilGateError("missing " + ", ".join(missing))
    return HilConfig(
        base_url=normalize_http_base_url(str(base_url)), device_token=str(device_token),
        ha_url=normalize_http_base_url(str(ha_url)), ha_token=str(ha_token),
        power_entity=args.power_entity, on_min_watts=args.on_min_watts,
        off_max_watts=args.off_max_watts, command_timeout=args.command_timeout,
        power_timeout=args.power_timeout, link_observation=args.link_observation,
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--device", type=pathlib.Path, default=pathlib.Path(".local/device.json"))
    result.add_argument("--base-url")
    result.add_argument("--ha-url")
    result.add_argument(
        "--power-entity",
        default=os.environ.get("HOME_ASSISTANT_POWER_ENTITY"),
        help="Home Assistant power sensor entity (or HOME_ASSISTANT_POWER_ENTITY)",
    )
    result.add_argument("--on-min-watts", type=float, default=5.0)
    result.add_argument("--off-max-watts", type=float, default=2.0)
    result.add_argument("--command-timeout", type=float, default=10.0)
    result.add_argument("--power-timeout", type=float, default=90.0)
    result.add_argument("--link-observation", type=float, default=2.0)
    result.add_argument("--repeats", type=int, default=3)
    result.add_argument("--output", type=pathlib.Path)
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if not 1 <= args.repeats <= 10:
            raise HilGateError("repeats must be between 1 and 10")
        if not args.power_entity:
            raise HilGateError(
                "missing --power-entity or HOME_ASSISTANT_POWER_ENTITY"
            )
        config = build_config(args)
        report = run_acceptance(config, args.repeats)
        encoded = json.dumps(report, ensure_ascii=False, indent=2)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(encoded + "\n", encoding="utf-8")
        print(encoded)
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError, RuntimeError) as error:
        print(f"HIL BLOCKED: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
