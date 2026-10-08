#!/usr/bin/env python3
# GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
# Copyright (c) 2026 albertplastauto
# SPDX-License-Identifier: MIT
#
"""Independent Python implementation of the siphash variant used by Cuckoo Cycle.

Purpose: cross-check the C++ `siphash_keys` / `sipnode` against a completely
separate implementation. A rotation-order mistake here would produce a miner that
runs fine and never finds a single share, so it must be pinned down independently.

The variant: SipHash-2-4 generalized to four 64-bit keys (k0..k3 initialise
v0..v3 directly), with the standard rotation set 13/16/21/17/32, and the standard
finalization v0 ^= nonce, v2 ^= 0xff, 4 rounds.

Usage:
    python check_siphash.py --selftest
    python check_siphash.py --pre-pow <476 hex> --nonce <n> --edge <e>
"""
import argparse
import binascii
import hashlib
import sys

MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1


def rotl(x, b):
    return ((x << b) | (x >> (64 - b))) & MASK64


def sip_round(v):
    v0, v1, v2, v3 = v
    v0 = (v0 + v1) & MASK64
    v2 = (v2 + v3) & MASK64
    v1 = rotl(v1, 13)
    v3 = rotl(v3, 16)
    v1 ^= v0
    v3 ^= v2
    v0 = rotl(v0, 32)
    v2 = (v2 + v1) & MASK64
    v0 = (v0 + v3) & MASK64
    v1 = rotl(v1, 17)
    v3 = rotl(v3, 21)
    v1 ^= v2
    v3 ^= v0
    v2 = rotl(v2, 32)
    return v0, v1, v2, v3


def siphash24(keys, nonce):
    """keys = (k0, k1, k2, k3); returns the xor of the four lanes."""
    v = list(keys)
    v[3] ^= nonce
    v = list(sip_round(v))
    v = list(sip_round(v))
    v[0] ^= nonce
    v[2] ^= 0xFF
    for _ in range(4):
        v = list(sip_round(v))
    return (v[0] ^ v[1] ^ v[2] ^ v[3]) & MASK64


def keys_from_digest(digest: bytes):
    if len(digest) != 32:
        raise SystemExit("digest must be 32 bytes")
    return tuple(int.from_bytes(digest[i * 8:(i + 1) * 8], "little") for i in range(4))


def derive_keys(pre_pow: bytes, nonce: int):
    header = pre_pow + nonce.to_bytes(8, "big")
    return keys_from_digest(hashlib.blake2b(header, digest_size=32).digest())


def selftest() -> int:
    failures = 0

    # 1. The synthetic header used by tools/selftest.cpp.
    pre = bytes(i & 0xFF for i in range(238))
    keys = derive_keys(pre, 0x0102030405060708)
    print(f"synthetic keys k0={keys[0]:016x} k1={keys[1]:016x} "
          f"k2={keys[2]:016x} k3={keys[3]:016x}")
    u = siphash24(keys, (0 << 1) | 0) & MASK32
    v = siphash24(keys, (0 << 1) | 1) & MASK32
    print(f"synthetic sipnode(edge=0) u={u} v={v}")
    if (u, v) != (1647366607, 56316686):
        print("  FAIL: does not match the C++ selftest output (1647366607, 56316686)")
        failures += 1
    else:
        print("  OK: matches the C++ selftest output")
    return failures


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--pre-pow")
    ap.add_argument("--nonce", type=int, default=0)
    ap.add_argument("--edge", type=int, default=0)
    args = ap.parse_args()

    if args.selftest:
        return 1 if selftest() else 0

    if not args.pre_pow:
        ap.error("--pre-pow is required unless --selftest is used")

    pre = binascii.unhexlify(args.pre_pow)
    keys = derive_keys(pre, args.nonce)
    print(f"nonce={args.nonce}")
    print(f"keys k0={keys[0]:016x} k1={keys[1]:016x} k2={keys[2]:016x} k3={keys[3]:016x}")
    e = args.edge
    print(f"sipnode(edge={e}) u={siphash24(keys, 2*e+0) & MASK32} "
          f"v={siphash24(keys, 2*e+1) & MASK32}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
