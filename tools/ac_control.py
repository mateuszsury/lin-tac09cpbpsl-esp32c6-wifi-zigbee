#!/usr/bin/env python3
"""Read or control Klima WiFi through its authenticated local HTTP API."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time
import urllib.error
import urllib.request

from http_utils import normalize_http_base_url, urlopen_http


def load_device(path: pathlib.Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("device configuration must be a JSON object")
    return data


def build_control_body(args: argparse.Namespace) -> dict:
    body: dict[str, object] = {}
    if args.power is not None:
        body["power"] = args.power == "on"
    if args.setpoint is not None:
        if not 18 <= args.setpoint <= 32:
            raise ValueError("setpoint must be between 18 and 32 C")
        body["setpoint_c"] = args.setpoint
    if args.mode is not None:
        body["mode"] = args.mode
    if args.fan is not None:
        body["fan"] = args.fan
    if args.raw_mode_fan is not None:
        if args.mode is not None or args.fan is not None:
            raise ValueError("raw mode/fan code conflicts with named mode or fan")
        if not 0 <= args.raw_mode_fan <= 0xFF:
            raise ValueError("raw mode/fan code must fit one byte")
        body["raw_mode_fan_code"] = args.raw_mode_fan
    quiet = getattr(args, "quiet", None)
    units = getattr(args, "units", None)
    timer = getattr(args, "timer", None)
    if quiet is not None:
        body["quiet"] = quiet == "on"
    if units is not None:
        body["units_fahrenheit"] = units == "fahrenheit"
    if timer is not None:
        body["timer"] = timer == "on"
    if not body:
        raise ValueError("at least one control field is required")
    return body


def request_json(
    base_url: str,
    path: str,
    *,
    token: str | None = None,
    body: dict | None = None,
    timeout: float = 10.0,
) -> dict:
    base_url = normalize_http_base_url(base_url)
    encoded = json.dumps(body).encode("utf-8") if body is not None else None
    headers = {"Accept": "application/json"}
    if encoded is not None:
        headers["Content-Type"] = "application/json"
    if token:
        headers["X-Klima-Token"] = token
    request = urllib.request.Request(
        base_url.rstrip("/") + path,
        data=encoded,
        headers=headers,
        method="POST" if encoded is not None else "GET",
    )
    try:
        with urlopen_http(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {error.code}: {detail}") from error


def wait_for_command(base_url: str, sequence: int, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    last: dict = {}
    while time.monotonic() < deadline:
        last = request_json(base_url, "/api/ac-state")
        control = last.get("control", {})
        if control.get("sequence", 0) >= sequence and not control.get("pending", True):
            status = control.get("status")
            if status == "confirmed":
                return last
            if status in {"timed_out", "cancelled"}:
                raise RuntimeError(f"command {sequence} {status}")
        time.sleep(0.25)
    raise TimeoutError(f"command {sequence} was not confirmed in {timeout:g} seconds")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument(
        "--device",
        type=pathlib.Path,
        default=pathlib.Path(".local/device.json"),
        help="private JSON containing base_url and token",
    )
    result.add_argument("--base-url", help="override device base URL")
    subparsers = result.add_subparsers(dest="command", required=True)
    subparsers.add_parser("state", help="print decoded state without authentication")

    control = subparsers.add_parser("control", help="queue an authenticated MITM command")
    control.add_argument("--power", choices=("on", "off"))
    control.add_argument("--setpoint", type=int)
    control.add_argument("--mode", choices=("cool", "fan", "dry"))
    control.add_argument("--fan", choices=("low", "high"))
    control.add_argument("--raw-mode-fan", type=lambda value: int(value, 0))
    control.add_argument("--quiet", choices=("on", "off"))
    control.add_argument("--units", choices=("celsius", "fahrenheit"))
    control.add_argument("--timer", choices=("on", "off"))
    control.add_argument("--wait", type=float, default=10.0)
    control.add_argument("--dry-run", action="store_true")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        config = load_device(args.device) if args.device.exists() else {}
        if args.command == "control" and args.dry_run:
            print(json.dumps(build_control_body(args), indent=2))
            return 0
        base_url = args.base_url or config.get("base_url")
        if not base_url:
            raise ValueError("base_url is missing")

        if args.command == "state":
            print(json.dumps(request_json(base_url, "/api/ac-state"), indent=2))
            return 0

        body = build_control_body(args)
        token = config.get("token")
        if not token:
            raise ValueError("token is missing from private device configuration")
        accepted = request_json(base_url, "/api/control", token=token, body=body)
        sequence = int(accepted["sequence"])
        print(json.dumps(accepted, indent=2))
        if args.wait > 0:
            confirmed = wait_for_command(base_url, sequence, args.wait)
            print(json.dumps(confirmed, indent=2))
        return 0
    except (OSError, ValueError, RuntimeError, TimeoutError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
