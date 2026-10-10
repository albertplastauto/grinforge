# Changelog

All notable changes to GrinForge. This project is young; the format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the versioning is
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.1] - 2026-10-10

### Fixed

- **The clock cap was lost after every reboot.** The guard keeps the 2500 MHz cap applied
  while the miner runs, and decides whether to act by comparing the desired state with what it
  last did. The state file survives a reboot; the driver's clock lock does not. So after a
  restart the guard concluded "already capped" and never re-applied it - observed on
  2026-10-10, when the miner ran at 2790 MHz and 73 W instead of 2490 MHz for as long as nobody
  looked. The state is now tied to the system's boot time, so a state recorded before the
  current boot is never trusted.

  Two attempts at that boot identifier were wrong and both were caught by testing rather than
  by reading the code: `[math]::Floor` on a `DateTime` silently produced `0` (the identifier
  never changed, so the bug stayed exactly as it was), and `[Environment]::TickCount64`
  returned nothing in the scheduled task's language mode, which made the identifier equal the
  *current* time and therefore change every minute, defeating the "nothing to do" shortcut.
  `LastBootUpTime` is now used, and verified to match the real boot time exactly and to be
  stable across consecutive runs.

## [0.3.0] - 2026-10-09

A performance release whose gain is measured and verified, plus a long-run test that has now
accumulated real evidence: several shares found on C32, accepted by the pool, with no local
verification failure.

### Added

- **Dense-tail edge trimming.** After 16 rounds the surviving edges are copied once into a
  dense array and the remaining 112 rounds iterate that array instead of scanning the 512 MiB
  alive bitmap. The motivation was measured, not assumed: a tail round costs 56.6 ms with 52M
  edges alive and still 25.1 ms with 1.03M, so the round is dominated by the bitmap scan, not
  by the edges. On C32 the trim goes 16,912 → 15,917 ms and the whole graph 18.36 → 17.34 s,
  a 5.6% gain.

  Verified by comparing edge counts against the recorded baselines rather than by trusting the
  code: C29 stays at 428,633 edges and C32 at 1,026,168 (both exact), and the previously
  verified C29 nonce-85 solution still reproduces as `raw_cycles=1 verify=OK lz=3`. Then
  confirmed on the live pool: 4 accepted shares, 0 failed verifications.

- **Randomised starting nonce.** The mining loop began every job at nonce 0, so a miner
  restarted inside the same job window replayed the same nonces and submitted a duplicate;
  the pool answered "Duplicate share". Each process now starts at a random point and logs it.

### Fixed

Two bugs introduced and caught while implementing the dense tail — neither by reading the
code, both by comparing against known-good baselines:

- The dense keep condition was inverted. The bitmap path kills an edge whose partner slot is
  absent, so the dense path must keep it when the slot is present; the first version kept the
  leaves instead, leaving a graph an order of magnitude too large (C29: 1.25M edges against
  428,633) and losing a previously verified solution.
- Warp-aggregated appends lost writes. Space was reserved by lane 0, but in an index-strided
  loop lane 0 can finish while other lanes are still running, so no reservation happened and
  the warp wrote through a stale base. The symptom was a survivor count that halved every
  round and reached zero, which reads like an over-eager kill rule. The loop now uses a
  uniform trip count so the full warp participates in every ballot.

### Documented

- Where the trim time goes: the ablations that ruled out atomics, the hash, grid size and
  occupancy, and the `ncu` profile behind them.
- The supervisor's log naming — the file is named after the launch date, not the calendar day,
  so a run crossing midnight keeps appending to the file it started with.

## [0.2.0] - 2026-10-09

A maintenance release: two real bug fixes, a measured performance investigation that ruled
out the usual optimisations, and a release package that can actually be unpacked anywhere.
**No hashrate improvement** - saying that plainly is part of the release.

### Fixed

- **The GPU clock guard no longer flashes a console window every minute.** It was launched
  as `powershell.exe -WindowStyle Hidden`, which still creates a console for a moment, so a
  window visibly blinked on screen every 60 seconds. It now goes through `wscript.exe`
  (`tools/run-hidden.vbs`), which has no console of its own.
- **`stop-miner.bat` no longer kills its own caller.** It stopped the supervisor by
  searching process command lines for `run-miner-forever.ps1` - a string that also appears
  in the command line of any shell that merely mentions the file. It now uses the PID the
  supervisor writes to `logs\supervisor.pid`. Hit twice during development, both times
  presenting as an exit code of -1 with no output at all.
- **The release package was incomplete.** `install-gpu-clock-guard.bat` was shipped without
  the `tools\gpu-clock-guard.ps1` and `tools\run-hidden.vbs` it references, so the packaged
  guard could not be installed.

### Changed

- The operator scripts derive their paths from their own location instead of hardcoding
  `E:\grin-miner`, so the project works wherever it is unpacked.

### Documented

