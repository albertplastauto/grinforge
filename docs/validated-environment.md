# Validated environment (baseline)

All results for the solver, the stratum client, and share acceptance were obtained **on this
configuration**. Hashrate figures, driver behaviour, and the very fact that the pool accepts a
share are tied to the environment: if the driver, CUDA, or compiler changes, they must be re-verified.

## Pinned versions

| Component | Version |
|---|---|
| GPU | NVIDIA GeForce RTX 4060 Ti, 8188 MiB, compute capability **8.9** (Ada), VBIOS 95.06.26.00.52 |
| **NVIDIA driver** | **617.42** (CUDA UMD 13.4) — updated and re-verified 2026-10-10; 617.14 was the previous verified base |
| CUDA Toolkit | **13.4**, V13.4.59 (`nvcc`) |
| Compiler | MSVC **14.51.36231**, `cl` 19.51.36257 (Visual Studio 18 Community 2026) |
| Windows SDK | 10.0.26100.0 |
| CMake | 4.3.1-msvc1 |
| Ninja | 1.13.2 |
| Nsight Compute | 2026.3.0 (26.3.0.0) |
| OS | Windows 11 Pro, 10.0.26200 |
| Python (for independent checks) | 3.12.10 |
| Build architecture | `-arch=sm_89`, Windows x64 |
| Project revision at the time of verification | `1b3eabf` |

To check these values on another machine:

```bat
nvidia-smi --query-gpu=name,driver_version,vbios_version,memory.total,compute_cap --format=csv
nvidia-smi | findstr /C:"CUDA Version"
nvcc --version
cl 2>&1 | findstr Version
cmake --version
```

## What exactly was verified in this environment

| What | Result | Verified with |
|---|---|---|
| BLAKE2b-512 | matches an independent Python implementation | `solver_bench --selftest`, `tools/check_keys.py` |
| siphash-2-4 on device and on host | 64/64 match | `solver_bench --device-check` |
| Header: `pre_pow` 238 B + big-endian nonce | `OK` | `solver_bench --selftest` |
| Cuckatoo cycle search | 6-cycles on C20/P6, 42-cycles on C29, all pass `grin_verify` | `solver_bench_tiny`, `solver_bench29` |
| Full C32 lean solver | ~1.0 M edges after trimming, 16.9 s per graph, no hangs | long run |
| Hashrate | **0.053–0.055 GPS** at 28–76 W | `grinforge --tune`, `/stat` |
| Stratum | login, job, keepalive, submit | live pool grin.2miners.com:3030 |
| **Share accepted by the pool** | **accepted**, `method=submit result=ok`, `acc=1` | miner log + pool public API |
| Independent confirmation | `currentHashrates {"32":0.07}` and the worker in `/api/miners` | pool API |
| Telemetry | NVML: temperature, power, clocks, VRAM, fan | `grin_monitor`, `/stat` |
| GPU control | power limit and clock locking work with administrator rights; undervolt is impossible (`numBaseVoltages=0`); the fan is locked by the driver | `--tune`, `--install-gpu-profile` |
| GPU mode tuning | hashrate does not depend on the profile (±1 %), the GPS/W optimum is the ~2500 MHz ceiling | `grinforge --tune 25` |
| Crypto primitives | verified against the specification, not against our own code | `docs/root-cause.md` |

## Rule

> **If the driver, CUDA, or compiler has changed, re-run the verification.**
> Not because something is bound to break, but because all the numbers above
> refer specifically to this environment, and without re-verification they stop
> being proof.

Practical minimum after a driver update (2–3 minutes, free up the GPU):

```bat
:: 1. Crypto primitives and the header model
build\cmake\solver_bench.exe --selftest

:: 2. Device siphash vs. host (catches a change in compiler behaviour)
build\cmake\solver_bench.exe --pre-pow-file build\job.txt --device-check

:: 3. Cycle search on the small and on the real configuration
::    WARNING: --pre-pow-file is required; without it the tool prints usage.
build\cmake\solver_bench_tiny.exe --pre-pow-file build\job.txt --nonce-start 15 --nonce-count 6 --ntrims 16
build\cmake\solver_bench29.exe  --pre-pow-file build\job.txt --nonce-start 85 --nonce-count 3 --ntrims 68

:: 4. Hashrate across profiles
build\cmake\grinforge.exe --tune 25 --bench-pre-pow build\job.txt

:: 5. Mining and a real share
build\cmake\grinforge.exe --pool grin.2miners.com:3030 ^
    --user <wallet>.RIG1 --pass x --allow-address <wallet>
:: then in the log: "submit accepted by the pool"
```

### Expected results (taken from the release build)

| Step | Expected output |
|---|---|
| 1 | `selftest: passed (0 failure(s))`, five `OK:` lines |
| 2 | `device-vs-host siphash: agree (0/64 mismatches)` |
| 3 (C20/P6) | `attempts=6 ... solutions=5 verified=5` |
| 3 (C29/P42) | nonce 85: `raw_cycles=1 vfail=0 sols=1`, then `verify=OK lz=3 difficulty=14848` |
| 4 | 0.053–0.055 GPS on all profiles |
| 5 | `submit accepted by the pool`, `acc=1` |

`build\job.txt` is a saved job string from the pool (used as the fixed input for
all checks). If it is missing, take any current one: it contains
`"pre_pow":"<476 hex>"`.


## Driver: the update decision

As of 8 October 2026, the NVIDIA app was offering an update. **The decision is not to
update:** the current configuration is fully verified, the card is supported, and an
update would mean a reboot and the loss of the verified baseline. No gain for mining
is expected, and lolMiner's refusal (`Unsupported device or driver version`) is not
cured by a new driver — that is its own C32 support.

If an update does become necessary (for example, because of security fixes), proceed
as follows: record the current version (617.14), keep the installer for rollback, update,
reboot, run the set of commands above, and compare the hashrate against the table.

## 617.42: the update was taken, and re-verified (2026-10-10)

The decision recorded above was to stay on 617.14. On 2026-10-10 the operator took the update
anyway, after a system restore point, a fresh backup and a written pre-update baseline. The
paragraph above is left in place on purpose: it was the right call with the information available
at the time, and this section supersedes it.

Result: **no regression, and the numbers came out bit-identical where they must be.**

| Check | Baseline on 617.14 | On 617.42 |
|---|---|---|
| `selftest` | passed | passed |
| device-vs-host siphash | agree (0/64) | agree (0/64) |
| C20/P6 window | 5 of 6 verified | 5 of 6 verified |
| C29 nonce 85 | 428,633 edges, `raw_cycles=1 verify=OK lz=3` | **428,633 edges, `raw_cycles=1 verify=OK lz=3`** |
| C29 nonces 86 / 87 | 416,590 / 429,144 edges | **416,590 / 429,144 edges** |
| C32 edges | 1,026,168 | **1,026,168** |
| C32 trim / total | 15,917 ms / 17.34 s | 15,853 ms / 17.17 s |

Identical edge counts on three separate configurations, and the previously verified solution
still reproduces, so the driver changed neither the generated code nor its behaviour. The timing
difference is inside run-to-run noise.

Two operational notes from the same event, both of which had been fixed earlier the same day and
were confirmed here rather than assumed:

* the clock guard re-applied the 2500 MHz cap by itself after the reboot, because its state is
  tied to the boot time and a state recorded before a reboot is no longer trusted (version
  0.3.1). Without that fix the miner would have run at 2790 MHz and nobody would have noticed;
* the fixed 40% fan setting survived the driver update, which is not something to rely on - check
  it after any update.

