#!/usr/bin/env python3
# GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
# Copyright (c) 2026 albertplastauto
# SPDX-License-Identifier: MIT
#
"""Probe the GRIN stratum protocol spoken by a pool.

Read-only diagnostic: connects, performs the standard GRIN stratum handshake
(login / getjobtemplate), then logs every line the server sends for N seconds.
No shares are submitted.

Usage: python stratum_probe.py [host:port] [seconds]
"""
import json
import socket
import sys
import time

HOSTPORT = sys.argv[1] if len(sys.argv) > 1 else "grin.2miners.com:3030"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0
USER = "grin1replacewithyourownaddressreplacewithyourownaddressreplacewithyourow.PROBE"

host, _, port = HOSTPORT.partition(":")
port = int(port or 3030)


def send(sock, obj):
    line = json.dumps(obj, separators=(",", ":")) + "\n"
    print(f">>> {line.rstrip()}", flush=True)
    sock.sendall(line.encode())


def main():
    print(f"# connecting to {host}:{port}", flush=True)
    s = socket.create_connection((host, port), timeout=15)
    s.settimeout(1.0)

    # Standard GRIN stratum handshake (as spoken by grin-miner).
    send(s, {"id": "1", "jsonrpc": "2.0", "method": "login",
             "params": {"agent": "grin-miner/0.6.0", "login": USER, "pass": "x"}})
    send(s, {"id": "2", "jsonrpc": "2.0", "method": "getjobtemplate", "params": []})

    deadline = time.time() + SECONDS
    buf = b""
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        except OSError as exc:
            print(f"# recv error: {exc}", flush=True)
            break
        if not chunk:
            print("# server closed connection", flush=True)
            break
        buf += chunk
        while b"\n" in buf:
            line, _, buf = buf.partition(b"\n")
            line = line.strip()
            if not line:
                continue
            text = line.decode("utf-8", "replace")
            print(f"<<< {text}", flush=True)
            try:
                obj = json.loads(text)
            except ValueError:
                continue
            # Print a readable schema of job notifications: which keys, and
            # the byte length of each hex blob (header layout matters most).
            params = obj.get("params")
            if isinstance(params, dict):
                schema = {}
                for key, value in params.items():
                    if isinstance(value, str) and value and all(
                            c in "0123456789abcdefABCDEF" for c in value):
                        schema[key] = f"str hex, {len(value)} chars = {len(value) // 2} bytes"
                    else:
                        schema[key] = value
                print(f"    # schema: {json.dumps(schema, ensure_ascii=False)}", flush=True)
    s.close()
    print("# done", flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:  # noqa: BLE001 - diagnostic script
        print(f"# FAILED: {type(exc).__name__}: {exc}", flush=True)
        sys.exit(1)
