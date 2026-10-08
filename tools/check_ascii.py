#!/usr/bin/env python3
# GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
# Copyright (c) 2026 albertplastauto
# SPDX-License-Identifier: MIT
#
# Report where Cyrillic (or other non-Latin) text remains in the repository.
# The project is published internationally, so every tracked file must be English.
#
# Run from the repository root:  python tools/check_ascii.py

import os
import re
import subprocess
import sys

CYRILLIC = re.compile(r'[\u0400-\u04FF]')


def tracked_files():
    out = subprocess.run(['git', 'ls-files'], capture_output=True, text=True, check=True)
    return [line for line in out.stdout.splitlines() if line]


def main():
    total = 0
    for path in tracked_files():
        if not os.path.isfile(path):
            continue
        try:
            with open(path, 'r', encoding='utf-8') as handle:
                text = handle.read()
        except (UnicodeDecodeError, OSError):
            continue
        hits = len(CYRILLIC.findall(text))
        if hits:
            total += hits
            print(f"{hits:6d}  {path}")
    print(f"\ntotal Cyrillic characters in tracked files: {total}")
    return 0 if total == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
