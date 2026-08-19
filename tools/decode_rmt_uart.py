#!/usr/bin/env python3
"""Decode 8N1 UART bytes from Klima WiFi RMT JSONL captures."""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re


RMT_RE = re.compile(
    r"RMT,t_us=(\d+),.*?gpio=(\d+),burst=(\d+),symbols=(\d+),part=(\d+),data=(.*)$"
)
RUN_RE = re.compile(r"([01]):(\d+)")


def decode_runs(runs: list[tuple[int, int]], ticks_per_bit: float) -> tuple[bytes, int]:
    bits: list[int] = []
    timing_errors = 0
    for level, duration in runs:
        if duration == 0:
            continue
        count = max(1, round(duration / ticks_per_bit))
        if abs(duration - count * ticks_per_bit) > ticks_per_bit * 0.25:
            timing_errors += 1
        bits.extend([level] * count)

    result = bytearray()
    index = 0
    while index + 9 < len(bits):
        if bits[index] != 0:
            index += 1
            continue
        stop = bits[index + 9]
        if stop != 1:
            index += 1
            continue
        value = sum(bits[index + 1 + bit] << bit for bit in range(8))
        result.append(value)
        index += 10
    return bytes(result), timing_errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--ticks-per-bit", type=float, default=1038.0)
    parser.add_argument("--gpio", type=int, default=7)
    args = parser.parse_args()

    bursts: dict[tuple[int, int], dict[int, list[tuple[int, int]]]] = collections.defaultdict(dict)
    host_times: dict[tuple[int, int], str] = {}
    for raw_line in args.capture.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            record = json.loads(raw_line)
        except json.JSONDecodeError:
            continue
        text = record.get("text", "")
        match = RMT_RE.search(text)
        if not match or int(match.group(2)) != args.gpio:
            continue
        key = (int(match.group(1)), int(match.group(3)))
        bursts[key][int(match.group(5))] = [
            (int(level), int(duration))
            for level, duration in RUN_RE.findall(match.group(6))
        ]
        host_times[key] = record.get("host_time", "")

    payload_counts: collections.Counter[str] = collections.Counter()
    decoded_rows: list[tuple[str, int, bytes, int]] = []
    for key in sorted(bursts):
        parts = bursts[key]
        runs = [run for part in sorted(parts) for run in parts[part]]
        payload, timing_errors = decode_runs(runs, args.ticks_per_bit)
        if payload:
            payload_counts[payload.hex().upper()] += 1
            decoded_rows.append((host_times[key], key[1], payload, timing_errors))

    print(f"decoded_bursts={len(decoded_rows)} unique_payloads={len(payload_counts)}")
    for payload, count in payload_counts.most_common(20):
        print(f"count={count:5d} len={len(payload)//2:3d} hex={payload}")
    print("-- latest --")
    for host_time, burst, payload, errors in decoded_rows[-20:]:
        print(f"{host_time} burst={burst} len={len(payload)} timing_errors={errors} hex={payload.hex().upper()}")
    print("-- transitions (complete frames) --")
    previous = b""
    transitions: list[tuple[str, int, bytes]] = []
    for host_time, burst, payload, _ in decoded_rows:
        if len(payload) != 14 or payload == previous:
            continue
        transitions.append((host_time, burst, payload))
        previous = payload
    for host_time, burst, payload in transitions[-100:]:
        print(f"{host_time} burst={burst} hex={payload.hex().upper()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
