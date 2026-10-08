# GRIN stratum protocol — verified against grin.2miners.com:3030

Status: **verified empirically** (live probe) and cross-checked against a working
open-source client, `client8568/High-Resource-Cuckatoo-Miner` (MIT), which is
known to mine 2Miners. Everything below is evidence-backed; nothing is assumed.

Evidence:
- Live capture: `tools/stratum_probe.py` against `grin.2miners.com:3030`.
- Reference client: `third_party/hires-cuckatoo/main.cpp` (lines 8030-8130 login,
  8405-8411 getjobtemplate, 8783 keepalive, 11179-11230 submit, 13240-13310 job
  parsing) and `third_party/hires-cuckatoo/blake2b.h` (lines 60-165, hash input).
- Reference build config: `third_party/hires-cuckatoo/Makefile` lines 4, 16, 19, 22.

## 1. Transport

Plain TCP (no TLS on port 3030), newline-delimited JSON-RPC 2.0. One JSON object
per line, terminated by `\n`. Server may send an `id` field of `"Stratum"` for
unsolicited notifications.

## 2. Message flow

### login (client → server)
```json
{"id":"1","jsonrpc":"2.0","method":"login","params":{"login":"<wallet>.<worker>","pass":"x","agent":"<name>/v<version>"}}
```
### login response (server → client)
```json
{"id":"1","jsonrpc":"2.0","method":"login","result":"ok"}
```
The reference client waits for `"method":"login"` (also tolerates `"method": "login"`).

### getjobtemplate (client → server)
Two forms exist in the reference client:
```json
{"id":"1","jsonrpc":"2.0","method":"getjobtemplate","params":{"algorithm":"Cuckoo"}}
{"id":"1","jsonrpc":"2.0","method":"getjobtemplate","params":null}
```
Our probe used `"params":[]` and 2Miners answered with a full job, so the server is
permissive. Prefer `"params":{"algorithm":"Cuckoo"}` (matches the reference build
where `STRATUM_SERVER_MINING_ALGORITHM_NAME = "Cuckoo"`).

### keepalive (client → server), every 10 s
```json
{"id":"1","jsonrpc":"2.0","method":"keepalive","params":null}
```

### job notification (server → client)
```json
{"id":"Stratum","jsonrpc":"2.0","method":"job","params":{"height":4053496,"difficulty":1,"job_id":0,"pre_pow":"<476 hex chars>"}}
```
- `pre_pow` is exactly `HEADER_SIZE_EXCLUDING_NONCE = 238` bytes → 476 hex chars.
- `height` is the block height the job belongs to.
- `difficulty` is the share difficulty (1 on 2Miners C32).
- A job may also arrive as the `result` of `getjobtemplate`.

### submit (client → server)
Primary form (reference build with `STRATUM_SERVER_MINING_ALGORITHM_NAME="Cuckoo"`):
```json
{"id":"1","jsonrpc":"2.0","method":"submit","params":{"height":H,"job_id":J,"nonce":N,"pow":{"Cuckoo":[32,[e0,e1,...,e41]]}}}
```
Alternative form used when the Cuckoo-keyed variant is disabled:
```json
{"id":"1","jsonrpc":"2.0","method":"submit","params":{"edge_bits":32,"height":H,"job_id":J,"nonce":N,"pow":[e0,e1,...,e41]}}
```
`pow` is the 42 edge nonces (PROOFSIZE = 42) as decimal integers. `nonce` is the
u64 PoW nonce used to produce them. Implement both; select by config, fall back on
a malformed-request rejection.

## 3. Header → siphash keys (the part that breaks Tromp's default ABI)

Reference constants:
| Constant | Value |
|---|---|
| `EDGE_BITS` | 32 |
| `SOLUTION_SIZE` / `PROOFSIZE` | 42 |
| `HEADER_SIZE_EXCLUDING_NONCE` | 238 |
| `NONCE_SIZE` | 8 |
| `NONCE_IN_HEADER_IS_BIG_ENDIAN` | true |

Algorithm, exactly as the reference client does it:

1. `header[0..237] = hex_decode(pre_pow)` — **the bytes are used verbatim, no
   byte-swapping of the header** (`readJobMessage` just decodes hex).
2. Append the u64 nonce **big-endian**: `header[238..245] = be64(nonce)`.
   (`blake2b.h` does `nonce = bswap64(nonce)` before `memcpy`, so the 8 bytes in
   the buffer are big-endian.)
3. Hash the **246-byte** buffer with BLAKE2b (512-bit), keyed with length
   `238 + 8 = 246`, no key.
4. Map the digest to siphash keys as four **little-endian u64**:
   `k0..k3 = le64(digest[0..7]), le64(digest[8..15]), le64(digest[16..23]), le64(digest[24..31])`.
   This matches `tromp/cuckoo` `src/crypto/siphash.hpp::setkeys`, whose
   `htole64(((uint64_t*)keybuf)[i])` is the identity on x86-64.

