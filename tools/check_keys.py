#!/usr/bin/env python3
# GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
# Copyright (c) 2026 albertplastauto
# SPDX-License-Identifier: MIT
#
"""Independent cross-check of the GRIN header -> siphash key derivation.

Uses Python's hashlib (a completely separate BLAKE2b implementation) to compute
BLAKE2b-512(pre_pow || be64(nonce)) and compare it against the digest printed by
the C++ tool. This validates the header layout and the hash together, without
trusting any of our own C++ code.

Usage:
    python check_keys.py --dump "<476 hex>" <nonce>
    python check_keys.py --check "<476 hex>" <nonce> <expected 64 hex chars>
"""
import argparse
import binascii
import hashlib
import sys

PRE_POW_SIZE = 238
NONCE_SIZE = 8


def derive(pre_pow_hex: str, nonce: int) -> bytes:
    pre = binascii.unhexlify(pre_pow_hex)
    if len(pre) != PRE_POW_SIZE:
        raise SystemExit(f"pre_pow must be {PRE_POW_SIZE} bytes, got {len(pre)}")
    header = pre + nonce.to_bytes(NONCE_SIZE, "big")
    digest = hashlib.blake2b(header, digest_size=32).digest()
    return digest


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pre_pow")
    ap.add_argument("nonce", type=int)
    ap.add_argument("--check", metavar="EXPECTED_HEX")
    args = ap.parse_args()

    digest = derive(args.pre_pow, args.nonce)
    print(f"nonce={args.nonce}")
    print(f"digest={digest.hex()}")

    if args.check:
        expected = bytes.fromhex(args.check.strip())
        if digest == expected:
            print("MATCH: C++ and Python BLAKE2b agree")
            return 0
        print("MISMATCH")
        print(f"  python   = {digest.hex()}")
        print(f"  c++      = {expected.hex()}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
