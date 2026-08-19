#!/usr/bin/env python3
"""Continuously save the device's Wi-Fi telemetry stream."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import time
import urllib.parse
import urllib.request

from http_utils import normalize_http_base_url, urlopen_http


def request_json(url: str, timeout: float = 3.0) -> dict:
    request = urllib.request.Request(url, headers={"Cache-Control": "no-store"})
    with urlopen_http(request, timeout=timeout) as response:
        return json.load(response)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", type=pathlib.Path, default=pathlib.Path(".local/device.json"))
    parser.add_argument(
        "--base-url",
        help="Override the device URL from the local config (useful when mDNS is unavailable).",
    )
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--seconds", type=float, default=300.0)
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Write only to the capture file (useful for a detached collector).",
    )
    args = parser.parse_args()

    config = json.loads(args.device.read_text(encoding="utf-8"))
    base_url = normalize_http_base_url(args.base_url or config["base_url"])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    after = 0
    deadline = time.monotonic() + args.seconds

    with args.output.open("w", encoding="utf-8", newline="\n") as stream:
        while time.monotonic() < deadline:
            try:
                payload = request_json(
                    f"{base_url}/api/logs?{urllib.parse.urlencode({'after': after})}"
                )
                for entry in payload.get("entries", []):
                    sequence = int(entry["seq"])
                    if after and sequence > after + 1:
                        gap = {
                            "host_time": dt.datetime.now(dt.timezone.utc).isoformat(),
                            "collector_gap": sequence - after - 1,
                            "after": after,
                            "resumed_at": sequence,
                        }
                        gap_line = json.dumps(gap, ensure_ascii=False, separators=(",", ":"))
                        if not args.quiet:
                            print(gap_line, flush=True)
                        stream.write(gap_line + "\n")
                    after = max(after, sequence)
                    record = {
                        "host_time": dt.datetime.now(dt.timezone.utc).isoformat(),
                        "sequence": sequence,
                        "text": entry["text"],
                    }
                    line = json.dumps(record, ensure_ascii=False, separators=(",", ":"))
                    if not args.quiet:
                        print(line, flush=True)
                    stream.write(line + "\n")
                    stream.flush()
            except Exception as error:  # keep collecting across Wi-Fi reconnects
                record = {
                    "host_time": dt.datetime.now(dt.timezone.utc).isoformat(),
                    "collector_error": str(error),
                }
                stream.write(json.dumps(record, ensure_ascii=False) + "\n")
                stream.flush()
                time.sleep(1.0)
            else:
                time.sleep(0.2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
