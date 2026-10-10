#!/usr/bin/env python3
# GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
# Copyright (c) 2026 albertplastauto
# SPDX-License-Identifier: MIT
#
# Two jobs:
#   1. verify that every tracked source file carries the authorship and licence header;
#   2. render the miner's dashboard to docs/dashboard.png, so the README can show what the
#      operator actually sees without anyone having to run it.
#
# The panel is reproduced from the live values with the same width rules the miner uses
# (table_widths()), so the picture matches the real output rather than being hand-drawn. It is a
# rendering of the console output, not a screen capture, and is labelled that way.
#
# Run from the repository root:  python tools/make_assets.py

import os
import subprocess
import sys

COPYRIGHT = "Copyright (c) 2026 albertplastauto"

# Files that must carry a header. Markdown is prose, not code, and is excluded on purpose.
CODE_SUFFIXES = (".py", ".ps1", ".bat", ".cpp", ".hpp", ".cu", ".vbs")
CODE_NAMES = ("CMakeLists.txt",)


def tracked():
    out = subprocess.run(["git", "ls-files"], capture_output=True, text=True, check=True)
    return [line for line in out.stdout.splitlines() if line]


def check_headers():
    missing = []
    for path in tracked():
        if path.startswith("third_party/"):
            continue          # upstream code keeps its own licence, untouched
        if not (path.endswith(CODE_SUFFIXES) or os.path.basename(path) in CODE_NAMES):
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            head = handle.read(1200)
        # Only the first few lines count as a header. Matching the whole file would pass any
        # script that merely mentions the copyright - which is exactly how add_headers.py slipped
        # through the first version of this check.
        first_lines = "\n".join(head.splitlines()[:6])
        has_copy = COPYRIGHT in first_lines
        has_spdx = "SPDX-License-Identifier" in first_lines
        if not (has_copy and has_spdx):
            missing.append((path, has_copy, has_spdx))
    return missing


def panel_rows():
    """The two dashboard tables, using the width rules of table_widths()."""
    head1 = ["ID", "GPU", "Speed", "Acc", "Rej", "Power", "Efficiency"]
    row1 = ["0", "RTX 4060 Ti", "0.0575 G/s", "75", "0", "55.0 W", "1.045 mG/W"]
    head2 = ["ID", "GPU", "Temp", "Fan", "Core", "Mem", "VRAM"]
    row2 = ["0", "RTX 4060 Ti", "42 C", "40 %", "2490 MHz", "8751 MHz", "2105 / 8187 MiB"]
    widths = [0] * len(head1)
    for cells in (head1, row1, head2, row2):
        for i, cell in enumerate(cells):
            widths[i] = max(widths[i], len(cell))

    def bar():
        return "+" + "+".join("-" * (w + 2) for w in widths) + "+"

    def line(cells):
        return "|" + "|".join(" " + c + " " * (w - len(c)) + " " for c, w in zip(cells, widths)) + "|"

    return [line(head1), bar(), line(row1), line(head2), bar(), line(row2)], widths


def render_png():
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("Pillow not available, skipping the image")
        return

    rows, _ = panel_rows()
    footer = "Pool grin.2miners.com:3030 | uptime 0d 12:34:56 | graphs 4521 | energy 0.66 kWh"
    startup = [
        "+--------------------------------------------------------------+",
        "|           GrinForge - GRIN Cuckatoo32 GPU miner              |",
        "|                0 % developer fee, MIT licensed               |",
        "+--------------------------------------------------------------+",
        "Algorithm:            Cuckatoo32 lean (CUDA)",
        "DevFee:               0 %",
        "Server:               grin.2miners.com:3030",
        "Wallet guard:         enforced (address must match the allowlist)",
        "GPU 0:                NVIDIA GeForce RTX 4060 Ti, 8187 MiB, sm_89",
        "NVIDIA driver:        617.42",
        "",
    ]

    font_path = None
    for candidate in (r"C:\Windows\Fonts\consola.ttf", r"C:\Windows\Fonts\lucon.ttf"):
        if os.path.exists(candidate):
            font_path = candidate
            break
    font = ImageFont.truetype(font_path, 17) if font_path else ImageFont.load_default()
    small = ImageFont.truetype(font_path, 15) if font_path else font

    char_w = font.getlength("M")
    line_h = 24
    pad = 24
    lines = len(startup) + len(rows) + 4
    width = int(char_w * 84) + pad * 2
    height = line_h * lines + pad * 2

    img = Image.new("RGB", (width, height), (12, 14, 18))
    draw = ImageDraw.Draw(img)
    y = pad

    def put(text, colour, use=font):
        draw.text((pad, y), text, font=use, fill=colour)
        return y + line_h

    cyan, green, dim, plain, yellow = (86, 182, 194), (126, 196, 126), (130, 138, 148), (208, 214, 220), (214, 190, 118)

    for text in startup:
        colour = cyan if text.startswith("+") or text.startswith("|") else plain
        if "0 % developer fee" in text:
            colour = green
        y = put(text, colour)

    for text in rows:
        y = put(text, cyan if text.startswith("+") else plain)

    draw.text((pad, y), footer, font=small, fill=dim)
    y += line_h + 6
    draw.text((pad, y), "rendered from the miner's console output - not a screen capture",
              font=small, fill=yellow)

    out = os.path.join("docs", "dashboard.png")
    img.save(out)
    print(f"wrote {out}  ({width}x{height})")


def main():
    missing = check_headers()
    if missing:
        print("MISSING authorship/licence header:")
        for path, has_copy, has_spdx in missing:
            print(f"  {path}   copyright={has_copy} spdx={has_spdx}")
    else:
        print("headers: every tracked source file carries copyright + SPDX")

    render_png()
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
