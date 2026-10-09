// GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
// Modifications Copyright (c) 2026 albertplastauto
// Portions derived from tromp/cuckoo, Copyright (c) 2013-2020 John Tromp,
// distributed under "The FAIR MINING License" (see LICENSE).
// SPDX-License-Identifier: LicenseRef-Fair-Mining
//
// Cuckatoo32 "lean" GPU solver — Cuckatoo32 port for CUDA 12/13 on sm_89 (Ada).
//
// PROVENANCE
//   Port of tromp/cuckoo `src/cuckatoo/lean.cu`
//   (Copyright (c) 2013-2020 John Tromp, "The FAIR MINING License").
//   The edge-trimming memory optimisation is due to Dave Andersen.
//   See third_party/tromp-cuckoo/LICENSE.txt and src/solver/grin_params.hpp.
//
// CHANGES vs upstream (upstream has no EDGEBITS=32 CUDA target at all; the
// Makefile ships lcuda19/29/30/31 only):
//   CH-1  32-bit correctness fix: `NONPART_BITS` / `NONPART_MASK` are computed in
//         64-bit and the unpartitioned (PART_BITS==0) case is short-circuited, so
//         no 32-bit value is ever shifted by 32 (upstream UB at EDGEBITS=32).
//   CH-2  Explicit shift handling: `alive >>= ffs` with ffs==32/64 relied on PTX
//         shift clamping. Made explicit so behaviour is defined.
//   CH-3  MSVC portability: no GNU statement-expression macros, no <unistd.h>,
//         no getopt; host bit intrinsics route through grin_params.hpp.
//   CH-4  "overloaded" no longer calls exit(0) — it returns a status, because a
//         miner must never kill itself.
//   CH-5  Raw device pointers are passed as kernel arguments instead of copying a
//         self-referential host struct into device memory.
//   CH-6  GRIN-specific header/key derivation (238-byte pre_pow + big-endian u64
//         nonce, BLAKE2b -> siphash keys) lives in grin_setheader.
//   CH-7  Two correctness fixes without which NO solution can ever be reported:
//           (a) the final stage is src/solver/cycle_finder.hpp, a bounded,
//               slot-aware Cuckatoo cycle finder, replacing
//               graph.hpp/compress.hpp/add_compress_edge;
//           (b) the trim kernels use the partner-slot rule (`w ^ 1` present)
//               instead of a degree >= 2 counter. See the CH-7 comment on
//               slot_seen_set/slot_seen_test below - the degree rule left almost
//               no node with both slots occupied, which is why raw_cycles was 0.

#include <cuda_runtime.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "grin_params.hpp"
#include "grin_verify.hpp"

// Upstream headers, unmodified. They are designed to be included after the
// parameter header has defined u32/word_t/PROOFSIZE/EDGEBITS/print_log.
#include "cycle_finder.hpp"
#include "siphash.cuh"
#include "lean_solver.hpp"