- **Where the trim time goes, measured rather than assumed.** A comment in the solver
  claimed the 512 MiB bitmap clear was "the single largest cost in this solver". It had
  never been measured and it is wrong: about 0.15 s of a 16.9 s graph. An ablation study on
  C32 round 0 - the only round whose work is deterministic, with a baseline reproducible to
  0.02% - plus an `ncu` profile, established the following:

  | Change | Round-0 trim | Verdict |
  |---|---|---|
  | none (baseline) | 5413 ms | — |
  | `atomicOr` -> plain `\|=` | 5538 ms | no effect |
  | siphash -> a single multiply | 3806 ms | no effect |
  | grid 128 -> 8192 blocks | 5448-5487 ms | no effect |
  | `tpb` 128 -> 256 | 5446 ms | no effect |
  | `__launch_bounds__(128, 12)` | 5446-5494 ms | no effect |
  | never kill (removes the random 512 MiB read) | 3779 ms | **−30%** |

  The profiler calls the kernel under-occupied ("0.31 full waves") and every utilisation
  metric is low at once - SM 2.76%, DRAM 25.82%, L1 hit rate 0% - with 203 cycles per issued
  instruction, ~162 of them a long-scoreboard stall. But 64x more blocks do not help, so the
  occupancy advice is a red herring here. One third of a dense round is the random bitmap
  read; the remaining two thirds is not the hash, not atomics and not occupancy, and it is
  recorded as unattributed rather than guessed at.

## [0.1.0] - 2026-10-08

First working release. It mines GRIN Cuckatoo32 on an NVIDIA Ada (sm_89) GPU under
Windows x64 and has had shares accepted by a real pool. It is not fast, and the
documentation says so plainly.

### Added

- **Cuckatoo32 lean CUDA solver** (`src/solver/`), ported from `tromp/cuckoo` and brought
  up for `EDGEBITS=32` on `sm_89` with MSVC. Trims a 2^32-edge graph down to roughly one
  million edges and searches it for 42-cycles.
- **Independent verifier** (`src/solver/grin_verify.hpp`) checked against GRIN's published
  `V1_32` consensus vector, so a solver bug cannot silently produce cycles the pool would
  reject.
- **GRIN stratum client** (`src/stratum/`): login, `getjobtemplate`, `job` notifications,
  `keepalive` and `submit`, with a verified `submit` envelope and automatic fallback to
  the other known envelope.
- **Mining loop and watchdog** (`src/host/main.cpp`): nonce iteration, thermal guard,
  stall recovery, pool failover, graceful shutdown.
- **Telemetry and GPU control** (`src/monitor/`): NVML readings and NVAPI / `nvidia-smi`
  based control, with every refusal reported instead of silently ignored.
- **Wallet guard**: `--allow-address` refuses to start unless the address about to be
  mined to is exactly the one the operator intends.
- **Measured GPU tuning**: `--tune` sweeps profiles, measures GPS and GPS/W, and restores
  defaults. `--install-gpu-profile` applies the measured optimum as a one-shot privileged
  action.
- **Guard against configuration drift**: a scheduled task keeps the 2500 MHz core-clock
  cap applied only while the miner runs, so games get the full boost.
- **Long-run supervisor**: `run-miner-forever.ps1` restarts the miner if it dies and logs
  to one append-only file named after the date the supervisor started. A run that crosses
  midnight keeps appending to the file it started with - the name is the launch date, not
  the calendar day of each line.
- Documentation: protocol specification verified against a live pool, a post-mortem of two
  wrong diagnoses, measured performance notes, a licence attribution record, and a pinned
  validated environment with a re-validation procedure.

### Verified

- BLAKE2b and siphash agree with independent Python implementations.
- Device and host siphash agree on all 2^20 edges of a small graph.
- Real cycles found and verified: 6-cycles at C20/P6 (5 of 6 nonces) and 42-cycles at
  C29/P42.
- A share submitted to 2miners returned `method=submit result=ok`, and the pool's public
  API reported our wallet's hashrate and listed the worker as active.

### Known limitations

- **Hashrate is a few hundredths of a GPS** (~0.055 locally, 0.07 as the pool accounts for
  it). On an 8 GB card only the lean solver fits; the faster mean solver needs 20-33 GB.
  Figures such as "RTX 4060 Ti ~ 0.65 H/s" are not reachable here.
- **GPU settings do not change hashrate at all.** Measured across six profiles, the spread
  is about 1 %, including halving the core clock. The lever is the kernel, not the card.
- **Undervolting is impossible on this SKU** (`numBaseVoltages = 0`) and fan control is
  blocked by the driver on consumer GeForce cards.
- **Failover and multi-GPU are implemented but not exercised live** (one pool, one card).
- **Not tested on other GPU models.** Other Ada cards should work; older architectures
  will need `CMAKE_CUDA_ARCHITECTURES` changed.
- Mining GRIN on a GPU is not competitive with ASICs. This project is about a correct,
  auditable, fee-free implementation.

[0.3.1]: https://github.com/albertplastauto/grinforge/releases/tag/v0.3.1
[0.3.0]: https://github.com/albertplastauto/grinforge/releases/tag/v0.3.0
[0.2.0]: https://github.com/albertplastauto/grinforge/releases/tag/v0.2.0
[0.1.0]: https://github.com/albertplastauto/grinforge/releases/tag/v0.1.0
