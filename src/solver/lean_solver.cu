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

#include <cuda_runtime.h>

#include <atomic>
#include <chrono>
#include <cstdio>
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

// CH-7 (see the helper section below): the leaf test needs a degree >= 2 counter,
// which costs two bits per node.
const u64 kNodeBits = 2;

const u64 kEdgeBytes = NEDGES / 8;                       // 512 MiB at C32
const u64 kNodeBytes = ((NEDGES >> PART_BITS) * kNodeBits) / 8;   // 1 GiB at PART_BITS=0
const u32 kPartMask = (1u << PART_BITS) - 1u;
const u32 kNonPartBits = EDGEBITS - PART_BITS;

// CH-1: 64-bit mask computation; ~0 when there is no partitioning.
const word_t kNonPartMask =
    (PART_BITS == 0) ? (word_t)~0ULL : (word_t)(((u64)1 << kNonPartBits) - 1ULL);

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

// CH-7 (bug fix): the "nonleaf" bitmap must answer "does this node have at least
// TWO alive edges?", because edge trimming removes edges whose endpoint is a leaf
// (degree 1). Upstream `src/cuckatoo/lean.cu` instead uses a SINGLE bit per node
// and tests `nonleaf.test((u & NONPART_MASK) ^ 1)`:
//   * a single bit can only express "degree >= 1", so it cannot detect a leaf;
//   * `^ 1` tests a DIFFERENT (effectively random) node than the one that was set,
//     which turns the kill into an unbiased ~37% random edge deletion each round.
// Random deletion destroys long cycles, which is exactly what we measured:
// 0 solutions in 24 C32 graphs and 0 in 40 C29 graphs, while short cycles
// (2, 22) still appeared.
//
// The fix is the structure Tromp himself uses in the working `src/cuckoo/lean.cu`
// (`twice_set`): two bits per node, bit 0 = "has an alive edge", bit 1 = "has at
// least two". `nonleaf_set2` is `twice_set::set` and `nonleaf_deg2` is
// `twice_set::test`, so the leaf test becomes `!nonleaf_deg2(u)`, with no `^ 1`.
// Cost: 2 bits per node == 1 GiB at C32 instead of 512 MiB.
__device__ __forceinline__ void nonleaf_set2(u32* nonleaf, word_t u) {
    const word_t idx = u >> 4;                  // 16 nodes per 32-bit word
    const u32 bit = 1u << (2 * (u & 15));
    const u32 old = atomicOr(&nonleaf[idx], bit);
    const u32 bit2 = bit << 1;
    if ((old & (bit2 | bit)) == bit) atomicOr(&nonleaf[idx], bit2);
}
__device__ __forceinline__ bool nonleaf_deg2(const u32* nonleaf, word_t u) {
    return ((nonleaf[u >> 4] >> (2 * (u & 15))) & 2u) != 0;
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
                nonleaf_set2(nonleaf, (word_t)(u & kNonPartMask));
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
            if (in_partition(u, part) && !nonleaf_deg2(nonleaf, (word_t)(u & kNonPartMask))) {
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
        return true;
    }

    // Run the edge-trimming rounds. Returns false on abort or CUDA error.
    bool trim(std::string& error) {
        cudaError_t e = cudaMemset(d_alive, 0, kEdgeBytes);
        if (e != cudaSuccess) { error = cuda_err("cudaMemset(alive)", e); return false; }

        const u32 totalRounds = cfg.ntrims;
        for (u32 round = 0; round < totalRounds; ++round) {
            if (abort_flag.load(std::memory_order_relaxed)) return false;
            for (u32 part = 0; part <= kPartMask; ++part) {
                // The nonleaf bitmap is per-round state. Clearing 512 MiB per round
                // is the single largest cost in this solver; measured and optimised
                // separately (see docs/performance-notes.md).
                e = cudaMemset(d_nonleaf, 0, kNodeBytes);
                if (e != cudaSuccess) { error = cuda_err("cudaMemset(nonleaf)", e); return false; }

                count_node_deg<<<cfg.blocks, cfg.tpb>>>(sipkeys, d_alive, d_nonleaf,
                                                        round & 1u, part);
                e = cudaGetLastError();
                if (e != cudaSuccess) { error = cuda_err("count_node_deg launch", e); return false; }

                kill_leaf_edges<<<cfg.blocks, cfg.tpb>>>(sipkeys, d_alive, d_nonleaf,
                                                         round & 1u, part);
                e = cudaGetLastError();
                if (e != cudaSuccess) { error = cuda_err("kill_leaf_edges launch", e); return false; }

                if (abort_flag.load(std::memory_order_relaxed)) return false;
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
