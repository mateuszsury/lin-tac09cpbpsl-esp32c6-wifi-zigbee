#!/usr/bin/env python3
"""Extract complete 9600-8N1 protocol frames and state transitions."""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re

from decode_rmt_uart import RMT_RE, RUN_RE, decode_runs


UART_RE = re.compile(
    r"UART,.*?direction=([^,]+),.*?gpio=(\d+),baud=9600,.*?hex=([0-9A-F]+)$"
)
PASSIVE_RE = re.compile(r"PASSIVE,.*?gpio=(\d+),len=(\d+),hex=([0-9A-F]+)$")
FRAME_RE = re.compile(
    r"FRAME,.*?direction=(panel_to_main|main_to_panel),.*?len=(\d+),hex=([0-9A-F]+)$"
)


def xor_bytes(payload: bytes) -> int:
    value = 0
    for byte in payload:
        value ^= byte
    return value


def complete_frames(payload: bytes) -> list[bytes]:
    """Extract complete TD frames using the verified length+4 rule."""
    frames: list[bytes] = []
    offset = 0
    while offset + 3 <= len(payload):
        start = payload.find(b"TD", offset)
        if start < 0 or start + 3 > len(payload):
            break
        expected = payload[start + 2] + 4
        if expected < 4 or start + expected > len(payload):
            offset = start + 1
            continue
        frames.append(payload[start : start + expected])
        offset = start + expected
    return frames


def frame_direction(payload: bytes) -> str | None:
    if len(payload) == 15 and payload.startswith(b"TD\x0b"):
        return "panel_to_main"
    if len(payload) == 22 and payload.startswith(b"TD\x12"):
        return "main_to_panel"
    return None


def read_records(path: pathlib.Path) -> list[dict]:
    records = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return records


def rmt_frames(records: list[dict], gpio: int = 7) -> list[tuple[str, bytes]]:
    """Decode legacy RMT logs and reconstruct the checksum byte they omitted."""
    bursts: dict[tuple[int, int], dict[int, list[tuple[int, int]]]] = collections.defaultdict(dict)
    times: dict[tuple[int, int], str] = {}
    for record in records:
        match = RMT_RE.search(record.get("text", ""))
        if not match or int(match.group(2)) != gpio:
            continue
        key = (int(match.group(1)), int(match.group(3)))
        bursts[key][int(match.group(5))] = [
            (int(level), int(duration)) for level, duration in RUN_RE.findall(match.group(6))
        ]
        times[key] = record.get("host_time", "")
    output = []
    for key in sorted(bursts):
        runs = [run for part in sorted(bursts[key]) for run in bursts[key][part]]
        payload, _ = decode_runs(runs, 1038.0)
        if len(payload) == 14 and payload.startswith(b"TD\x0b"):
            checksum = xor_bytes(payload) ^ 0x10
            output.append((times[key], payload + bytes([checksum])))
    return output


def uart_frames(records: list[dict]) -> dict[str, list[tuple[str, bytes]]]:
    output: dict[str, list[tuple[str, bytes]]] = collections.defaultdict(list)
    for record in records:
        match = UART_RE.search(record.get("text", ""))
        if not match:
            continue
        for payload in complete_frames(bytes.fromhex(match.group(3))):
            direction = frame_direction(payload)
            if direction:
                output[direction].append((record.get("host_time", ""), payload))
    return output


def framed_log_frames(records: list[dict]) -> dict[str, list[tuple[str, bytes]]]:
    """Read current PASSIVE/FRAME telemetry without lossy RMT decoding."""
    output: dict[str, list[tuple[str, bytes]]] = collections.defaultdict(list)
    for record in records:
        text = record.get("text", "")
        frame_match = FRAME_RE.search(text)
        if frame_match:
            raw = bytes.fromhex(frame_match.group(3))
            for payload in complete_frames(raw):
                direction = frame_direction(payload)
                if direction:
                    output[direction].append((record.get("host_time", ""), payload))
            continue

        passive_match = PASSIVE_RE.search(text)
        if not passive_match:
            continue
        raw = bytes.fromhex(passive_match.group(3))
        for payload in complete_frames(raw):
            direction = frame_direction(payload)
            if direction:
                output[direction].append((record.get("host_time", ""), payload))
    return output


def legacy_uart_frames(records: list[dict], gpio: int = 6) -> list[tuple[str, bytes]]:
    """Compatibility with the first UART capture format."""
    output = []
    for record in records:
        match = UART_RE.search(record.get("text", ""))
        if not match or int(match.group(2)) != gpio:
            continue
        for payload in complete_frames(bytes.fromhex(match.group(3))):
            if len(payload) == 22:
                output.append((record.get("host_time", ""), payload))
    return output


def print_summary(label: str, frames: list[tuple[str, bytes]]) -> None:
    counts = collections.Counter(payload.hex().upper() for _, payload in frames)
    print(f"[{label}] frames={len(frames)} unique={len(counts)}")
    for payload, count in counts.most_common(20):
        raw = bytes.fromhex(payload)
        total = sum(raw) & 0xFF
        xor = xor_bytes(raw)
        print(f"count={count:5d} sum={total:02X} xor={xor:02X} hex={payload}")

    print(f"[{label} transitions]")
    previous = b""
    for host_time, payload in frames:
        if payload != previous:
            print(f"{host_time} {payload.hex().upper()}")
            previous = payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=pathlib.Path)
    args = parser.parse_args()
    records = read_records(args.capture)
    current = framed_log_frames(records)
    uart = uart_frames(records)
    panel = current["panel_to_main"] or uart["panel_to_main"]
    main_frames = current["main_to_panel"] or uart["main_to_panel"]
    if not panel:
        panel = rmt_frames(records)
        if panel:
            print("[note] legacy RMT panel records: reconstructed final XOR byte")
    if not main_frames:
        main_frames = legacy_uart_frames(records)
    print_summary("panel_to_main", panel)
    print_summary("main_to_panel", main_frames)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
