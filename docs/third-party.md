# Licenses and code provenance

The project is distributed under **MIT** (`LICENSE`), except for the files
explicitly marked below. This split is deliberate: MIT was chosen as the project's
primary license (trust, compatibility, a customer requirement), but the solver
code originates from `tromp/cuckoo`, which is distributed under **The FAIR MINING
License**, and that license is not MIT.

## 1. The FAIR MINING License (John Tromp)

Files and their derivatives:

| File | What was changed |
|---|---|
| `src/solver/lean_solver.cu` | port of `lean.cu`: fix for `EDGEBITS=32`, MSVC compatibility, GRIN header, dropping `exit()` |
| `src/solver/grin_params.hpp` | derivative of `cuckatoo.h` + `siphash.hpp` |
| `src/solver/grin_verify.hpp` | derivative of `cuckatoo.h` (`verify`, `setheader`) |
| `third_party/tromp-cuckoo/**` | unmodified, used as is |

Full text: `third_party/tromp-cuckoo/LICENSE.txt`.

### Local upstream patches (minimal, and marked in the code)

The MSVC build turned up two places that only compile with GCC/Clang:

| File | Patch | Reason |
|---|---|---|
| `src/cuckatoo/graph.hpp` | `sizeof(word_t[2*MAXNODES])` → `sizeof(word_t) * 2 * MAXNODES` (and the same for `link[2*MAXEDGES]`, 4 places) | `sizeof(T[n])` with a runtime bound is a VLA extension; MSVC: "expression must have a constant value" |
| `src/cuckatoo/compress.hpp` | `sizeof(word_t[SIZE])` → `sizeof(word_t) * SIZE` (2 places) | same |
| `src/solver/grin_params.hpp` | do not define `NODEBITS` | the macro conflicts with the field `u32 NODEBITS;` in `compress.hpp`; that is exactly why upstream defines it *after* `graph.hpp` |

The semantics of the `sizeof` patches are identical on all compilers. The changes
are marked with `LOCAL PATCH (GrinForge, ...)` comments in the files themselves.

### Key license condition

> **FAIR MINING**
> Any derived miner that charges a developer fee for mining a fair coin
> — one with no premine or other form of developer compensation —
> shall offer to share half the fee revenue with the coin developers.

**GrinForge charges no developer fee (0 %).** The FAIR MINING condition is worded
as an obligation arising *for the miner that charges a fee*. At a zero fee the
obligation does not activate, and the license explicitly permits:

> use, copy, modify, merge, publish, distribute, **sublicense**, and/or sell

Distribution of the derivative code is therefore lawful subject to two conditions,
which we meet:

1. the copyright notice and the FAIR MINING text are retained (this file + the headers in the files);
2. the dev fee is zero — and it cannot be added without switching to the FAIR MINING condition.

The license requirement "The above copyright notice, FAIR MINING condition, and
this permission notice shall be included in all copies or substantial portions of
the Software" is satisfied: `third_party/tromp-cuckoo/LICENSE.txt` is distributed
together with the sources, and every derivative file carries a reference to its
origin.

**Important:** if anyone adds a dev fee in the future, they are obliged to fulfil
the FAIR MINING condition and share half the revenue with the GRIN developers.
That is why the fee in this project is not merely "not enabled" — it is
architecturally not provided for.

## 2. BLAKE2b

`third_party/tromp-cuckoo/src/crypto/blake2b-ref.c`, `blake2.h`, `blake2-impl.h` —
reference implementation of BLAKE2, Copyright 2012 Samuel Neves.
License: **CC0 1.0 / OpenSSL / Apache-2.0, your choice** (see the header of `blake2.h`).
Compatible with the MIT project.

## 3. portable_endian.h

Public domain (Mathias Panzenböck). We do **not** use it — the endian helpers were
replaced by a direct `memcpy` in `grin_params.hpp` (`FIX-1`), because the target
platform is little-endian and the header requires `sys/param.h` and `htonll`,
which MSVC does not have.

## 4. Research materials (not part of the build)

| Item | Source | License | How it was used |
|---|---|---|---|
| `third_party/hires-cuckatoo/` | `client8568/High-Resource-Cuckatoo-Miner` | MIT | **reference only**, to reverse-engineer the 2Miners stratum protocol |
| `tools/stratum_probe.py` | our code | MIT | live protocol verification |

**No line of executable code was copied** from `High-Resource-Cuckatoo-Miner`.
Only protocol facts were taken from it (the 238-byte `pre_pow`, the 8-byte
big-endian nonce, the `login`/`getjobtemplate`/`submit` formats), and those were
confirmed by an independent live connection to the pool. Facts and algorithms are
not protected by license; what is protected is the specific expression of the
code — and it was not borrowed.

The idea of a VRAM table for Cuckatoo32 was also taken from
`High-Resource-Cuckatoo-Miner`; it was used in analytics only.

## 5. What is not used and why

| Project | License | Reason for rejection |
|---|---|---|
| `mozkomor/GrinGoldMiner` | **GPL-3.0** | a derivative would oblige us to license the whole project under GPL-3.0 and to release the sources; besides, the project is marked DISCONTINUED and was halted in January 2020 — *before* HardFork4 |
| `mimblewimble/grin-miner` | Apache-2.0 | license-compatible, but it does not build on Windows; for C32 it has only a 20 GB mean plugin |
| lolMiner, GMiner, Bminer | closed source | the licenses explicitly forbid modification, decompilation and changing the dev fee |
| `3k3r1l4rz/m1_grin_miner_fastest` | **no LICENSE** | defaults to all rights reserved |

## 6. Final license structure

```
GrinForge (as a whole)                    MIT
├── src/host/, src/stratum/, src/monitor/  MIT
├── src/solver/lean_solver.cu              FAIR MINING (derivative of lean.cu)
├── src/solver/grin_params.hpp             FAIR MINING (derivative of cuckatoo.h)
├── src/solver/grin_verify.hpp             FAIR MINING (derivative of cuckatoo.h)
├── third_party/tromp-cuckoo/              FAIR MINING / GPL-2.0+ (as is)
└── third_party/tromp-cuckoo/src/crypto/blake2*  CC0 / OpenSSL / Apache-2.0
```

Distributing the binary requires attaching the FAIR MINING text and the copyright
of John Tromp — see `third_party/tromp-cuckoo/LICENSE.txt`.
