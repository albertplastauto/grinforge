#!/usr/bin/env python3
# GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
# Copyright (c) 2026 albertplastauto
# SPDX-License-Identifier: MIT
#
# (This file was the one place the header pass missed: it looks for the copyright string, and
# this script contains it as a literal, so the check passed while the file itself had no header.)
"""Insert authorship/SPDX headers into GrinForge's own files.

Separate from the licence split already recorded in LICENSE and docs/third-party.md:
files derived from tromp/cuckoo are marked LicenseRef-Fair-Mining, everything else MIT.
third_party/** is never touched.

Idempotent: a file that already carries the copyright line is left alone.
Run from the repository root:  python tools/add_headers.py
"""
import os
import sys

COPYRIGHT = "Copyright (c) 2026 albertplastauto"

MIT_FILES = [
    "CMakeLists.txt",
    "src/host/main.cpp",
    "src/host/http_api.cpp",
    "src/host/http_api.hpp",
    "src/monitor/telemetry.cpp",
    "src/monitor/telemetry.hpp",
    "src/monitor/gpu_control.cpp",
    "src/monitor/gpu_control.hpp",
    "src/stratum/stratum_client.cpp",
    "src/stratum/stratum_client.hpp",
    "src/solver/lean_solver.hpp",
    "src/solver/cycle_finder.hpp",
    "tools/selftest.cpp",
    "tools/simple_ref.cpp",
    "tools/solver_bench.cpp",
    "tools/stratum_client_test.cpp",
    "tools/check_keys.py",
    "tools/check_siphash.py",
    "tools/ref_core.py",
    "tools/stratum_probe.py",
    "tools/pre_pow_analyze.py",
    "tools/gpu-clock-guard.ps1",
    "tools/add_headers.py",
    "gpu-lock-2500.bat",
    "gpu-unlock.bat",
    "install-gpu-clock-task.bat",
    "install-gpu-clock-guard.bat",
    "run-miner-forever.bat",
    "run-miner-forever.ps1",
    "stop-miner.bat",
]

FAIR_MINING_FILES = [
    "src/solver/lean_solver.cu",
    "src/solver/grin_params.hpp",
    "src/solver/grin_verify.hpp",
]

TITLE = "GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee."


def header_lines(path, fair_mining):
    if fair_mining:
        return [
            TITLE,
            f"Modifications {COPYRIGHT}",
            "Portions derived from tromp/cuckoo, Copyright (c) 2013-2020 John Tromp,",
            'distributed under "The FAIR MINING License" (see LICENSE).',
            "SPDX-License-Identifier: LicenseRef-Fair-Mining",
            "",
        ]
    return [
        TITLE,
        COPYRIGHT,
        "SPDX-License-Identifier: MIT",
        "",
    ]


def comment_prefix(path):
    # NOTE: CMake and shell-family files must never get C++ comments. The first run of
    # this script wrote "//" into CMakeLists.txt, which breaks the configure step.
    if path.endswith(".py") or path.endswith(".ps1") or os.path.basename(path) == "CMakeLists.txt":
        return "# "
    if path.endswith(".bat"):
        return "rem "
    if path.endswith(".cmake") or path.endswith(".txt"):
        return "# "
    return "// "


def insert(path, fair_mining):
    with open(path, "r", encoding="utf-8", newline="") as handle:
        text = handle.read()

    if COPYRIGHT in text:
        return "already"

    prefix = comment_prefix(path)
    block = "".join((prefix + line).rstrip() + "\n" for line in header_lines(path, fair_mining))

    lines = text.split("\n")
    insert_at = 0

    if path.endswith(".py") and lines and lines[0].startswith("#!"):
        insert_at = 1                      # keep the shebang first
    elif path.endswith(".bat"):
        # keep @echo off first, otherwise every rem below it gets echoed
        for index, line in enumerate(lines[:5]):
            if line.strip().lower().startswith("@echo off"):
                insert_at = index + 1
                break

    lines.insert(insert_at, block.rstrip("\n"))
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("\n".join(lines))
    return "inserted"


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    changed = 0
    for path, fair in [(p, False) for p in MIT_FILES] + [(p, True) for p in FAIR_MINING_FILES]:
        if not os.path.exists(path):
            print(f"  missing (skipped): {path}")
            continue
        result = insert(path, fair)
        if result == "inserted":
            changed += 1
            print(f"  header added: {path}")
    print(f"done: {changed} file(s) updated")


if __name__ == "__main__":
    sys.exit(main())
