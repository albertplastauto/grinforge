// Cycle search for the trimmed Cuckatoo graph — the replacement for the
// `graph.hpp` + `compress.hpp` + `add_compress_edge` stage.
//
// WHY THIS EXISTS (see docs/root-cause.md)
//   `graph.hpp::add_edge` only closes a cycle when `adjlist[u ^ 1]` is live, i.e. it
//   assumes a node's partner is the adjacent id (2i, 2i+1). In the Cuckatoo model
//   `sipnode()` encodes the partition in the hash INPUT (`siphash24(2*edge + uorv)`)
//   and returns a plain masked hash, so no such partner exists; and
//   `compress.hpp::compress` derives the compressed id's low bit from the raw hash
//   parity, so `compressed_u ^ 1` is almost never a live node. Measured result:
//   `raw_cycles == 0` and not one "cycle found" line, even at EDGEBITS=20 with
//   PROOFSIZE=6 where 6-cycles are abundant.
//
// WHAT THIS DOES INSTEAD
//   Sober, explicit bipartite graph:
//     * every edge e has u = sipnode(keys, e, 0) in partition 0 and
//       v = sipnode(keys, e, 1) in partition 1;
//     * raw node ids are interned into dense per-partition indices with an open
//       addressing hash table, so nothing is ever derived from a hash bit;
//     * adjacency is a plain CSR layout over the union of both partition ranges;
//     * a solution is a simple cycle of exactly PROOFSIZE edges, found by a bounded
//       depth-first walk that starts from each edge in ascending order and refuses
//       to use an edge with a smaller index, which reports every cycle exactly once.
//
// The search is explicitly bounded by `max_steps`; a graph can never stall the
// miner (an unbounded DFS previously hung a benchmark for over ten minutes).
//
// Every solution is still passed through grin::grin_verify by the caller, which is
// GRIN's actual acceptance rule, so a bug here cannot produce a bogus share.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "grin_params.hpp"

namespace grin {

// A found cycle: PROOFSIZE edge (nonce) indices, ascending, as GRIN requires.
struct RawSolution {
    uint32_t edges[PROOFSIZE] = {};
};

class CycleFinder {
public:
    struct Params {
        int      max_solutions = 8;
        uint64_t max_steps = 400ull * 1000ull * 1000ull;
    };

    CycleFinder() = default;
    CycleFinder(const CycleFinder&) = delete;
    CycleFinder& operator=(const CycleFinder&) = delete;

    // `inverted_alive_bitmap` is the raw device bitmap the trimmer produced: a SET
    // bit means the edge is DEAD (the "alive" set is inverted upstream). Indexed by
    // 64-edge words, `nedges_total` bits in total.
    bool build(const uint64_t* inverted_alive_bitmap, uint64_t nedges_total,
               const siphash_keys& keys, const Params& params, std::string& error);

    // Returns the number of solutions written to `out` (at most params.max_solutions).
    int search(RawSolution* out, bool& step_cap_hit);

    struct Stats {
        uint64_t alive_edges = 0;
        uint64_t nodes = 0;
        uint64_t steps = 0;
        uint64_t step_cap_hits = 0;
        uint64_t build_ms = 0;
        uint64_t search_ms = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    // Open-addressing table mapping a raw 32-bit node id to a dense index.
    struct InternTable {
        std::vector<uint32_t> keys;    // 0xFFFFFFFF = empty
        std::vector<uint32_t> values;
        uint32_t mask = 0;
        uint32_t count = 0;

        void init(uint32_t capacity_pow2) {
            keys.assign(capacity_pow2, 0xFFFFFFFFu);
            values.assign(capacity_pow2, 0);
            mask = capacity_pow2 - 1;
            count = 0;
        }
        // Returns the dense index, allocating a new one on first sight.
        uint32_t intern(uint32_t raw) {
            uint32_t slot = (raw * 2654435761u) & mask;
            for (;;) {
                if (keys[slot] == raw) return values[slot];
                if (keys[slot] == 0xFFFFFFFFu) {
                    keys[slot] = raw;
                    values[slot] = count++;
                    return values[slot];
                }
                slot = (slot + 1) & mask;
            }
        }
    };

