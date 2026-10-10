#!/usr/bin/env python3
# GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
# Copyright (c) 2026 albertplastauto
# SPDX-License-Identifier: MIT
#
# Diagnose the pool connection churn the operator noticed in the log: how often the peer closes
# the connection, what the miner sends in between, and what the last lines before a close are.
#
# Run from the repository root:  python tools/analyse_connection.py [logfile]

import re
import sys

TIME = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.\d+\]")


def seconds(line):
    match = TIME.search(line)
    if not match:
        return None
    h, m, s = (int(part) for part in match.groups())
    return h * 3600 + m * 60 + s


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "logs/visible-2026-10-10.log"
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        lines = [line.rstrip("\n") for line in handle]

    closes = [line for line in lines if "closed by the peer" in line]
    connects = [line for line in lines if "connected to" in line]
    keepalives = [line for line in lines if "keepalive" in line]

    print(f"file: {path}")
    print(f"lines: {len(lines)}")
    print(f"connections opened : {len(connects)}")
    print(f"connections closed : {len(closes)}")
    print(f"keepalive messages : {len(keepalives)}")

    stamps = [seconds(line) for line in closes]
    stamps = [s for s in stamps if s is not None]
    if len(stamps) > 2:
        deltas = [b - a for a, b in zip(stamps, stamps[1:])]
        deltas = [d if d >= 0 else d + 86400 for d in deltas]
        deltas_sorted = sorted(deltas)
        print(f"close-to-close interval: min {min(deltas)}s  median "
              f"{deltas_sorted[len(deltas_sorted) // 2]}s  max {max(deltas)}s")
        # A tight cluster around one value means a timer, not a fault.
        buckets = {}
        for d in deltas:
            buckets[d // 10 * 10] = buckets.get(d // 10 * 10, 0) + 1
        print("interval histogram (10 s buckets):",
              "  ".join(f"{k}-{k + 9}s:{v}" for k, v in sorted(buckets.items())))

    # What the miner and the pool did around the most recent close.
    print("\n--- last 14 lines up to and including the most recent close ---")
    index = max(i for i, line in enumerate(lines) if "closed by the peer" in line)
    for line in lines[max(0, index - 13):index + 1]:
        print("   " + line.strip()[:150])

    # Does the miner ever send anything between jobs other than keepalive?
    sends = [line for line in lines if "send:" in line or "->" in line]
    print(f"\nlines that look like outbound messages: {len(sends)}")


if __name__ == "__main__":
    main()
