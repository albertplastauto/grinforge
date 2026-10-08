# Post-mortem: why the pipeline found no solutions, and how it was fixed

This document was rewritten after two earlier versions of the diagnosis turned out
to be wrong. What follows is only verified facts and explicitly flagged lessons.

## Bottom line

**The pipeline works.** Confirmed by independently reproducible results:

```
solver_bench_tiny --pre-pow-file build\job.txt --nonce-start 15 --nonce-count 6 --ntrims 16
  attempts=6 solutions=5 verified=5        every solution verify=OK

solver_bench29    --pre-pow-file build\job.txt --nonce-start 85 --nonce-count 3 --ntrims 68
  nonce 85: raw_cycles=1 sols=1 verify=OK lz=3 difficulty=14848
  attempts=3 solutions=1 verified=1
```

The second result is a genuine 42-cycle: 42 distinct edge indices that passed
`grin_verify`, meaning it is good enough to submit to a pool.

## Root cause: the Cuckatoo node model

The key was found **in the GRIN consensus code**, not in our tests:

> **A Cuckatoo node is `sipnode(keys, e, uorv) >> 1`.** The discarded low bit is
> the **parity slot**, and the two edges of a cycle that meet at the same node must
> have **different** slot values.

Proof: our `grin_verify` accepts the consensus vector `V1_32` published in GRIN
(header `[0u8;80]` + little-endian u32 nonce 17). Its 42 u-ends form exactly
21 pairs `{x, x^1}`, while matching nodes by equality of the "raw" value is
rejected with `POW_DEAD_END`.

This model implies **two** independent bugs, and both had to be fixed.

### Bug 1 (introduced by me): the CH-7 rule was a regression

I replaced the one-bit `nonleaf` bitmap with a degree-count ≥ 2 over "raw" values,
assuming the original `^1` in Tromp was a typo. That turned out to be wrong: `u ^ 1`
is the **slot partner of the same node**, and Tromp's whole construction is
consistent (`graph.hpp::adjlist[u ^ 1]`, `compress.hpp` with `parity = u & 1`,
`kill_leaf_edges` with `!nonleaf.test(u ^ 1)`).

CPU measurements that exactly reproduce the kernel logic (C20/P6, 16 rounds):

| Rule | Live edges | Nodes with both slots occupied |
|---|---|---|
| CH-7 (degree ≥ 2 by raw value) | 13 533 | **48** |
| partner-slot (`kill if sipnode^1 is absent`) | 13 209 | **11 831** |

With 48 nodes, a valid cycle cannot exist in principle: the `raw_cycles` counter
would have stayed at zero even with a perfect search. The rule was restored to
`partner-slot`, as in GRIN's `core/src/pow/lean.rs::count_and_kill` and in
`tromp/cuckatoo/lean.cu`. `kNodeBits` 2 → 1.

### Bug 2: the cycle search ignored slots

The first version of my `cycle_finder.hpp` interned the "raw" `sipnode` values
without the slot — such cycles cannot pass `grin_verify`. Rewritten: `raw >> 1`
is interned into a dense index, the slot is carried onto the half-edge, the walk
alternates U/V and requires a **different slot** at each node, closing exactly the
way `grin_verify` traverses. The DFS is iterative, with a hard `max_steps` limit.

### Separately: a build-system trap

Ninja **did not track** `lean_solver.hpp` for the C++ targets: changing the header
relinked but did not recompile `solver_bench.cpp` and `main.cpp`.
The stale harness read `SolverConfig::max_search_steps` as uninitialized
**0**, the search was capped at zero steps, and `raw_cycles=0` came out **even with
the solver fixed**; on top of that, `last_run()` wrote a larger struct into the
caller's smaller temporary variable (`STATUS_HEAP_CORRUPTION`). Fixed with
explicit `OBJECT_DEPENDS` in `CMakeLists.txt`.

## Expected solution rate

The mathematical expectation of the number of cycles of length `PROOFSIZE` per
graph is **1 / PROOFSIZE**, independent of `EDGEBITS`:

| Cycle | Expectation per graph | Observation |
|---|---|---|
| 6 at C20 | 1/6 ≈ 0.167 | 7 solutions across 24 graphs |
| 42 at C29 | 1/42 ≈ 0.024 | 4 solutions across 170 graphs |
| 42 at C32 | 1/42 ≈ 0.024 | 0 across 3 graphs (0.07 expected) |

Hence: **the absence of solutions in short runs proves nothing.** With
p = 1/42, the probability of seeing no solution at all across 24 graphs is 56%.

## What measurements confirmed (summary)

| Claim | Check | Result |
|---|---|---|
| Header → siphash keys | independent BLAKE2b and siphash in Python | match bit for bit |
| device and host siphash | `--device-check`, plus cross-check over the entire 2^20 edge space | 0 discrepancies |
| Node model | GRIN consensus vector `V1_32` through `grin_verify` | OK; 21 pairs `{x, x^1}` |
| Trimming | agreement with a CPU replication of the kernel logic per nonce | 13209/13754/12958/12776/13370/12272 |
| Cycle search | reproduced solutions | C20: 5/5 OK; C29: 42-cycle OK, difficulty 14848 |
| Step bound | maxima 65k / 3.9M / 8.9M against a 400M limit | limit not reached; a forced 200-step limit does trigger |
| No hangs | C32: ~1.0M edges, 16.9 s of trimming, no `OVERLOADED` | reproduced |

## Lessons

1. **Verification parameters must be chosen where solutions are guaranteed to exist.**
   `EDGEBITS=20` with `PROOFSIZE=6` is not such a case: the expectation is 1/6 per
   graph, and zero across six graphs is normal.
2. **An independent implementation does not help if it repeats the wrong model.**
   I "confirmed" the trimming with an independent Python computation of the 2-core
   (GPU 488 = Python 488), but the Python used the same "raw" node model as CH-7.
   That was a check of the implementation of a wrong model, not of the model.
3. **The model must come from the specification, not from your own code's behavior.**
   The correct model was found in the GRIN consensus vector, not in our tests.
4. **A negative result on a small sample is not proof.** My two
   "root causes" were both built on insufficient statistics.
5. **The build system is part of the pipeline.** A stale object file produced
   `raw_cycles=0` with already-fixed code and masked the fix.
