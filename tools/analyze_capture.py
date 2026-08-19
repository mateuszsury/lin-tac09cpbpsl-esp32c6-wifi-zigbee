#!/usr/bin/env python3
"""Summarize SCAN/UART/RMT records from a passive capture."""

from __future__ import annotations

import argparse
import collections
import pathlib
import re


UART_RE = re.compile(r"UART,.*?baud=(\d+),.*?len=(\d+),hex=([0-9A-F]*)")
RMT_RE = re.compile(r"RMT,.*?symbols=(\d+),data=(.*)$")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=pathlib.Path)
    args = parser.parse_args()

    uart_counts: collections.Counter[int] = collections.Counter()
    uart_bytes: collections.Counter[int] = collections.Counter()
    unique_payloads: dict[int, set[str]] = collections.defaultdict(set)
    rmt_bursts = 0
    rmt_symbols = 0

    for line in args.capture.read_text(encoding="utf-8", errors="replace").splitlines():
        uart_match = UART_RE.search(line)
        if uart_match:
            baud = int(uart_match.group(1))
            uart_counts[baud] += 1
            uart_bytes[baud] += int(uart_match.group(2))
            unique_payloads[baud].add(uart_match.group(3))
        rmt_match = RMT_RE.search(line)
        if rmt_match:
            rmt_bursts += 1
            rmt_symbols += int(rmt_match.group(1))

    print(f"RMT bursts: {rmt_bursts}; symbols: {rmt_symbols}")
    for baud in sorted(uart_counts):
        print(
            f"UART {baud}: records={uart_counts[baud]} "
            f"bytes={uart_bytes[baud]} unique_payloads={len(unique_payloads[baud])}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