namespace grin {
namespace {

// ---------------------------------------------------------------------------
// Algorithm constants
// ---------------------------------------------------------------------------

#ifndef MAXSOLS
#define MAXSOLS 4
#endif

// Bits used to partition the edge set, to trade memory for speed.
//   0 = no partitioning, fastest, needs 512 MiB per bitmap (our default)
//   1 = half the node bitmap at roughly 33% slowdown
#ifndef PART_BITS
#define PART_BITS 0
#endif

// NOTE: upstream's `#define MAXEDGES (NEDGES >> IDXSHIFT)` and the whole
// graph.hpp/compress.hpp path are gone. They were only needed to size the graph
// that the (broken) compressed cycle finder built; `CycleFinder` sizes itself from
// the actual surviving edge count, so the compressor's fixed 2^(EDGEBITS-IDXSHIFT)
// table — which silently overflowed and returned garbage node ids — no longer
// exists in this build.

// CH-7 (see the helper section below): the leaf test is the partner-slot test, so
// one bit per raw endpoint value is enough (upstream's `biitmap`).
const u64 kNodeBits = 1;

const u64 kEdgeBytes = NEDGES / 8;                       // 512 MiB at C32
const u64 kNodeBytes = ((NEDGES >> PART_BITS) * kNodeBits) / 8;   // 512 MiB at PART_BITS=0
const u32 kPartMask = (1u << PART_BITS) - 1u;
const u32 kNonPartBits = EDGEBITS - PART_BITS;

// CH-1: 64-bit mask computation; ~0 when there is no partitioning.
const word_t kNonPartMask =
    (PART_BITS == 0) ? (word_t)~0ULL : (word_t)(((u64)1 << kNonPartBits) - 1ULL);

// ---------------------------------------------------------------------------
// Dense tail
// ---------------------------------------------------------------------------
// Measured, not assumed: the cost of a trim round in the tail barely depends on how many
// edges are still alive. Rounds 16-24, with 52M to 25M edges alive, cost 56.6 ms each;
// rounds 120-128, with 1.16M to 1.03M alive, still cost 25.1 ms each. A 50x drop in live
// edges buying only a 2x drop in time means the round is dominated by something that does
// not scale with the edges: reading the 512 MiB alive bitmap in a grid-stride pattern with
// a 64 KiB step, so every iteration lands in a different page and nothing can be prefetched.
//
// So once the graph has become sparse, the surviving edges are copied into a dense array and
// the remaining rounds iterate that array instead. The bitmap scan disappears; 112 rounds x
// ~20 ms is the 2.2 s this is meant to recover out of a ~17 s trim.
const u32 kDenseAfterRound = 16;                   // bitmap rounds before switching
const u32 kDenseCapacity = 64u * 1024u * 1024u;    // edges; 256 MiB per ping-pong buffer

__device__ __forceinline__ bool in_partition(word_t u, u32 part) {
    if (PART_BITS == 0) return part == 0;   // everything belongs to partition 0
    return ((u >> kNonPartBits) == part);
}

// The "alive" set is inverted: a set bit means the edge is DEAD. It starts fully
// zeroed (everything alive) and threads kill edges, each thread owning a disjoint
// run of 32-edge words, so no atomic is required.
__device__ __forceinline__ u32 alive_block(const u32* alive, u64 block) {
    return ~alive[block >> 5];
}
__device__ __forceinline__ void alive_kill(u32* alive, u64 nonce) {
    alive[nonce >> 5] |= (1u << (nonce & 31));
}

// CH-7 (corrected): the trimming rule must be the *partner-slot* test, exactly as
// in Grin's own lean miner (`core/src/pow/lean.rs::count_and_kill`) and upstream
// `src/cuckatoo/lean.cu`: kill an edge when the raw endpoint value `w` of the
// processed side has no alive counterpart at `w ^ 1`.
//
// Why `^ 1` is correct here (and not a degree counter): a Cuckatoo NODE is
// `sipnode(...) >> 1` and the dropped low bit is a two-slot parity that
// grin_verify requires to DIFFER between the two cycle edges meeting at that node.
// Grin's consensus verifier matches half-edges by `uvs >> 1` and rejects a match
// whose raw values are equal (`POW_DEAD_END`), and grin_verify accepts Grin's
// published Cuckatoo32 vector V1_32 exactly under that reading (its 42 u-endpoints
// form 21 pairs {x, x^1}). "The other slot of the same node" is therefore literally
// `w ^ 1`, and keeping both slots occupied is the only rule under which a cycle
// survives trimming - the partner slot of a cycle edge is occupied by the next
// edge of the same cycle, so no cycle edge is ever killed.
//
// The earlier CH-7 revision replaced this with cuckoo's `twice_set` degree >= 2
// counter. That is right for Cuckoo Cycle (a single node space, where the node IS
// the raw value) but wrong for Cuckatoo: two edges at the SAME raw value say
// nothing about the partner slot. Measured at EDGEBITS=20/PROOFSIZE=6, 16 trims:
// the degree rule leaves ~3900 edges but only 1 node in partition 0 (0 in
// partition 1) with both slots occupied, so no verify-valid cycle can exist in it
// and raw_cycles is 0 by construction. With the partner-slot rule the same graphs
// leave ~3600 edges with ~3400 two-slot nodes and yield verified 6-cycles.
__device__ __forceinline__ void slot_seen_set(u32* seen, word_t w) {
    atomicOr(&seen[w >> 5], 1u << (w & 31));
}
__device__ __forceinline__ bool slot_seen_test(const u32* seen, word_t w) {
    return ((seen[w >> 5] >> (w & 31)) & 1u) != 0;
}

__device__ __forceinline__ bool safe_shift32(u32& v, u32 ffs) {
    if (ffs >= 32) { v = 0; return true; }
    v >>= ffs;
    return false;
}

// ---------------------------------------------------------------------------
// Kernels
// ---------------------------------------------------------------------------

__global__ void count_node_deg(siphash_keys sipkeys, const u32* alive, u32* nonleaf,
                               u32 uorv, u32 part) {
    const u64 nthreads = (u64)blockDim.x * gridDim.x;
    const u64 id = (u64)blockIdx.x * blockDim.x + threadIdx.x;
    for (u64 block = id * 32; block < NEDGES; block += nthreads * 32) {
        u32 alive32 = alive_block(alive, block);
        u64 nonce = block - 1;
        while (alive32) {
            const u32 ffs = (u32)__ffs(alive32);
            nonce += ffs;
            safe_shift32(alive32, ffs);
            const word_t u = (word_t)dipnode(sipkeys, nonce, uorv);
            if (in_partition(u, part)) {
                slot_seen_set(nonleaf, (word_t)(u & kNonPartMask));
            }
        }
    }
}

__global__ void kill_leaf_edges(siphash_keys sipkeys, u32* alive, const u32* nonleaf,
                                u32 uorv, u32 part) {
    const u64 nthreads = (u64)blockDim.x * gridDim.x;
    const u64 id = (u64)blockIdx.x * blockDim.x + threadIdx.x;
    for (u64 block = id * 32; block < NEDGES; block += nthreads * 32) {
        u32 alive32 = alive_block(alive, block);
        u64 nonce = block - 1;
        while (alive32) {
            const u32 ffs = (u32)__ffs(alive32);
            nonce += ffs;
            safe_shift32(alive32, ffs);
            const word_t u = (word_t)dipnode(sipkeys, nonce, uorv);
            if (in_partition(u, part) &&
                !slot_seen_test(nonleaf, (word_t)((u & kNonPartMask) ^ 1))) {
                alive_kill(alive, nonce);
            }
        }
    }
}

__global__ void dipnode_probe_kernel(siphash_keys keys, u32 count, u32* out) {
    const u32 i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    out[i] = (u32)dipnode(keys, (u64)i, 0);
    out[count + i] = (u32)dipnode(keys, (u64)i, 1);
}

// One-off: copy the alive edges out of the inverted bitmap into a dense array.
// Per-element atomicAdd is fine here because this runs once; the hot path is the per-round
// compaction below, which aggregates per warp instead.
__global__ void compact_alive_kernel(const u32* alive, u32* edges, u32* count) {
    const u64 nthreads = (u64)blockDim.x * gridDim.x;
    const u64 id = (u64)blockIdx.x * blockDim.x + threadIdx.x;
    for (u64 block = id * 32; block < NEDGES; block += nthreads * 32) {
        u32 alive32 = alive_block(alive, block);
        u64 nonce = block - 1;
        while (alive32) {
            const u32 ffs = (u32)__ffs(alive32);
            nonce += ffs;
            safe_shift32(alive32, ffs);
            edges[atomicAdd(count, 1u)] = (u32)nonce;
        }
    }
}

// Dense-tail pass 1: same rule as count_node_deg, but the edge list is an array.
__global__ void count_node_deg_dense(siphash_keys sipkeys, const u32* edges, u32 count,
                                     u32* nonleaf, u32 uorv, u32 part) {
    const u32 stride = blockDim.x * gridDim.x;
    for (u32 i = blockIdx.x * blockDim.x + threadIdx.x; i < count; i += stride) {
        const word_t u = (word_t)dipnode(sipkeys, (u64)edges[i], uorv);
        if (in_partition(u, part)) {
            slot_seen_set(nonleaf, (word_t)(u & kNonPartMask));
        }
    }
}

// Dense-tail pass 2: keep an edge whose partner slot is occupied, and compact the survivors
// into `out`. Survivors are appended, so `out` must be a separate buffer from `edges`.
//
// The loop is written with a UNIFORM trip count across the warp on purpose. An earlier
// version used the natural `for (i = tid; i < count; i += stride)` shape and reserved space
// per warp from lane 0: whenever lane 0 had already left the loop while other lanes were
// still running, the reservation never happened and the whole warp wrote through a stale
// base. The symptom was a survivor count that halved every round and reached zero, which
// looked like an over-eager kill rule rather than a lost write. Iterating a fixed number of
// steps with a `valid` predicate keeps the full warp active for every ballot.
__global__ void kill_leaf_edges_dense(siphash_keys sipkeys, const u32* edges, u32 count,
                                      u32* out, u32* outCount, const u32* nonleaf, u32 uorv,
                                      u32 part) {
    const u32 stride = blockDim.x * gridDim.x;
    const u32 start = blockIdx.x * blockDim.x + threadIdx.x;
    const u32 lane = threadIdx.x & 31u;
    const u32 steps = (count + stride - 1u) / stride;
    for (u32 k = 0; k < steps; ++k) {
        const u32 i = start + k * stride;
        const bool valid = i < count;
        u32 nonce = 0u;
        bool keep = false;
        if (valid) {
            nonce = edges[i];
            const word_t u = (word_t)dipnode(sipkeys, (u64)nonce, uorv);
            // Keep an edge when it is outside this partition (the pass does not touch it) or
            // when its partner slot is occupied: the exact complement of the bitmap path's
            // kill condition.
            keep = !in_partition(u, part) ||
                   slot_seen_test(nonleaf, (word_t)((u & kNonPartMask) ^ 1));
        }
        const unsigned mask = __ballot_sync(0xffffffffu, keep);
        const unsigned rank = __popc(mask & ((1u << lane) - 1u));
        u32 base = 0u;
        if (lane == 0u) base = atomicAdd(outCount, (u32)__popc(mask));
        base = __shfl_sync(0xffffffffu, base, 0);
        if (keep) out[base + rank] = nonce;
    }
}

// Rebuild the inverted alive bitmap from the dense survivor list, so the cycle finder can
// keep using the bitmap path unchanged. The bitmap is filled with ones (everything dead)
// and only the survivors are cleared, which costs one pass over the survivors instead of
// one over all 2^32 edges.
__global__ void scatter_alive_kernel(u32* alive, const u32* edges, u32 count) {
    const u32 stride = blockDim.x * gridDim.x;
    for (u32 i = blockIdx.x * blockDim.x + threadIdx.x; i < count; i += stride) {
        atomicAnd(&alive[edges[i] >> 5], ~(1u << (edges[i] & 31)));
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string cuda_err(const char* what, cudaError_t e) {
    return std::string(what) + ": " + cudaGetErrorString(e);
}

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

struct LeanSolver::Impl {
    SolverConfig cfg;

    uint32_t* d_alive = nullptr;
    uint32_t* d_nonleaf = nullptr;
    u64* h_bits = nullptr;          // 512 MiB host copy of the alive bitmap

    // Dense-tail ping-pong buffers and their device-side element counts. Allocated on a
    // best-effort basis: if they do not fit, the solver keeps using the bitmap path, which
    // is correct but slower.
    u32* d_edges_a = nullptr;
    u32* d_edges_b = nullptr;
    u32* d_count_a = nullptr;
    u32* d_count_b = nullptr;
    bool dense_ready = false;
    u32  dense_count = 0;

    CycleFinder finder;

    siphash_keys sipkeys{};
    uint8_t pre_pow[GRIN_PRE_POW_SIZE] = {};
    bool have_pre_pow = false;

    std::atomic<bool> abort_flag{false};
    LastRun last{};

    ~Impl() { release(); }

    void release() {
        if (d_alive) { cudaFree(d_alive); d_alive = nullptr; }
        if (d_nonleaf) { cudaFree(d_nonleaf); d_nonleaf = nullptr; }
        if (d_edges_a) { cudaFree(d_edges_a); d_edges_a = nullptr; }
        if (d_edges_b) { cudaFree(d_edges_b); d_edges_b = nullptr; }
        if (d_count_a) { cudaFree(d_count_a); d_count_a = nullptr; }
        if (d_count_b) { cudaFree(d_count_b); d_count_b = nullptr; }
        dense_ready = false;
        delete[] h_bits; h_bits = nullptr;
    }

    bool allocate(std::string& error) {
        cudaError_t e = cudaSetDevice(cfg.device);
        if (e != cudaSuccess) { error = cuda_err("cudaSetDevice", e); return false; }

        e = cudaMalloc((void**)&d_alive, kEdgeBytes);
        if (e != cudaSuccess) {
            error = cuda_err("cudaMalloc(alive bitmap) - reduce PART_BITS/increase VRAM", e);
            return false;
        }
        e = cudaMalloc((void**)&d_nonleaf, kNodeBytes);
        if (e != cudaSuccess) {
            error = cuda_err("cudaMalloc(nonleaf bitmap)", e);
            return false;
        }

        try {
            h_bits = new u64[NEDGES / 64];
        } catch (const std::bad_alloc&) {
            error = "host allocation failed (need about 1.1 GiB of RAM for the graph)";
            return false;
        }

        // Dense-tail buffers: best effort, 512 MiB in total. Failing to get them is not an
        // error - the solver simply stays on the bitmap path.
        const size_t denseBytes = (size_t)kDenseCapacity * sizeof(u32);
        if (cudaMalloc((void**)&d_edges_a, denseBytes) == cudaSuccess &&
            cudaMalloc((void**)&d_edges_b, denseBytes) == cudaSuccess &&
            cudaMalloc((void**)&d_count_a, sizeof(u32)) == cudaSuccess &&
            cudaMalloc((void**)&d_count_b, sizeof(u32)) == cudaSuccess) {
            dense_ready = true;
        } else {
            if (d_edges_a) { cudaFree(d_edges_a); d_edges_a = nullptr; }
            if (d_edges_b) { cudaFree(d_edges_b); d_edges_b = nullptr; }
            if (d_count_a) { cudaFree(d_count_a); d_count_a = nullptr; }
            if (d_count_b) { cudaFree(d_count_b); d_count_b = nullptr; }
            cudaGetLastError();
        }
        return true;
    }

    // Copy the alive edges out of the inverted bitmap into the dense buffer, once.
    bool densify(std::string& error) {
        dense_count = 0;
        cudaError_t e = cudaMemset(d_count_a, 0, sizeof(u32));
        if (e != cudaSuccess) { error = cuda_err("cudaMemset(dense count)", e); return false; }
        compact_alive_kernel<<<cfg.blocks, cfg.tpb>>>(d_alive, d_edges_a, d_count_a);
        e = cudaGetLastError();
        if (e != cudaSuccess) { error = cuda_err("compact_alive_kernel launch", e); return false; }
        e = cudaMemcpy(&dense_count, d_count_a, sizeof(u32), cudaMemcpyDeviceToHost);
        if (e != cudaSuccess) { error = cuda_err("dense count copy", e); return false; }
        return true;
    }

    // Run the edge-trimming rounds. Returns false on abort or CUDA error.
    bool trim(std::string& error) {
        cudaError_t e = cudaMemset(d_alive, 0, kEdgeBytes);
        if (e != cudaSuccess) { error = cuda_err("cudaMemset(alive)", e); return false; }

        const u32 totalRounds = cfg.ntrims;

        // Dense-tail state, carried across rounds.
        bool dense = false;
        u32* edges = nullptr;
        u32* edgesOut = nullptr;
        u32* cnt = nullptr;
        u32* cntOut = nullptr;
        u32 count = 0;

        for (u32 round = 0; round < totalRounds; ++round) {
            if (abort_flag.load(std::memory_order_relaxed)) return false;

            // Switch to the dense survivor list once, when the graph has become sparse.
            // If anything about that fails, stay on the bitmap path for the rest of the
            // rounds: slower, but always correct.
            if (!dense && dense_ready && round == kDenseAfterRound) {
                if (densify(error)) {
                    if (std::getenv("GRINFORGE_TRIM_DEBUG") != nullptr) {
                        std::fprintf(stderr, "[trim-debug] switched at round=%u edges=%u\n", round,
                                     dense_count);
                    }
                }
                if (dense_count > 0 && dense_count <= kDenseCapacity) {
                    dense = true;
                    count = dense_count;
                    edges = d_edges_a;
                    edgesOut = d_edges_b;
                    cnt = d_count_a;
                    cntOut = d_count_b;
                }
            }

            for (u32 part = 0; part <= kPartMask; ++part) {
                // The nonleaf bitmap is per-round state. Clearing 512 MiB per round costs
                // on the order of 0.15 s of a 16.9 s C32 graph, so it is NOT the dominant
                // cost - an earlier comment here claimed it was, without measuring it.
                // Measured ablations (docs/performance-notes.md section 7) show that the
                // atomics and the hash are not the bottleneck either, that the random
                // bitmap read is about 30% of a dense round, and that the rest is not yet
                // explained by a per-edge model.
                e = cudaMemset(d_nonleaf, 0, kNodeBytes);
                if (e != cudaSuccess) { error = cuda_err("cudaMemset(nonleaf)", e); return false; }

                if (!dense) {
                    count_node_deg<<<cfg.blocks, cfg.tpb>>>(sipkeys, d_alive, d_nonleaf,
                                                            round & 1u, part);
                    e = cudaGetLastError();
                    if (e != cudaSuccess) { error = cuda_err("count_node_deg launch", e); return false; }

                    kill_leaf_edges<<<cfg.blocks, cfg.tpb>>>(sipkeys, d_alive, d_nonleaf,
                                                             round & 1u, part);
                    e = cudaGetLastError();
                    if (e != cudaSuccess) { error = cuda_err("kill_leaf_edges launch", e); return false; }
                } else {
                    count_node_deg_dense<<<cfg.blocks, cfg.tpb>>>(sipkeys, edges, count, d_nonleaf,
                                                                  round & 1u, part);
                    e = cudaGetLastError();
                    if (e != cudaSuccess) { error = cuda_err("count_node_deg_dense launch", e); return false; }

                    e = cudaMemset(cntOut, 0, sizeof(u32));
                    if (e != cudaSuccess) { error = cuda_err("cudaMemset(dense count)", e); return false; }

                    kill_leaf_edges_dense<<<cfg.blocks, cfg.tpb>>>(sipkeys, edges, count, edgesOut,
                                                                   cntOut, d_nonleaf, round & 1u, part);
                    e = cudaGetLastError();
                    if (e != cudaSuccess) { error = cuda_err("kill_leaf_edges_dense launch", e); return false; }

                    // The next launch needs the survivor count on the host. One copy per pass
                    // is cheap next to the passes themselves.
                    e = cudaMemcpy(&count, cntOut, sizeof(u32), cudaMemcpyDeviceToHost);
                    if (e != cudaSuccess) { error = cuda_err("dense count copy", e); return false; }
                    if (std::getenv("GRINFORGE_TRIM_DEBUG") != nullptr) {
                        std::fprintf(stderr, "[trim-debug] round=%u part=%u survivors=%u\n",
                                     round, part, count);
                    }

                    u32* tmp_edges = edges; edges = edgesOut; edgesOut = tmp_edges;
                    u32* tmp_cnt = cnt; cnt = cntOut; cntOut = tmp_cnt;
                }

                if (abort_flag.load(std::memory_order_relaxed)) return false;
            }
        }

        // Hand the survivors back to the inverted bitmap, so the cycle finder keeps using
        // the same representation it always has. Filling with ones means "everything dead";
        // only the survivors are then cleared, which costs one pass over the survivors
        // rather than one over all 2^32 edges.
        if (dense) {
            e = cudaMemset(d_alive, 0xFF, kEdgeBytes);
            if (e != cudaSuccess) { error = cuda_err("cudaMemset(alive, dead)", e); return false; }
            if (count > 0) {
                scatter_alive_kernel<<<cfg.blocks, cfg.tpb>>>(d_alive, edges, count);
                e = cudaGetLastError();
                if (e != cudaSuccess) { error = cuda_err("scatter_alive_kernel launch", e); return false; }
            }
        }

        e = cudaDeviceSynchronize();
        if (e != cudaSuccess) { error = cuda_err("cudaDeviceSynchronize", e); return false; }
        return true;
    }

    SolveStatus solve(uint64_t nonce, FoundSolution* out, int max_out, int& found,
                      std::string& error) {
        found = 0;
        if (!have_pre_pow) return SolveStatus::NotConfigured;
        abort_flag.store(false, std::memory_order_relaxed);

        const double t_start = now_ms();

        // 1. GRIN header -> siphash keys (see docs/stratum-protocol.md).
        uint8_t header[GRIN_HEADER_LEN];
        grin_build_header(pre_pow, GRIN_PRE_POW_SIZE, nonce, header);
        grin_setheader(header, (u32)GRIN_HEADER_LEN, &sipkeys);

        // 2. Edge trimming.
        if (!trim(error)) {
            return abort_flag.load(std::memory_order_relaxed) ? SolveStatus::Aborted
                                                             : SolveStatus::CudaError;
        }
        const double t_trim = now_ms();

        // 3. Copy the alive bitmap back and hand it to the cycle finder.
        cudaError_t e = cudaMemcpy(h_bits, d_alive, kEdgeBytes, cudaMemcpyDeviceToHost);
        if (e != cudaSuccess) { error = cuda_err("cudaMemcpy(alive)", e); return SolveStatus::CudaError; }

        last.verify_failures = 0;
        last.raw_cycles = 0;
        last.search_capped = false;

        CycleFinder::Params fp;
        fp.max_solutions = (max_out > 0 && max_out <= 8) ? max_out : 8;
        fp.max_steps = cfg.max_search_steps;

        // The finder reports a clean failure when the graph does not fit its node
        // table, instead of the silent corruption the old compressor produced.
        if (!finder.build(h_bits, (uint64_t)NEDGES, sipkeys, fp, error)) {
            last.edges_after_trim = finder.stats().alive_edges;
            last.trim_ms = t_trim - t_start;
            last.find_cycles_ms = 0.0;
            last.total_ms = now_ms() - t_start;
            return SolveStatus::Overloaded;
        }
        last.edges_after_trim = finder.stats().alive_edges;

        // 4. Search for cycles of exactly PROOFSIZE edges.
        RawSolution raw[8];
        bool capped = false;
        const int nsol = finder.search(raw, capped);
        const double t_search = now_ms();

        last.raw_cycles = (uint64_t)nsol;
        last.search_capped = capped;
        last.search_steps = finder.stats().steps;
        print_log("  finder: edges=%llu nodes=%llu full_slot_nodes=%llu steps=%llu cap=%d\n",
                  (unsigned long long)finder.stats().alive_edges,
                  (unsigned long long)finder.stats().nodes,
                  (unsigned long long)finder.stats().full_nodes,
                  (unsigned long long)finder.stats().steps, capped ? 1 : 0);

        for (int s = 0; s < nsol && found < max_out; ++s) {
            FoundSolution fs;
            fs.nonce = nonce;
            for (u32 i = 0; i < PROOFSIZE; ++i) fs.proof[i] = raw[s].edges[i];

            // Independent verification: never report a cycle we cannot verify.
            if (grin_verify((const word_t*)raw[s].edges, &sipkeys) != POW_OK) {
                ++last.verify_failures;
                continue;
            }

            fs.cyclehash_leading_zeros = grin_cyclehash_leading_zeros(fs.proof);
            out[found++] = fs;
        }

        last.trim_ms = t_trim - t_start;
        last.find_cycles_ms = t_search - t_trim;
        last.total_ms = now_ms() - t_start;
        return SolveStatus::Ok;
    }
};

// ---------------------------------------------------------------------------
// LeanSolver
// ---------------------------------------------------------------------------

LeanSolver::LeanSolver(const SolverConfig& cfg) : impl_(new Impl()) {
    impl_->cfg = cfg;
    std::string error;
    if (!impl_->allocate(error)) {
        // Keep the object unusable but alive; solve() will report CudaError.
        print_log("solver allocation failed: %s\n", error.c_str());
    }
}

LeanSolver::~LeanSolver() { delete impl_; }

bool LeanSolver::set_pre_pow(const uint8_t* data, size_t len, std::string& error) {
    if (len != GRIN_PRE_POW_SIZE) {
        error = "pre_pow must be exactly 238 bytes, got " + std::to_string(len);
        return false;
    }
    std::memcpy(impl_->pre_pow, data, GRIN_PRE_POW_SIZE);
    impl_->have_pre_pow = true;
    return true;
}

SolveStatus LeanSolver::solve(uint64_t nonce, FoundSolution* out, int max_out, int& found,
                              std::string& error) {
    if (impl_->d_alive == nullptr || impl_->h_bits == nullptr) {
        error = "solver was not allocated (see the allocation error above)";
        return SolveStatus::CudaError;
    }
    return impl_->solve(nonce, out, max_out, found, error);
}

void LeanSolver::request_abort() { impl_->abort_flag.store(true, std::memory_order_relaxed); }
void LeanSolver::clear_abort()   { impl_->abort_flag.store(false, std::memory_order_relaxed); }

LeanSolver::LastRun LeanSolver::last_run() const { return impl_->last; }

void LeanSolver::derive_keys(const uint8_t* pre_pow, size_t pre_pow_len, uint64_t nonce,
                             uint8_t out_digest32[32]) {
    uint8_t header[GRIN_HEADER_LEN];
    grin_build_header(pre_pow, pre_pow_len, nonce, header);
    blake2b(out_digest32, 32, header, GRIN_HEADER_LEN, nullptr, 0);
}

bool LeanSolver::device_probe(const uint8_t* pre_pow, size_t pre_pow_len, uint64_t header_nonce,
                              uint32_t count, uint32_t* out_uv, std::string& error) {
    if (pre_pow_len != GRIN_PRE_POW_SIZE) {
        error = "pre_pow must be exactly 238 bytes";
        return false;
    }
    if (count == 0 || out_uv == nullptr) {
        error = "device_probe needs a positive count and an output buffer";
        return false;
    }

    uint8_t header[GRIN_HEADER_LEN];
    grin_build_header(pre_pow, pre_pow_len, header_nonce, header);
    siphash_keys keys;
    grin_setheader(header, (u32)GRIN_HEADER_LEN, &keys);

    const size_t bytes = sizeof(uint32_t) * 2 * (size_t)count;
    uint32_t* d_out = nullptr;
    cudaError_t e = cudaMalloc((void**)&d_out, bytes);
    if (e != cudaSuccess) { error = cuda_err("cudaMalloc(probe)", e); return false; }

    dipnode_probe_kernel<<<(count + 255) / 256, 256>>>(keys, count, d_out);
    e = cudaGetLastError();
    if (e != cudaSuccess) {
        error = cuda_err("dipnode_probe_kernel launch", e);
        cudaFree(d_out);
        return false;
    }
    e = cudaMemcpy(out_uv, d_out, bytes, cudaMemcpyDeviceToHost);
    if (e != cudaSuccess) {
        error = cuda_err("cudaMemcpy(probe)", e);
        cudaFree(d_out);
        return false;
    }
    cudaFree(d_out);
    return true;
}

uint64_t LeanSolver::device_bytes_for(const SolverConfig&) {
    return kEdgeBytes + kNodeBytes;
}

int LeanSolver::device_count() {
    int n = 0;
    if (cudaGetDeviceCount(&n) != cudaSuccess) return 0;
    return n;
}

bool LeanSolver::device_info(int index, std::string& name, uint64_t& vram_bytes,
                             int& cc_major, int& cc_minor) {
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, index) != cudaSuccess) return false;
    name = prop.name;
    vram_bytes = (uint64_t)prop.totalGlobalMem;
    cc_major = prop.major;
    cc_minor = prop.minor;
    return true;
}

bool LeanSolver::device_memory(int index, uint64_t& free_bytes, uint64_t& total_bytes) {
    if (cudaSetDevice(index) != cudaSuccess) return false;
    size_t freeB = 0, totalB = 0;
    if (cudaMemGetInfo(&freeB, &totalB) != cudaSuccess) return false;
    free_bytes = freeB;
    total_bytes = totalB;
    return true;
}

} // namespace grin
