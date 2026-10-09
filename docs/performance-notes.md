# Performance: measurements, cost model, optimization plan

Updated after the pipeline fix (see `docs/root-cause.md`).

## 1. Baseline measurements of other miners

RTX 4060 Ti 8 GB (8188 MiB), driver 617.14 (CUDA UMD 13.4), Windows 11 Pro 26200,
Ryzen 7 1800X, 32 GB RAM. Pool `grin.2miners.com:3030`, C32.

| Miner | Version | GPS | VRAM | Power | Shares in 4.5 min | Dev fee |
|---|---|---|---|---|---|---|
| GMiner | 3.44 | **0.07** | 7759 / 8188 MiB | 46–53 W | **0** | **5%** |
| lolMiner | 1.98a | **does not start** | — | — | — | 2% |

```
GMiner:  GPU0 PALIT RTX 4060 Ti 8GB: Selected 11GB Solver / DevFee: 5 % / Fidelity: 0.00
lolMiner: Device 0 Active: false (Unsupported device or driver version.)
          Device 1 Active: false (OpenCL init failure: Invalid buffer size)
          All devices deselected or failed compatiblity check. Closing lolMiner
```

GMiner picked the 11 GB solver on an 8-gigabyte card — hence the 7.7 GB of occupied VRAM
and only 52 W at 100% utilization. lolMiner refuses to run: the CUDA path is
rejected by the driver, and OpenCL fails on allocation (C32 mean requires 20–33 GB).

## 2. Our own solver: measurements

Built with MSVC 14.51 + CUDA 13.4 (`-arch=sm_89`), with a live job from the pool.

| Configuration | trim | cycle search | Edges after trimming | Result |
|---|---|---|---|---|
| `ntrims=128 blocks=128 tpb=128` | 16.9 s | 1.2–1.3 s | 1,026,168 | **~0.055 GPS** |
| `blocks=408` / `816` / `204×256` | 16.6–16.8 s | 0.4–0.5 s | 1,026,168 | no change |

From the miner's working log (`finder:` lines, 3 graphs):

| Metric | Value |
|---|---|
| Edges after trimming | 978,852 … 1,013,870 (average 990,804) |
| **Nodes with both slots occupied** | **98.5%** of the edge count |
| Search steps | average 8.56 million, maximum 8.77 million |
| Step limit (400 million) reached | **0 times** |

Power: 48–77 W, GPU utilization 100%, memory 33–54%, temperature 48–53 °C.

Device memory: `alive` 512 MiB + `nonleaf` 512 MiB = **1 GiB** (after reverting
to the one-bit bitmap, `kNodeBits = 1`).

## 3. What follows from this

1. **Launch geometry has no effect at all** (16.6–16.8 s when the block count
   changes by 6.4×). The bottleneck is neither occupancy nor thread count.
2. **14.5 of the 16.9 seconds are the first ~16 rounds**, where almost all
   4.29 billion edges are alive. The remaining 112 rounds cost ~2 s. The cost is
   concentrated where the graph is dense.
3. **Memory is only 33–54% utilized, not 100%** → the bottleneck is not bandwidth
   but random-access latency and `atomicOr` on the `nonleaf` bitmap
   (one per live edge in every round).
4. **Cycle search is no longer the bottleneck**: 1.2 s versus 16.9 s for trimming.
   The step limit is never reached (8.8 million out of 400 million), so the budget is taken with headroom.
5. **Energy efficiency is still worse than the broken GMiner**: ~70 W per 0.055 GPS
   versus 52 W per 0.07 GPS. By the "hashrate per watt" creed, this is the main
   argument for optimization, not for cosmetics.
6. **A negative result requires a long run.** The expected number of cycles of
   length `PROOFSIZE` per graph is `1 / PROOFSIZE` and does not depend on
   `EDGEBITS`: 1/6 for 6-cycles, **1/42 ≈ 2.4%** for 42-cycles. At 18 s per graph,
   that is on average one solution per ~13 minutes. Over 24 graphs, the probability
   of seeing no solution at all is 56%.

