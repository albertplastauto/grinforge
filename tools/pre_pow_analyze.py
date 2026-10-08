#!/usr/bin/env python3
# GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
# Copyright (c) 2026 albertplastauto
# SPDX-License-Identifier: MIT
#
"""Decode a GRIN stratum pre_pow blob and figure out its real layout.

Usage: python pre_pow_analyze.py <hex> [<hex2> ...]
"""
import sys

JOBS = {
    "h4053495": "000500000000003dd9f7000000006ac7b1fd000478d9f83f75fea4ccc29dfb3e8642afa8f2871bd31e26aabcb3967ecf78e655e9a63965678058d8fefe0570762e420c619ac2bfebc41da92722d2ad60f593b858d57787427d21550fdf620878cd8165008eefa1ecd6d7be3af5fbff2bc23f8096e0f166a140b323d3fc083dd306c3bdf6778da1320349316cf722b84bf9305fbd0c64203694a143802f5cebdb3d0ee5083bfae729b88f5b6bbd7c8baa46e842f576580454e46da44c3c56fa4f6ce841479c57063abef59e542346680b5d5b00000000011cbc960000000000c2c7c900086dbdad185dfc00000000",
    "h4053496": "000500000000003dd9f8000000006ac7b2070001692446808dd7227dc13cc378d48777795725adf2b92073d3406fb4e04f2feb9f62a3e894e6a700847df2357dffab3e2d3a8b03a24303bac49ab79356ceb92b49f10e97d817f4ff365af5914efc3b5ce9d5fa0652709cde35a0b3b8bb719c1f1e97a9a9e13e7a6969594e5fdb79bdf5f9104e2339070201ececed9e826c0329fa7b7ca6425d3b8376ef43762aff8f7e2261d12e11de6c5870505eda1a6e0d42f576580454e46da44c3c56fa4f6ce841479c57063abef59e542346680b5d5b00000000011cbc980000000000c2c7cc00086dbdb2a6e7fc00000000",
}

# GRIN block header field layout (little-endian, bincode/serde)
FIELDS = [
    ("version", 2, "u16"),
    ("height", 8, "u64"),
    ("previous", 32, "hash"),
    ("prev_root", 32, "hash"),
    ("timestamp", 8, "u64"),
    ("output_root", 32, "hash"),
    ("output_mmr_size", 8, "u64"),
    ("nonce", 8, "u64"),
    ("edge_bits", 1, "u8"),
    ("proof_vec_len", 1, "compact-u8"),
    ("proof_nonces", 168, "42 x u32"),
]


def dump(name, hexstr):
    raw = bytes.fromhex(hexstr)
    print(f"=== {name}: {len(hexstr)} hex chars = {len(raw)} bytes")

    # 1) Try the canonical GRIN header layout.
    off = 0
    print("  -- canonical GRIN header parse --")
    for fname, size, ftype in FIELDS:
        if off + size > len(raw):
            print(f"     {fname:16s} @{off:4d}  <TRUNCATED, only {len(raw)-off} bytes left>")
            break
        chunk = raw[off:off + size]
        if ftype == "u16":
            val = int.from_bytes(chunk, "little")
        elif ftype == "u64":
            val = int.from_bytes(chunk, "little")
        elif ftype == "u8":
            val = chunk[0]
        else:
            val = None
        shown = chunk.hex() if ftype in ("hash", "compact-u8", "42 x u32") else val
        print(f"     {fname:16s} @{off:4d} len={size:3d} {ftype:10s} = {shown}")
        off += size
    print(f"     bytes consumed: {off} of {len(raw)}")

    # 2) Positional diff against the other blob, to find fixed vs varying regions.
    for other_name, other_hex in JOBS.items():
        if other_hex == hexstr:
            continue
        other = bytes.fromhex(other_hex)
        if len(other) != len(raw):
            continue
        diffs = [i for i in range(len(raw)) if raw[i] != other[i]]
        runs = []
        for i in diffs:
            if runs and i == runs[-1][-1] + 1:
                runs[-1].append(i)
            else:
                runs.append([i])
        print(f"  -- differing byte ranges vs {other_name} ({len(diffs)} bytes differ) --")
        for run in runs:
            print(f"     [{run[0]:3d}..{run[-1]:3d}] len={len(run):3d}  {raw[run[0]:run[-1]+1].hex()}")

    # 3) Tail: 4-byte nonce placeholder hypothesis.
    print(f"  -- last 12 bytes: {raw[-12:].hex()}  (ends with {raw[-4:].hex()})")
    print()


def main():
    targets = sys.argv[1:] or list(JOBS.values())
    for i, hexstr in enumerate(targets):
        name = next((k for k, v in JOBS.items() if v == hexstr), f"job{i}")
        dump(name, hexstr)


if __name__ == "__main__":
    main()
