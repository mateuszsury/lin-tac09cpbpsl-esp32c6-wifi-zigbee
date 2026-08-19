#!/usr/bin/env python3
"""Capture the ESP probe console without interpreting appliance data."""

from __future__ import annotations

import argparse
import datetime as dt
import pathlib
import sys
import time

import serial


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    serial_port = serial.Serial()
    serial_port.port = args.port
    serial_port.baudrate = args.baud
    serial_port.timeout = 0.1
    serial_port.dtr = False
    serial_port.rts = False
    serial_port.open()

    deadline = time.monotonic() + args.seconds
    buffer = bytearray()
    with args.output.open("w", encoding="utf-8", newline="\n") as stream:
        while time.monotonic() < deadline:
            chunk = serial_port.read(4096)
            if not chunk:
                continue
            buffer.extend(chunk)
            while b"\n" in buffer:
                raw_line, _, buffer = buffer.partition(b"\n")
                timestamp = dt.datetime.now(dt.timezone.utc).isoformat()
                line = raw_line.rstrip(b"\r").decode("utf-8", "backslashreplace")
                record = f"{timestamp}\t{line}"
                print(record, flush=True)
                stream.write(record + "\n")
                stream.flush()

        if buffer:
            timestamp = dt.datetime.now(dt.timezone.utc).isoformat()
            line = bytes(buffer).decode("utf-8", "backslashreplace")
            stream.write(f"{timestamp}\t{line}\n")

    serial_port.close()
    print(f"capture={args.output} seconds={args.seconds}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