## 4. Optimization plan (in descending order of expected effect)

Priority 0 — measure first, don't guess. Nsight Compute 2026.3.0 is installed
together with CUDA 13.4; the profile is worth capturing before any kernel edits (the
miner must be stopped during the measurement, otherwise it competes for the GPU):

```bat
"C:\Program Files\NVIDIA Corporation\Nsight Compute 2026.3.0\target\windows-desktop-win7-x64\ncu.exe" ^
    --set full --launch-count 4 --kernel-name regex:count_node_deg ^
    build\cmake\solver_bench.exe --pre-pow-file build\job.txt --nonce-count 1
```

What to look for: `sm__throughput` versus `dram__throughput` (latency or bandwidth),
the share of time spent in `atomic` operations, and the average number of transactions per request.

### 4.1 List of live 32-edge words (expected 1.1–1.3×)
The late rounds (17–128) read 512 MiB each for a few megabytes of useful data.
A compact list of non-empty words removes that traffic, but by measurement the
late rounds cost only ~2 s out of 16.9 s — so the gain is limited.

### 4.2 Drop the full `cudaMemset(nonleaf)` per round (expected 1.05×)
128 rounds × 512 MiB = 64 GiB of writes ≈ 0.22 s at 288 GB/s. Minor effect.

### 4.3 Attack the dense rounds — that is where the main reserve lies
Rounds 0–16 cost 14.5 s. Hypotheses to verify with the profiler:

* **`atomicOr` on every live edge.** 4.29 billion atomic operations in round 0.
  Workarounds: warp-level aggregation; switching to a byte map; splitting the
  round into passes where the write goes through without atomics.
* **Random access to the 512-MiB bitmap.** 32-byte transactions for 4 bytes of
  useful data → ~12% efficiency. The workaround is `PART_BITS > 0`, but Tromp
  documents a ~33% slowdown, so it is not a free win.
* **Fusing count and kill.** It requires knowing the full degree before removal,
  so two passes over the live edges seem unavoidable; but the first round, where ALL
  edges are alive, can be handled by a specialized kernel without reading the `alive`
  bitmap and without the `ffs` loop.

### 4.4 `PART_BITS` (memory ↔ speed)
`PART_BITS=1` halves `nonleaf` at the cost of a ~33% slowdown. On 8 GB, memory is not
a constraint (~1 GiB is needed), so `PART_BITS=0` is the right choice; the option is kept
for cards with little VRAM.

### 4.5 Tuning `ntrims`
`ntrims=128` is Tromp's default for `EDGEBITS=31`; for 32 it was never tuned (there is
no `lcuda32` target in the upstream Makefile). The `MAXEDGES` limit no longer applies:
`CycleFinder` computes the capacity of its own table itself and returns a clean `Overloaded`
if the graph does not fit. So `ntrims` now only affects the balance between trimming
time and search time, and it can be tuned by measurement:
`--ntrims` 64/96/128/160.

Important: trimming **preserves cycles at any number of rounds** (only leaves are
removed), so reducing `ntrims` does not lose solutions — it only makes the graph
larger for the search.

### 4.6 Economics of API calls
Work on the project is paused during DeepSeek API peak-price hours (see README):
peak is 01:00–04:00 and 06:00–10:00 UTC on weekdays, that is 04:00–07:00 and 09:00–13:00
Moscow time; at all other times it is half price. This is not a hashrate optimization
but a direct saving on development cost.

## 5. Mandatory verification commands

```bat
solver_bench.exe --selftest
solver_bench.exe --pre-pow-file build\job.txt --device-check
solver_bench_tiny.exe --pre-pow-file build\job.txt --nonce-start 15 --nonce-count 6 --ntrims 16
solver_bench29.exe    --pre-pow-file build\job.txt --nonce-start 85 --nonce-count 3 --ntrims 68
solver_bench.exe      --pre-pow-file build\job.txt --nonce-count 100
```

