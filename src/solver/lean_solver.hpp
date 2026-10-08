// Public interface of the Cuckatoo32 "lean" GPU solver.
//
// The solver is a port of tromp/cuckoo `src/cuckatoo/lean.cu`
// (Copyright (c) 2013-2020 John Tromp, "The FAIR MINING License") with the
// EDGEBITS=32 correctness fix and a GRIN-specific header/key derivation.
// See docs/stratum-protocol.md and src/solver/grin_params.hpp for provenance.
//
// This header deliberately contains no CUDA types, so the miner's host code and
// the standalone benchmark can use it without nvcc.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace grin {

struct SolverConfig {
    int      device = 0;
    // Edge-trimming rounds. Upstream default is 128. Fewer rounds finish sooner
    // but leave more edges for the CPU cycle finder (and can overflow it).
    uint32_t ntrims = 128;
    // Kernel launch geometry. Upstream default is 128 blocks x 128 threads.
    uint32_t blocks = 128;
    uint32_t tpb = 128;
};

struct FoundSolution {
    uint64_t nonce = 0;                       // the PoW nonce that produced the cycle
    uint32_t proof[42] = {};                  // PROOFSIZE ascending edge indices
    uint64_t cyclehash_leading_zeros = 0;     // metric used by the pool's difficulty rule
};

enum class SolveStatus {
    Ok,             // solve() ran; `found` says how many cycles were found
    Aborted,        // request_abort() was called
    Overloaded,     // too many edges survived trimming; this attempt is unusable
    CudaError,      // a CUDA call failed; `error` has the details
    NotConfigured,  // set_pre_pow() has not been called
};

class LeanSolver {
public:
    // ---- device queries (no solver instance required) ----
    static int  device_count();
    static bool device_info(int index, std::string& name, uint64_t& vram_bytes,
                            int& cc_major, int& cc_minor);
    static bool device_memory(int index, uint64_t& free_bytes, uint64_t& total_bytes);

    explicit LeanSolver(const SolverConfig& cfg);
    ~LeanSolver();
    LeanSolver(const LeanSolver&) = delete;
    LeanSolver& operator=(const LeanSolver&) = delete;

    // The 238-byte `pre_pow` prefix delivered by the pool.
    bool set_pre_pow(const uint8_t* data, size_t len, std::string& error);

    // Derives siphash keys from BLAKE2b(pre_pow || be64(nonce)) and runs exactly
    // one graph attempt. Writes up to `max_out` solutions into `out`, sets `found`.
    SolveStatus solve(uint64_t nonce, FoundSolution* out, int max_out, int& found,
                      std::string& error);

    // Ask the running solve() to stop at the next kernel boundary. Thread-safe.
    void request_abort();
    void clear_abort();

    struct LastRun {
        double   trim_ms = 0.0;
        double   find_cycles_ms = 0.0;
        double   total_ms = 0.0;
        uint64_t edges_after_trim = 0;
        uint64_t peak_device_bytes = 0;
        // Cycles of length PROOFSIZE closed by the graph builder, BEFORE the
        // uncompress + verify step. raw_cycles > 0 with `found == 0` means the
        // cycle finder works and the edge-index recovery is wrong; raw_cycles == 0
        // means the search itself finds nothing.
        uint64_t raw_cycles = 0;
        uint64_t verify_failures = 0;
    };
    LastRun last_run() const;

    // Diagnostics: the 32-byte BLAKE2b digest (== siphash key material) for a nonce.
    static void derive_keys(const uint8_t* pre_pow, size_t pre_pow_len, uint64_t nonce,
                            uint8_t out_digest32[32]);

    // Diagnostics: compute the edge endpoints for edge indices [0, count) ON THE
    // DEVICE, writing 2*count u32 (all u endpoints, then all v endpoints).
    //
    // Why this exists: the trim kernels generate edges with the device siphash
    // (`dipnode`, which uses a uint2/PTX fast path), while findcycles() rebuilds
    // the graph with the host siphash (`sipnode`). If those two implementations
    // disagreed, the solver would trim one graph and search a different one, and
    // would never find long cycles. This probe makes the comparison possible.
    static bool device_probe(const uint8_t* pre_pow, size_t pre_pow_len, uint64_t header_nonce,
                             uint32_t count, uint32_t* out_uv, std::string& error);
    // Bytes of device memory this configuration will allocate.
    static uint64_t device_bytes_for(const SolverConfig& cfg);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace grin
