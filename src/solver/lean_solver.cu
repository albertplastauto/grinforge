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
#include "graph.hpp"
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

#ifndef IDXSHIFT
#define IDXSHIFT (PART_BITS + 8)
#endif

#define MAXEDGES (NEDGES >> IDXSHIFT)

const u64 kEdgeBytes = NEDGES / 8;                       // 512 MiB at C32
const u64 kNodeBytes = (NEDGES >> PART_BITS) / 8;        // 512 MiB at PART_BITS=0
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

__device__ __forceinline__ void nonleaf_set(u32* nonleaf, word_t n) {
    atomicOr(&nonleaf[n >> 5], 1u << (n & 31));
}
__device__ __forceinline__ bool nonleaf_test(const u32* nonleaf, word_t n) {
    return ((nonleaf[n >> 5] >> (n & 31)) & 1u) != 0;
}

// CH-2: explicit shift-count handling. PTX clamps shifts >= width to width, which
// is what upstream silently relies on; this makes it defined behaviour.
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
                nonleaf_set(nonleaf, (word_t)(u & kNonPartMask));
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
            if (in_partition(u, part) && !nonleaf_test(nonleaf, (word_t)((u & kNonPartMask) ^ 1))) {
                alive_kill(alive, nonce);
            }
        }
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

    graph<word_t>* cg = nullptr;
    proof* sols = nullptr;          // MAXSOLS solutions of PROOFSIZE edge indices

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
        delete cg; cg = nullptr;
        delete[] sols; sols = nullptr;
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
            cg = new graph<word_t>(MAXEDGES, MAXEDGES, MAXSOLS, IDXSHIFT);
            sols = new proof[MAXSOLS];
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

    // Build the compressed graph from the surviving edges.
    void findcycles() {
        cg->reset();
        for (u64 block = 0; block < NEDGES; block += 64) {
            u64 alive64 = ~h_bits[block / 64];
            while (alive64) {
                const int ffs = grin_ffs64(alive64);
                const u64 nonce = block + (u64)(ffs - 1);
                alive64 = (ffs == 64) ? 0ULL : (alive64 >> ffs);
                const word_t u = sipnode(&sipkeys, (word_t)nonce, 0);
                const word_t v = sipnode(&sipkeys, (word_t)nonce, 1);
                cg->add_compress_edge(u, v);
            }
        }
    }

    // Map the compressed edge indices back to graph edge indices (nonces).
    void uncompress_solutions() {
        for (u32 s = 0; s < cg->nsols; ++s) {
            u32 j = 0;
            u64 nalive = 0;
            bool done = false;
            for (u64 block = 0; block < NEDGES && !done; block += 64) {
                u64 alive64 = ~h_bits[block / 64];
                while (alive64) {
                    const int ffs = grin_ffs64(alive64);
                    const u64 nonce = block + (u64)(ffs - 1);
                    alive64 = (ffs == 64) ? 0ULL : (alive64 >> ffs);
                    if (nalive++ == (u64)cg->sols[s][j]) {
                        sols[s][j] = (word_t)nonce;
                        if (++j == PROOFSIZE) { done = true; break; }
                    }
                }
            }
        }
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

        // 3. Copy the alive bitmap back and count survivors.
        cudaError_t e = cudaMemcpy(h_bits, d_alive, kEdgeBytes, cudaMemcpyDeviceToHost);
        if (e != cudaSuccess) { error = cuda_err("cudaMemcpy(alive)", e); return SolveStatus::CudaError; }

        u64 nedges = 0;
        const u64 words = NEDGES / 64;
        for (u64 i = 0; i < words; ++i) nedges += (u64)grin_popcount64(~h_bits[i]);
        last.edges_after_trim = nedges;

        // CH-4: upstream exits the process here. A miner must instead treat this
        // attempt as unusable and carry on with the next nonce.
        if (nedges >= (u64)MAXEDGES) {
            last.trim_ms = t_trim - t_start;
            last.find_cycles_ms = 0.0;
            last.total_ms = now_ms() - t_start;
            return SolveStatus::Overloaded;
        }

        // 4. Find cycles in the compressed graph, then uncompress.
        findcycles();
        const double t_cycles = now_ms();
        uncompress_solutions();

        for (u32 s = 0; s < cg->nsols && found < max_out; ++s) {
            FoundSolution fs;
            fs.nonce = nonce;
            for (u32 i = 0; i < PROOFSIZE; ++i) fs.proof[i] = (uint32_t)sols[s][i];

            // Independent verification: never report a cycle we cannot verify.
            if (grin_verify((const word_t*)sols[s], &sipkeys) != POW_OK) continue;

            fs.cyclehash_leading_zeros = grin_cyclehash_leading_zeros(fs.proof);
            out[found++] = fs;
        }

        last.trim_ms = t_trim - t_start;
        last.find_cycles_ms = t_cycles - t_trim;
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