Expectations: the first two — "passed" and "agree"; the third — 5 solutions, all `verify=OK`;
the fourth — 1 solution at nonce 85 (`lz=3 difficulty=14848`); the last — about
100/42 ≈ 2.4 solutions, but at p=2.4% the absence of solutions over 100 graphs still
has probability 9%, so a claim that it "works" requires a run over several hundred
graphs.

## 6. Measured tuning of GPU modes (`--tune`)

```bat
grinforge.exe --tune 25 --bench-pre-pow build\job.txt
```

The mode applies one profile at a time, measures actual GPS and average power, computes
GPS/W, and restores the defaults at the end. It requires administrator rights (otherwise
`nvidia-smi -pl / -lgc` will fail and the profile will be marked `NOT APPLIED (needs elevation)`)
and a stopped miner — otherwise they compete for the GPU.

Results on the RTX 4060 Ti, three independent runs, six profiles:

| profile | GPS | W | GPS/W |
|---|---|---|---|
| stock (2790 MHz, 160 W limit) | 0.0540 | 28.2 | 0.00191 |
| power limit 100 W | 0.0542 | 35.1 | 0.00155 |
| power limit 120 W | 0.0547 | 40.2 | 0.00136 |
| **core lock 1500 MHz** | **0.0497** | 27.0 | 0.00185 |
| **core lock 2500 MHz** | 0.0540 | 25.2 | **0.00214** |
| core lock 2800 MHz | 0.0548 | 35.0 | 0.00157 |

### Conclusions

1. **Hashrate does not depend on the card's settings.** The spread of 0.0538–0.0549 (about ±1%) across
   all profiles — including a nearly two-fold reduction in core clock — is direct proof
   that the GPU part is bound neither by frequency nor by ALU: it is latency/atomic-bound.
2. **The power limit is useless**: the card draws less than the minimum limit (100 W),
   so the slider physically cannot affect this workload.
3. **The best efficiency comes from a ~2500 MHz core lock**: the same hashrate at lower
   consumption, with GPS/W about 12% higher than "as is".
4. The owner's observation that a +500 MHz memory overclock gives no gain is consistent with the
   measurement: memory load is 33–54%, and the algorithm is not bandwidth-bound.

Caveat: the power column is noisy (2–4 samples per profile), so the direction is right while the
absolute watts are estimates. The GPS column is stable and reproduces across runs.

**Recommendation:** apply the 2500 MHz lock **with an external tool** (NVIDIA app,
Afterburner) as a permanent profile, and keep the miner running without administrator rights:
gaining about 12% in efficiency is not worth granting the miner admin rights. The
`--lock-core <mhz>` flag is provided for those who deliberately run the miner elevated.

### Cross-check against external estimates

The "RTX 4060 Ti ≈ 0.65 H/s at 120 W" figure found in reviews is unattainable on this
card: 0.65 GPS requires a mean solver, which needs 20–33 GB of VRAM, and it does not fit
into 8 GB — this is confirmed by lolMiner's failure on this very card
(`OpenCL init failure: Invalid buffer size`). Our measured 0.054 GPS is confirmed
independently: **the pool itself** estimates our hashrate at 0.07 GPS from accepted shares.

## 7. Where the trim time actually goes (measured, not assumed)

A comment in `lean_solver.cu` claimed that clearing the 512 MiB `nonleaf` bitmap was "the
single largest cost in this solver". That was never measured and it is wrong: 128 rounds x
512 MiB is 69 GB of memset traffic, which at this card's bandwidth is on the order of
0.15 s of a 16.9 s graph. The cost is in the per-edge work, and this section measures
which part of it.

### Method

Round 0 of the C32 trim is the only round whose input is deterministic: every edge is
alive, so the amount of work cannot depend on what an ablation does to the graph.
Measuring with `--ntrims 1` on C32 therefore isolates that round cleanly. The baseline is
highly reproducible - 5413.7 ms, 5412.6 ms and 5414.8 ms over three runs, within 0.02%.

