#!/usr/bin/env python3
"""Upload a signed-by-build ESP-IDF application image over the local network."""

from __future__ import annotations

import argparse
import json
import pathlib
import time
import urllib.request

from http_utils import normalize_http_base_url, urlopen_http


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", type=pathlib.Path, default=pathlib.Path(".local/device.json"))
    parser.add_argument(
        "--base-url",
        help="Override the device URL from the local config (useful when mDNS is unavailable).",
    )
    parser.add_argument(
        "--firmware",
        type=pathlib.Path,
        default=pathlib.Path("build/esp-idf/klima_wifi.bin"),
    )
    parser.add_argument(
        "--allow-active",
        action="store_true",
        help="Allow an explicitly active MITM_NTS artifact (requires physical HIL sign-off).",
    )
    args = parser.parse_args()

    config = json.loads(args.device.read_text(encoding="utf-8"))
    manifest = args.firmware.parent / "release-manifest.json"
    if manifest.exists():
        metadata = json.loads(manifest.read_text(encoding="utf-8"))
        if metadata.get("active_control") and not args.allow_active:
            raise SystemExit(
                "refusing active MITM_NTS image; review physical HIL evidence and pass --allow-active"
            )
    firmware = args.firmware.read_bytes()
    base_url = normalize_http_base_url(args.base_url or config["base_url"])
    initial_status = None
    try:
        with urlopen_http(f"{base_url}/api/status", timeout=2.0) as response:
            initial_status = json.load(response)
    except Exception:
        pass
    request = urllib.request.Request(
        f"{base_url}/api/ota",
        data=firmware,
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "X-Klima-Token": config["token"],
        },
    )
    with urlopen_http(request, timeout=120.0) as response:
        result = json.load(response)
    if not result.get("ok"):
        raise SystemExit("device rejected OTA image")

    print(f"uploaded={len(firmware)} restarting=true")
    for _ in range(30):
        time.sleep(1.0)
        try:
            with urlopen_http(f"{base_url}/api/status", timeout=2.0) as response:
                status = json.load(response)
            rebooted = initial_status is None or (
                status.get("version") != initial_status.get("version")
                or status.get("ota_partition") != initial_status.get("ota_partition")
                or float(status.get("uptime_s", 0)) + 2.0
                < float(initial_status.get("uptime_s", 0))
            )
            if rebooted:
                print(json.dumps(status, ensure_ascii=False))
                return 0
        except Exception:
            continue
    raise SystemExit("device did not return after OTA")


if __name__ == "__main__":
    raise SystemExit(main())
