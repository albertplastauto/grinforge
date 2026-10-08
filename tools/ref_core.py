#!/usr/bin/env python3
"""Fully independent reference for the Cuckoo Cuckatoo pipeline (CPU, Python).

Written from the definitions, sharing no code with the C++/CUDA solver apart from
the already-verified siphash and BLAKE2b (both re-implemented here with hashlib).

It answers two questions that the GPU solver cannot answer about itself:

  1. What is the true 2-core of the graph for a given header/nonce? (Removing
     degree-1 nodes is trivially correct, so this is a trustworthy reference for
     the GPU edge-trimming.)
  2. Which cycles actually exist in that core, and of what length? Enumeration is
     done on the core only - every cycle lies in the 2-core - with a plain,
     obviously-correct brute force.

Usage:
    python ref_core.py <476-hex pre_pow> <nonce> [max_cycle_len]

EDGEBITS is fixed at 20 so the graph fits in memory and runs in seconds.
"""
import binascii
import hashlib
import sys
from collections import deque

EDGEBITS = 20
PROOFSIZE = 6
MASK = (1 << 64) - 1
NODE_MASK = (1 << EDGEBITS) - 1
NEDGES = 1 << EDGEBITS
PRE_POW_SIZE = 238


def rotl(x, b):
    return ((x << b) | (x >> (64 - b))) & MASK


def sip_round(v):
    v0, v1, v2, v3 = v
    v0 = (v0 + v1) & MASK
    v2 = (v2 + v3) & MASK
    v1 = rotl(v1, 13)
    v3 = rotl(v3, 16)
    v1 ^= v0
    v3 ^= v2
    v0 = rotl(v0, 32)
    v2 = (v2 + v1) & MASK
    v0 = (v0 + v3) & MASK
    v1 = rotl(v1, 17)
    v3 = rotl(v3, 21)
    v1 ^= v2
    v3 ^= v0
    v2 = rotl(v2, 32)
    return v0, v1, v2, v3


def siphash24(keys, nonce):
    v = list(keys)
    v[3] ^= nonce
    v = list(sip_round(v))
    v = list(sip_round(v))
    v[0] ^= nonce
    v[2] ^= 0xFF
    for _ in range(4):
        v = list(sip_round(v))
    return (v[0] ^ v[1] ^ v[2] ^ v[3]) & MASK


def keys_for(pre_pow: bytes, nonce: int):
    header = pre_pow + nonce.to_bytes(8, "big")
    digest = hashlib.blake2b(header, digest_size=32).digest()
    return tuple(int.from_bytes(digest[i * 8:(i + 1) * 8], "little") for i in range(4))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    pre_pow = binascii.unhexlify(sys.argv[1])
    if len(pre_pow) != PRE_POW_SIZE:
        print(f"pre_pow must be {PRE_POW_SIZE} bytes, got {len(pre_pow)}")
        return 2
    nonce = int(sys.argv[2])
    max_len = int(sys.argv[3]) if len(sys.argv) > 3 else 12

    keys = keys_for(pre_pow, nonce)
    print(f"EDGEBITS={EDGEBITS} PROOFSIZE={PROOFSIZE} nonce={nonce}")

    # --- build the graph ----------------------------------------------------
    n_part = NEDGES           # nodes per partition
    total_nodes = 2 * n_part
    eu = [0] * NEDGES         # partition-0 endpoint of each edge
    ev = [0] * NEDGES         # partition-1 endpoint (offset by n_part)
    for e in range(NEDGES):
        eu[e] = siphash24(keys, 2 * e + 0) & NODE_MASK
        ev[e] = n_part + (siphash24(keys, 2 * e + 1) & NODE_MASK)
    print(f"edges: {NEDGES}")

    # --- 2-core: repeatedly drop degree-1 nodes -----------------------------
    deg = [0] * total_nodes
    for i in range(NEDGES):
        deg[eu[i]] += 1
        deg[ev[i]] += 1

    # CSR adjacency over ALL edges (needed to decrement neighbours)
    start = [0] * (total_nodes + 1)
    for i in range(NEDGES):
        start[eu[i] + 1] += 1
        start[ev[i] + 1] += 1
    for n in range(total_nodes):
        start[n + 1] += start[n]
    cursor = start[:total_nodes]
    adj_edge = [0] * (2 * NEDGES)
    for i in range(NEDGES):
        adj_edge[cursor[eu[i]]] = i
        cursor[eu[i]] += 1
        adj_edge[cursor[ev[i]]] = i
        cursor[ev[i]] += 1

    removed_edge = bytearray(NEDGES)
    node_deg = [start[n + 1] - start[n] for n in range(total_nodes)]
    q = deque(n for n in range(total_nodes) if node_deg[n] < 2)
    in_core = bytearray([1]) * total_nodes
    while q:
        n = q.popleft()
        if node_deg[n] >= 2 or not in_core[n]:
            continue
        in_core[n] = 0
        for a in range(start[n], start[n + 1]):
            e = adj_edge[a]
            if removed_edge[e]:
                continue
            removed_edge[e] = 1
            other = ev[e] if eu[e] == n else eu[e]
            node_deg[other] -= 1
            if node_deg[other] < 2:
                q.append(other)

    core_edges = [i for i in range(NEDGES) if not removed_edge[i]]
    core_nodes = [n for n in range(total_nodes) if in_core[n]]
    print(f"2-core: {len(core_nodes)} nodes, {len(core_edges)} edges")
    if core_nodes:
        print(f"2-core min degree: {min(node_deg[n] for n in core_nodes)}")

    # --- enumerate cycles of length <= max_len on the core -------------------
    # Adjacency restricted to core edges.
    cstart = [0] * (total_nodes + 1)
    for e in core_edges:
        cstart[eu[e] + 1] += 1
        cstart[ev[e] + 1] += 1
    for n in range(total_nodes):
        cstart[n + 1] += cstart[n]
    ccursor = cstart[:total_nodes]
    cadj = [0] * (2 * len(core_edges))
    for e in core_edges:
        cadj[ccursor[eu[e]]] = e
        ccursor[eu[e]] += 1
        cadj[ccursor[ev[e]]] = e
        ccursor[ev[e]] += 1

    found = {}
    for length in range(2, max_len + 1, 2):
        seen = set()
        count = 0
        for e0 in core_edges:
            a, b = eu[e0], ev[e0]
            # DFS from b back to a using `length` edges total
            stack = [(b, (e0,), frozenset((a, b)))]
            while stack:
                node, path, visited = stack.pop()
                if len(path) == length:
                    # last node must be a and edge closes onto it: our walk already
                    # alternates, so check the returning edge separately below
                    continue
                for idx in range(cstart[node], cstart[node + 1]):
                    e = cadj[idx]
                    if e in path:
                        continue
                    other = ev[e] if eu[e] == node else eu[e]
                    if len(path) + 1 == length:
                        if other == a:
                            key = tuple(sorted(path + (e,)))
                            if key not in seen:
                                seen.add(key)
                                count += 1
                        continue
                    if other in visited:
                        continue
                    stack.append((other, path + (e,), visited | {other}))
        found[length] = count
        print(f"cycles of length {length}: {count}")

    total = sum(found.values())
    print(f"total cycles up to length {max_len}: {total}")
    print("VERDICT:", "cycles EXIST in the core" if total else
          "NO cycles found at all - the graph really is acyclic up to this length")
    return 0


if __name__ == "__main__":
    sys.exit(main())