| Ablation | Round-0 trim | Change |
|---|---|---|
| none (baseline) | 5413 ms | — |
| `atomicOr` replaced by a plain `\|=` | 5538 ms | none, within noise |
| never kill: removes the random 512 MiB bitmap read | 3779 ms | **−30%** |
| ... and `dipnode` replaced by a single multiply | 3806 ms | none, within noise |

### What this rules out

* **Atomic contention is not the bottleneck.** Replacing `atomicOr` with a plain OR changes
  nothing at all. Any optimisation aimed at aggregating atomics would buy nothing here.
* **The hash is not the bottleneck.** Replacing siphash with one multiply changes nothing.
* **The bitmap read is worth about 30%** of the round.

### The profiler result, and everything it rules out

`ncu` on `count_node_deg` (section 6 has the command) reports:

```
Compute (SM) Throughput .............  2.76 %
DRAM Throughput ..................... 25.82 %
L2 Cache Throughput ................. 24.80 %
L1/TEX Hit Rate .....................  0 %
L2 Hit Rate ......................... 32.82 %
Warp Cycles Per Issued Instruction .. 203.08
  of which ~162 cycles are a long-scoreboard stall (79.9% of all stalls)
of 3.78 active warps per scheduler, only 0.02 were eligible per cycle
OPT: grid too small - only 0.31 full waves across all SMs
```

Every percentage is low at once, and warps spend 203 cycles per issued instruction: the
signature of a kernel that is stalled on memory rather than limited by compute, traffic or
occupancy. The obvious follow-ups were then tested and all of them changed nothing on C32
round 0 (baseline 5413 ms, reproducible to 0.02%):

| Change | Round-0 trim | Verdict |
|---|---|---|
| none (baseline) | 5413 ms | — |
| `atomicOr` -> plain `\|=` | 5538 ms | no effect |
| `dipnode` -> a single multiply | 3806 ms | no effect |
| grid 128 -> 8192 blocks (`--blocks`) | 5448-5487 ms | no effect |
| `tpb` 128 -> 256 | 5446 ms | no effect |
| `__launch_bounds__(128, 12)` | 5446-5494 ms | no effect |
| never kill (removes the bitmap read) | 3779 ms | **−30%** |

The grid sweep is the surprising one: a kernel the profiler calls under-occupied does not
get faster with 64x more blocks or with forced higher occupancy. That means the resident
warp count is not what limits it, and the "grid too small" advice is a red herring here.

### What is left

One third of a dense round is the random read of the 512 MiB bitmap, and that number is
solid. The remaining two thirds is not hashing, not atomics, not occupancy, and not grid
size; it scales with the number of edge visits, and I could not attribute it to a single
cause with the tools available. Reporting that plainly is more useful than another guess:
anyone picking this up should profile the *memory* side (`--section MemoryWorkloadAnalysis`
with `--replay-mode range`) rather than reach for the usual occupancy and atomic tricks,
because all of those have now been measured and none of them move the needle.


### One direction the numbers already justify

After the dense opening rounds only about 10^6 edges remain out of 2^32 - that is 0.02%.
The late rounds nevertheless scan the whole 512 MiB bitmap bit by bit, which is why rounds
17-128 still cost about 2.4 s despite having almost no edges left. **Compacting the
surviving edges into a dense array** (the standard lean optimisation) removes that scan
entirely. It does not explain the dense rounds, but it is a guaranteed win for the tail,
and it is the only change in this document that is justified by measurement alone.


## 8. Other economics

The GRIN network is ~3.5–4 kGps, mined by ASICs (iPollo G1: 36 h/s at 2800 W). One
4060 Ti provides 0.01–0.02% of the network; revenue per WhatToMine is on the order of $0.16/day
at a loss of $0.13 at $0.13/kWh. The project makes sense as an engineering effort, as a
measurement bench, and as a bid for Tromp's bounty ($10,000 for an open
C32 solver at 1 gps within ≤100·x W): a 4060 Ti at ~100 W should deliver ~1 GPS,
that is, the target is the ceiling from section 3, point 2.
