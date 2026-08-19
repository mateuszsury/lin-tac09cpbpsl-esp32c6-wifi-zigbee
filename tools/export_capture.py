#!/usr/bin/env python3
"""Download and validate the ESP32's persistent transition archive."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import urllib.parse
import urllib.request

from http_utils import normalize_http_base_url, urlopen_http


def parse_ndjson(payload: str) -> list[dict]:
    records: list[dict] = []
    for line_number, line in enumerate(payload.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError(f"invalid JSON on line {line_number}: {error}") from error
        if not isinstance(record, dict):
            raise ValueError(f"line {line_number} is not a JSON object")
        records.append(record)
    return records


def validate_records(records: list[dict], after: int = 0) -> None:
    previous = after
    for index, record in enumerate(records, start=1):
        sequence = record.get("sequence")
        timestamp = record.get("device_time_us")
        text = record.get("text")
        if not isinstance(sequence, int) or isinstance(sequence, bool) or sequence <= previous:
            raise ValueError(f"record {index} has non-monotonic sequence")
        if not isinstance(timestamp, int) or isinstance(timestamp, bool) or timestamp < 0:
            raise ValueError(f"record {index} has invalid device_time_us")
        if not isinstance(text, str) or not text:
            raise ValueError(f"record {index} has invalid text")
        previous = sequence


def request_json(url: str, token: str | None = None) -> dict:
    headers = {"Cache-Control": "no-store"}
    if token:
        headers["X-Klima-Token"] = token
    request = urllib.request.Request(url, headers=headers)
    with urlopen_http(request, timeout=10.0) as response:
        value = json.load(response)
    if not isinstance(value, dict):
        raise ValueError("device returned a non-object JSON response")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", type=pathlib.Path, default=pathlib.Path(".local/device.json"))
    parser.add_argument("--base-url")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--after", type=int, default=0)
    args = parser.parse_args()
    if not 0 <= args.after <= 0xFFFFFFFF:
        parser.error("--after must be in range 0..4294967295")

    config = json.loads(args.device.read_text(encoding="utf-8"))
    base_url = normalize_http_base_url(args.base_url or config["base_url"])
    token = config["token"]
    status = request_json(f"{base_url}/api/capture/status")
    query = urllib.parse.urlencode({"after": args.after})
    request = urllib.request.Request(
        f"{base_url}/api/capture/export?{query}",
        headers={"Cache-Control": "no-store", "X-Klima-Token": token},
    )
    with urlopen_http(request, timeout=30.0) as response:
        payload = response.read().decode("utf-8")
    records = parse_ndjson(payload)
    validate_records(records, args.after)

    downloaded_at = dt.datetime.now(dt.timezone.utc).isoformat()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as stream:
        for record in records:
            record["host_time"] = downloaded_at
            stream.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")

    first = records[0]["sequence"] if records else None
    last = records[-1]["sequence"] if records else None
    print(
        json.dumps(
            {
                "ok": True,
                "records": len(records),
                "first_sequence": first,
                "last_sequence": last,
                "device_latest_sequence": status.get("latest_sequence"),
                "device_dropped_records": status.get("dropped_records"),
                "device_write_errors": status.get("write_errors"),
                "output": str(args.output),
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
