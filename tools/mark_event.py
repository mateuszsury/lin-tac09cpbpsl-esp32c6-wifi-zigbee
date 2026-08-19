#!/usr/bin/env python3
"""Add an authenticated timestamped label to the device telemetry stream."""

from __future__ import annotations

import argparse
import json
import pathlib
import urllib.request

from http_utils import normalize_http_base_url, urlopen_http


def validate_label(label: str) -> str:
    if not 1 <= len(label.encode("utf-8")) <= 95:
        raise ValueError("label must contain 1-95 UTF-8 bytes")
    if any(ord(char) < 0x20 or char in ',\\"' for char in label):
        raise ValueError('label cannot contain control characters, comma, backslash, or quote')
    return label


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("label", type=validate_label)
    parser.add_argument("--device", type=pathlib.Path, default=pathlib.Path(".local/device.json"))
    parser.add_argument("--base-url")
    args = parser.parse_args()

    config = json.loads(args.device.read_text(encoding="utf-8"))
    base_url = normalize_http_base_url(args.base_url or config["base_url"])
    body = json.dumps({"label": args.label}, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        f"{base_url}/api/marker",
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "X-Klima-Token": config["token"],
        },
    )
    with urlopen_http(request, timeout=5.0) as response:
        print(json.dumps(json.load(response), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