    void dfs(uint32_t node, uint32_t start_u, uint32_t used, uint32_t min_edge,
             uint32_t* path, bool& cap_hit);

    Params params_{};
    Stats stats_{};

    std::vector<uint32_t> eu_;      // dense partition-0 node of each alive edge
    std::vector<uint32_t> ev_;      // dense partition-1 node (offset by n0_)
    std::vector<uint32_t> enonce_;  // original edge index (nonce)

    uint32_t n0_ = 0;               // number of partition-0 nodes
    uint32_t n1_ = 0;
    uint32_t n_nodes_ = 0;

    // CSR adjacency over the union of both partition ranges.
    std::vector<uint32_t> adj_begin_;   // size n_nodes_ + 1
    std::vector<uint32_t> adj_edges_;   // size 2 * alive edges

    std::vector<uint8_t> visited_;
    std::vector<uint32_t> path_;
    int solutions_found_ = 0;
    RawSolution* out_ = nullptr;
};

inline bool CycleFinder::build(const uint64_t* inverted_alive_bitmap, uint64_t nedges_total,
                               const siphash_keys& keys, const Params& params,
                               std::string& error) {
    params_ = params;
    stats_ = Stats{};

    // ---- 1. collect the surviving edges, ascending by nonce -------------------
    // The bitmap is inverted: bit clear == alive.
    std::vector<uint32_t> alive_nonce;
    alive_nonce.reserve(1u << 20);
    for (uint64_t word = 0; word * 64 < nedges_total; ++word) {
        uint64_t alive64 = ~inverted_alive_bitmap[word];
        while (alive64) {
            unsigned long bit = 0;
#if defined(_MSC_VER)
            if (_BitScanForward64(&bit, alive64) == 0) break;
#else
            bit = (unsigned long)__builtin_ctzll(alive64);
#endif
            const uint64_t nonce = word * 64 + bit;
            if (nonce < nedges_total) alive_nonce.push_back((uint32_t)nonce);
            alive64 &= alive64 - 1;   // clear the lowest set bit
        }
    }
    const uint64_t n_alive = alive_nonce.size();
    stats_.alive_edges = n_alive;
    if (n_alive == 0) {
        error = "no surviving edges";
        return false;
    }

    // ---- 2. intern raw node ids into dense per-partition indices --------------
    // Capacity: at most 2*n_alive distinct nodes overall; keep the table at least
    // 2x the node count so linear probing stays cheap, and round up to a power of 2.
    uint64_t cap = 16;
    while (cap < 4 * n_alive + 16) cap <<= 1;
    if (cap > (1ull << 32)) {
        error = "graph too large for the cycle finder's node table";
        return false;
    }

    InternTable t0, t1;
    t0.init((uint32_t)cap);
    t1.init((uint32_t)cap);

    eu_.resize(n_alive);
    ev_.resize(n_alive);
    enonce_.resize(n_alive);
    for (uint64_t i = 0; i < n_alive; ++i) {
        const uint32_t e = alive_nonce[i];
        const uint32_t u = (uint32_t)sipnode(const_cast<siphash_keys*>(&keys), (word_t)e, 0);
        const uint32_t v = (uint32_t)sipnode(const_cast<siphash_keys*>(&keys), (word_t)e, 1);
        eu_[i] = t0.intern(u);
        ev_[i] = t1.intern(v);
        enonce_[i] = e;
    }
    n0_ = t0.count;
    n1_ = t1.count;
    n_nodes_ = n0_ + n1_;
    stats_.nodes = n_nodes_;
    if (n_nodes_ == 0) {
        error = "no nodes after interning";
        return false;
    }

    // Partition-1 dense ids live above partition-0 ids so one CSR serves both.
    for (uint64_t i = 0; i < n_alive; ++i) ev_[i] += n0_;

    // ---- 3. CSR adjacency ----------------------------------------------------
    adj_begin_.assign((size_t)n_nodes_ + 1, 0);
    for (uint64_t i = 0; i < n_alive; ++i) {
        ++adj_begin_[eu_[i] + 1];
        ++adj_begin_[ev_[i] + 1];
    }
    for (uint32_t n = 0; n < n_nodes_; ++n) adj_begin_[n + 1] += adj_begin_[n];

    adj_edges_.resize((size_t)2 * n_alive);
    {
        std::vector<uint32_t> cursor(adj_begin_.begin(), adj_begin_.end() - 1);
        for (uint64_t i = 0; i < n_alive; ++i) {
            adj_edges_[cursor[eu_[i]]++] = (uint32_t)i;
            adj_edges_[cursor[ev_[i]]++] = (uint32_t)i;
        }
    }

    visited_.assign(n_nodes_, 0);
    path_.assign(PROOFSIZE, 0);
    solutions_found_ = 0;
    return true;
}

inline void CycleFinder::dfs(uint32_t node, uint32_t start_u, uint32_t used, uint32_t min_edge,
                             uint32_t* path, bool& cap_hit) {
    if (solutions_found_ >= params_.max_solutions) return;
    if (stats_.steps >= params_.max_steps) { cap_hit = true; return; }
    ++stats_.steps;

    const uint32_t begin = adj_begin_[node];
    const uint32_t end = adj_begin_[node + 1];
    for (uint32_t a = begin; a < end; ++a) {
        const uint32_t e = adj_edges_[a];
        if (e <= min_edge) continue;          // report each cycle once, from its lowest edge

        // The other endpoint of edge e.
        const uint32_t other = (eu_[e] == node) ? ev_[e] : eu_[e];

        if (used + 1 == PROOFSIZE) {
            // Closing the cycle: we must land back on the starting partition-0 node.
            if (other != start_u) continue;
            uint32_t tmp[PROOFSIZE];
            for (uint32_t i = 0; i < used; ++i) tmp[i] = path[i];
            tmp[used] = e;
            // Sort ascending: GRIN requires it and grin_verify enforces it.
            for (uint32_t i = 0; i + 1 < PROOFSIZE; ++i) {
                for (uint32_t j = i + 1; j < PROOFSIZE; ++j) {
                    if (tmp[j] < tmp[i]) { const uint32_t s = tmp[i]; tmp[i] = tmp[j]; tmp[j] = s; }
                }
            }
            for (uint32_t i = 0; i + 1 < PROOFSIZE; ++i) {
                if (tmp[i] == tmp[i + 1]) return;   // repeated edge: not a simple cycle
            }
            for (uint32_t i = 0; i < PROOFSIZE; ++i) out_[solutions_found_].edges[i] = enonce_[tmp[i]];
            ++solutions_found_;
            if (solutions_found_ >= params_.max_solutions) return;
            continue;
        }

        if (visited_[other]) continue;
        visited_[other] = 1;
        path[used] = e;
        dfs(other, start_u, used + 1, min_edge, path, cap_hit);
        visited_[other] = 0;
        if (cap_hit || solutions_found_ >= params_.max_solutions) return;
    }
}

inline int CycleFinder::search(RawSolution* out, bool& step_cap_hit) {
    out_ = out;
    step_cap_hit = false;
    solutions_found_ = 0;
    if (out_ == nullptr || adj_begin_.empty()) return 0;

    const uint64_t n_alive = enonce_.size();
    for (uint64_t i0 = 0; i0 < n_alive; ++i0) {
        if (solutions_found_ >= params_.max_solutions) break;
        if (stats_.steps >= params_.max_steps) { step_cap_hit = true; break; }

        const uint32_t a = eu_[i0];      // start node, partition 0
        const uint32_t b = ev_[i0];      // first step, partition 1
        if (a == b) continue;            // degenerate self-loop
        if (visited_[a] || visited_[b]) continue;

        visited_[a] = 1;
        visited_[b] = 1;
        path_[0] = (uint32_t)i0;
        dfs(b, a, 1, (uint32_t)i0, path_.data(), step_cap_hit);
        visited_[a] = 0;
        visited_[b] = 0;

        if (step_cap_hit) { ++stats_.step_cap_hits; break; }
    }
    return solutions_found_;
}

} // namespace grin