**Consequence for the Tromp solver:** do NOT use
`lean.cu::run_solver`'s built-in nonce handling. That path (`setheadernonce`)
writes a 4-byte little-endian nonce into the last 4 bytes of an assumed 80-byte
buffer, i.e. it is designed for Cuckaroo/Cuckatoo29-style 76+4 headers. For GRIN
C32 on a pool, the nonce is 8 bytes big-endian appended to a 238-byte prefix.
Build the 246-byte buffer ourselves, then set `mutate_nonce = false` and let the
solver hash the buffer as-is. Each new nonce requires a fresh BLAKE2b → fresh
siphash keys → a fresh graph.

## 4. Observed job samples (for regression tests)

```text
height 4053495  difficulty 1  job_id 4
pre_pow (238 bytes):
000500000000003dd9f7000000006ac7b1fd000478d9f83f75fea4ccc29dfb3e8642afa8f2871bd31e26aabcb3967ecf78e655e9a63965678058d8fefe0570762e420c619ac2bfebc41da92722d2ad60f593b858d57787427d21550fdf620878cd8165008eefa1ecd6d7be3af5fbff2bc23f8096e0f166a140b323d3fc083dd306c3bdf6778da1320349316cf722b84bf9305fbd0c64203694a143802f5cebdb3d0ee5083bfae729b88f5b6bbd7c8baa46e842f576580454e46da44c3c56fa4f6ce841479c57063abef59e542346680b5d5b00000000011cbc960000000000c2c7c900086dbdad185dfc00000000

height 4053496  difficulty 1  job_id 0
pre_pow differs from the above in these byte ranges: [9], [16..17], [19..177], [217], [225], [230..232]
```

Byte 2..9 read big-endian equals the job `height` (0x3DD9F7 = 4053495) and byte 9
increments by 1 between consecutive jobs, which is how the 238-byte layout was
identified. Do not reorder or normalise these bytes — hash them as delivered.

## 5. Test vectors

`tools/stratum_probe.py <host:port> <seconds>` prints raw traffic and a decoded
schema. Use it to verify a client without submitting shares.
`tools/pre_pow_analyze.py` dumps a `pre_pow` blob byte-by-byte with offsets.

## 6. Resolved: which form of `pow` the pool actually accepts

The first real submission (a share with `lz=6`, difficulty 1 048 576) went out **in
the form keyed by `Cuckoo`** — and the pool did not answer at all: neither `accepted`
nor `rejected`. Locally `total_accepted_shares` stayed at 0, and the pool's public API
showed neither `currentHashrates` for our wallet nor a worker in the list of active
miners. In other words, the pool silently ignores the keyed form.

The deciding factor is the reference client `client8568/High-Resource-Cuckatoo-Miner`
(MIT), which is known to work with this pool. Its `Makefile`:

```
STRATUM_SERVER_USES_MORE_THAN_ONE_MINING_ALGORITHM = false
```

With `false`, `main.cpp` takes the `#else` branch, i.e. the **flat** form:

```json
{"id":"1","jsonrpc":"2.0","method":"submit","params":{"edge_bits":32,"height":438...,"job_id":0,"nonce":...,"pow":[e0,...,e41]}}
```

Here `edge_bits` sits at the top level of `params`, and `pow` is just an array. **This
form is the default** in our client (`Config::use_edge_bits_submit_form = true`). The
form keyed by `Cuckoo` remains as an automatic fallback in case of a “malformed”
rejection.

### What else the pool's public API confirms

`https://grin.2miners.com/api/stats`:

| Field | Value | Meaning |
|---|---|---|
| `minDiff` | **16384** | minimum share difficulty = `graph_weight(32)`. Our share with `lz=6` has difficulty 1 048 576 — 64 times the minimum, so difficulty is not the issue |
| `netdiff` | 88 837 534 | network difficulty |
| `nethr` | 3486 | network hashrate in GPS |
| `minersTotal` / `workersTotal` | 141 / 419 | the pool as a whole |

### Connection behaviour

The pool closes the TCP connection roughly every **110 seconds** (RST, Winsock 10054) —
that is its policy, not a client bug: a keepalive every 10 s gets `result: ok`, and jobs
arrive regularly. The client reconnects within 1 s. The risk is that a share sent
immediately before the disconnect is left unanswered — that is exactly what happened
with the first submission. For this reason every miner log line carries a timestamp:
without them the correlation between submissions and disconnects cannot be measured.

## 7. Verified

1. **Acceptance of a share by the pool in the flat form is confirmed.** 8 October 2026 at 21:04 MSK:

   ```
   submitted solution height=4053661 job_id=0 nonce=0 lz=1
   recv: {"id":"49","jsonrpc":"2.0","method":"submit","result":"ok"}
   submit accepted by the pool
   ```

   Locally `acc=1 rej=0 stale=0`. Independently, from the pool's side:
   `https://grin.2miners.com/api/accounts/<wallet>` returned
   `currentHashrates: {"32":0.07}`, and the worker appeared in
   `https://grin.2miners.com/api/miners`.

2. **Minimum share difficulty threshold**: `minDiff` in `/api/stats` is 16384, i.e.
   `graph_weight(32)`. Our shares start at 16384 · 2^lz, so they clear it with room to
   spare.

3. **Connection behaviour**: the pool closes TCP every ~110 s by its own policy; the
   client reconnects within 1 s. A share sent immediately before the disconnect may be
   left unanswered — that is exactly what happened with the first submission, which was
   still in the wrong form.
