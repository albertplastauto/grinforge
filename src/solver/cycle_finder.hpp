// GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
// Copyright (c) 2026 albertplastauto
// SPDX-License-Identifier: MIT
//
// Cycle search for the trimmed Cuckatoo graph — the replacement for the
// `graph.hpp` + `compress.hpp` + `add_compress_edge` stage.
//
// ---------------------------------------------------------------------------
// THE MODEL (established from evidence, not assumed)
// ---------------------------------------------------------------------------
// A Cuckatoo NODE is `sipnode(keys, edge, uorv) >> 1`, and the dropped low bit is
// a two-slot parity that must DIFFER between the two cycle edges meeting at that
// node. Evidence:
//
//   * grin::grin_verify (== Grin's `core/src/pow/cuckatoo.rs::verify_impl`,
//     v5.3.3) matches half-edges by `uvs >> 1` and rejects a match whose two raw
//     values are equal (`POW_DEAD_END`, `uvs[j] == uvs[i]`).
//   * grin_verify accepts Grin's published Cuckatoo32 consensus vector (header
//     [0u8;80] + LE u32 nonce 17, `validate32_vectors`): measured with this very
//     header file, its 42 u-endpoints are 42 DISTINCT values forming exactly
//     21 pairs {x, x^1} - the slot reading above. Cycles that pair equal raw
//     values cannot pass.
//   * Grin's own lean trimmer (`core/src/pow/lean.rs`) and upstream
//     tromp/cuckoo `src/cuckatoo/lean.cu` kill an edge unless its *partner slot*
//     `sipnode ^ 1` is itself present; that is the only trimming rule under which
//     the {x, x^1} pair structure - and therefore every cycle - survives. See the
//     comment on the trim kernels in lean_solver.cu.
//
// ---------------------------------------------------------------------------
// WHY THE OLD STAGE FOUND NOTHING
// ---------------------------------------------------------------------------
// `graph.hpp::add_edge` only enters its DFS when `adjlist[u ^ 1]` is non-empty.
// With `compress.hpp` that `^ 1` does mean "the other slot of the same node"
// (compress keys on `u >> 1`), so the walk itself was the Cuckatoo walk - but the
// CH-7 trim rule (degree >= 2 per raw value) left almost no node with both slots
// occupied, so `adjlist[u ^ 1]` was NIL for nearly every edge and the DFS was
// never entered: `raw_cycles == 0` and not one "cycle found" line. Measured at
// EDGEBITS=20/PROOFSIZE=6, 16 trims: the CH-7 core has ~3900 edges but only 1
// node in partition 0 (0 in partition 1) with both slots occupied, so no
// verify-valid cycle can exist in it at all.
//
// ---------------------------------------------------------------------------
// WHAT THIS DOES INSTEAD
// ---------------------------------------------------------------------------
//   * every alive edge e has u = sipnode(keys, e, 0) in partition 0 and
//     v = sipnode(keys, e, 1) in partition 1;
//   * the node key is `raw >> 1`; the low bit is kept as the slot, and is never
//     derived into the dense node id (the failure mode of compress.hpp's ids);
//   * raw keys are interned into dense per-partition indices with open-addressing
//     tables, so the partition is explicit and no id bit comes from a hash;
//   * adjacency is one CSR over the union of both partition ranges;
//   * a solution is a simple cycle of exactly PROOFSIZE edges in which the two
//     edges meeting at every node use DIFFERENT slots:
//         node(e_i, s_i) == node(e_{i+1}, s_i),   slot(e_i, s_i) != slot(e_{i+1}, s_i)
//     with s_i alternating 0,1,0,1,... and e_P == e_0 (the closing match pairs
//     the V endpoint of the last edge with the V endpoint of e_0, exactly as
//     grin_verify's walk does). The walk starts from the U endpoint of every edge
//     in ascending order and refuses edges with a smaller index, so every cycle is
//     reported exactly once and every reported cycle verifies.
//   * the DFS is iterative (no recursion at all), so neither deep graphs nor
//     pathological branching can overflow the stack.
//
// The search is explicitly bounded by `max_steps` half-edge examinations; a graph
// can never stall the miner (an unbounded DFS previously hung a benchmark for over
// ten minutes). When the budget runs out, whatever was found is returned and
// `step_cap_hit` is reported.
//
// Every solution is still passed through grin::grin_verify by the caller, which is
// GRIN's actual acceptance rule, so a bug here cannot produce a bogus share.

#pragma once

#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
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

    struct Stats {
        uint64_t alive_edges = 0;
        uint64_t nodes = 0;          // distinct nodes over both partitions
        uint64_t nodes_p0 = 0;
        uint64_t nodes_p1 = 0;
        uint64_t full_nodes = 0;     // nodes with at least one edge in EACH slot
        uint64_t steps = 0;
        uint64_t step_cap_hits = 0;
        uint64_t build_ms = 0;
        uint64_t search_ms = 0;
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

    const Stats& stats() const { return stats_; }

private:
    // Open-addressing table mapping a node key (raw sipnode >> 1) to a dense index.
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
        uint32_t intern(uint32_t key) {
            uint32_t slot = (key * 2654435761u) & mask;
            for (;;) {
                if (keys[slot] == key) return values[slot];
                if (keys[slot] == 0xFFFFFFFFu) {
                    keys[slot] = key;
                    values[slot] = count++;
                    return values[slot];
                }
                slot = (slot + 1) & mask;
            }
        }
    };

    struct Frame {
        uint32_t edge;    // dense edge index
        uint32_t cursor;  // next CSR position to examine
    };

    static uint32_t node_of(uint32_t packed) { return packed >> 1; }
    static uint32_t slot_of(uint32_t packed) { return packed & 1u; }

    Params params_{};
    Stats stats_{};

    // Half-edge descriptors. `eu_` holds partition-0 (u) endpoints, `ev_` holds
    // partition-1 (v) endpoints; each is packed as (dense node << 1) | slot.
    std::vector<uint32_t> eu_;
    std::vector<uint32_t> ev_;
    std::vector<uint32_t> enonce_;  // original edge index (nonce)

    uint32_t n0_ = 0;               // number of partition-0 nodes
    uint32_t n1_ = 0;
    uint32_t n_nodes_ = 0;

    // CSR adjacency over the union of both partition ranges.
    std::vector<uint32_t> adj_begin_;   // size n_nodes_ + 1
    std::vector<uint32_t> adj_edges_;   // size 2 * alive edges

    // Search scratch (allocated in build, so search never allocates).
    std::vector<uint32_t> stamp_;       // per-node visit stamp
    std::vector<uint32_t> edge_used_;   // bitmap over dense edges
    std::vector<Frame>    frames_;      // iterative DFS stack, depth PROOFSIZE

    int solutions_found_ = 0;
    RawSolution* out_ = nullptr;

    static void sort_ascending(uint32_t* v) {
        for (int i = 1; i < PROOFSIZE; ++i) {
            const uint32_t key = v[i];
            int j = i;
            while (j > 0 && v[j - 1] > key) { v[j] = v[j - 1]; --j; }
            v[j] = key;
        }
    }
};

inline bool CycleFinder::build(const uint64_t* inverted_alive_bitmap, uint64_t nedges_total,
                               const siphash_keys& keys, const Params& params,
                               std::string& error) {
    params_ = params;
    stats_ = Stats{};

    const uint64_t t0 = 0;   // build_ms is filled by the caller if it wants timing
    (void)t0;

    try {
        // ---- 1. collect the surviving edges, ascending by nonce ---------------
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
            error = "no surviving edges after trimming";
            return false;
        }
        if (n_alive > (uint64_t)(1u << 25)) {
            error = "too many surviving edges for the cycle finder";
            return false;
        }

        // ---- 2. intern node keys (raw >> 1), keep the slot ---------------------
        // At most n_alive distinct keys per partition; a table twice that size
        // keeps linear probing at a load factor of at most 0.5.
        uint64_t cap = 16;
        while (cap < 2 * n_alive + 16) cap <<= 1;

        InternTable t0i, t1i;
        t0i.init((uint32_t)cap);
        t1i.init((uint32_t)cap);

        eu_.resize(n_alive);
        ev_.resize(n_alive);
        enonce_.resize(n_alive);
        siphash_keys* sk = const_cast<siphash_keys*>(&keys);
        for (uint64_t i = 0; i < n_alive; ++i) {
            const uint32_t e = alive_nonce[i];
            const uint32_t u = (uint32_t)sipnode(sk, (word_t)e, 0);
            const uint32_t v = (uint32_t)sipnode(sk, (word_t)e, 1);
            eu_[i] = (t0i.intern(u >> 1) << 1) | (u & 1u);
            ev_[i] = (t1i.intern(v >> 1) << 1) | (v & 1u);
            enonce_[i] = e;
        }
        n0_ = t0i.count;
        n1_ = t1i.count;
        n_nodes_ = n0_ + n1_;
        stats_.nodes = n_nodes_;
        stats_.nodes_p0 = n0_;
        stats_.nodes_p1 = n1_;
        if (n_nodes_ == 0) {
            error = "no nodes after interning";
            return false;
        }

        // Partition-1 dense ids live above partition-0 ids so one CSR serves both.
        // `node_of()` therefore always yields a combined node index. The slot is
        // preserved by the shift, so `^ 1` on a packed id would be the other slot
        // of the same node - but nothing in this file relies on that.
        for (uint64_t i = 0; i < n_alive; ++i) ev_[i] += 2u * n0_;

        // ---- 3. CSR adjacency -------------------------------------------------
        adj_begin_.assign((size_t)n_nodes_ + 1, 0);
        for (uint64_t i = 0; i < n_alive; ++i) {
            ++adj_begin_[node_of(eu_[i]) + 1];
            ++adj_begin_[node_of(ev_[i]) + 1];
        }
        for (uint32_t n = 0; n < n_nodes_; ++n) adj_begin_[n + 1] += adj_begin_[n];

        adj_edges_.resize((size_t)2 * n_alive);
        {
            std::vector<uint32_t> cursor(adj_begin_.begin(), adj_begin_.end() - 1);
            for (uint64_t i = 0; i < n_alive; ++i) {
                adj_edges_[cursor[node_of(eu_[i])]++] = (uint32_t)i;
                adj_edges_[cursor[node_of(ev_[i])]++] = (uint32_t)i;
            }
        }

        // ---- 4. diagnostics + search scratch ----------------------------------
        // A node can only take part in a verify-valid cycle if some edge uses each
        // of its two slots; counting those makes "search finds nothing" easy to
        // tell apart from "the graph cannot contain a cycle at all".
        {
            std::vector<uint8_t> seen0((size_t)n_nodes_, 0), seen1((size_t)n_nodes_, 0);
            for (uint64_t i = 0; i < n_alive; ++i) {
                const uint32_t nu = node_of(eu_[i]);
                const uint32_t nv = node_of(ev_[i]);
                (slot_of(eu_[i]) ? seen1 : seen0)[nu] = 1;
                (slot_of(ev_[i]) ? seen1 : seen0)[nv] = 1;
            }
            uint64_t full = 0;
            for (uint32_t n = 0; n < n_nodes_; ++n)
                if (seen0[n] && seen1[n]) ++full;
            stats_.full_nodes = full;
        }

        stamp_.assign(n_nodes_, 0);
        edge_used_.assign(((size_t)n_alive + 31) / 32, 0);
        frames_.assign((size_t)PROOFSIZE, Frame{0u, 0u});
        solutions_found_ = 0;
        return true;
    } catch (const std::bad_alloc&) {
        error = "host allocation failed in the cycle finder";
        return false;
    } catch (const std::length_error&) {
        error = "graph too large for the cycle finder";
        return false;
    }
}

inline int CycleFinder::search(RawSolution* out, bool& step_cap_hit) {
    out_ = out;
    step_cap_hit = false;
    solutions_found_ = 0;
    if (out_ == nullptr || adj_begin_.empty()) return 0;

    for (size_t i = 0; i < edge_used_.size(); ++i) edge_used_[i] = 0;
    for (size_t i = 0; i < stamp_.size(); ++i) stamp_[i] = 0;

    const uint32_t n_alive = (uint32_t)enonce_.size();
    uint32_t cur = 0;

    for (uint32_t start = 0; start < n_alive; ++start) {
        if (solutions_found_ >= params_.max_solutions) break;
        if (stats_.steps >= params_.max_steps) { step_cap_hit = true; ++stats_.step_cap_hits; break; }

        if (++cur == 0) {                     // stamp counter wrap (never in practice)
            for (size_t i = 0; i < stamp_.size(); ++i) stamp_[i] = 0;
            cur = 1;
        }

        // Root: start at the U endpoint of `start`; its U node is consumed.
        // `node_of()` already returns a combined node index for both partitions.
        const uint32_t root_node = node_of(eu_[start]);
        stamp_[root_node] = cur;
        edge_used_[start >> 5] |= (1u << (start & 31));
        frames_[0].edge = start;
        frames_[0].cursor = adj_begin_[root_node];

        uint32_t depth = 0;
        bool capped = false;
        for (;;) {
            Frame& f = frames_[depth];
            const uint32_t side = depth & 1u;              // 0 = u(partition 0), 1 = v
            const uint32_t packed = (side == 0) ? eu_[f.edge] : ev_[f.edge];
            const uint32_t node = node_of(packed);         // combined node index

            if (depth + 1u == (uint32_t)PROOFSIZE) {
                // Closing match: the start edge's endpoint on this side must be the
                // same node in the other slot. That node is this state's own node
                // (it was checked fresh when this edge was pushed), so no extra
                // visited test belongs here - the reference walk does not have one
                // either, and adding it rejects every valid cycle.
                const uint32_t sraw = (side == 0) ? eu_[start] : ev_[start];
                const uint32_t snode = node_of(sraw);
                if (snode == node && slot_of(sraw) != slot_of(packed)) {
                    if (solutions_found_ < params_.max_solutions) {
                        uint32_t tmp[PROOFSIZE];
                        for (uint32_t k = 0; k < (uint32_t)PROOFSIZE; ++k)
                            tmp[k] = enonce_[frames_[k].edge];
                        sort_ascending(tmp);
                        for (uint32_t k = 0; k < (uint32_t)PROOFSIZE; ++k)
                            out_[solutions_found_].edges[k] = tmp[k];
                        ++solutions_found_;
                    }
                }
                if (depth == 0) break;
                stamp_[node] = 0;
                edge_used_[f.edge >> 5] &= ~(1u << (f.edge & 31));
                --depth;
                continue;
            }

            const uint32_t end = adj_begin_[node + 1];
            const uint32_t slot = slot_of(packed);
            bool pushed = false;
            while (f.cursor < end) {
                if (stats_.steps >= params_.max_steps) {
                    step_cap_hit = true; capped = true; break;
                }
                ++stats_.steps;
                const uint32_t e2 = adj_edges_[f.cursor++];
                if (e2 <= start) continue;                        // canonical start edge
                if ((edge_used_[e2 >> 5] >> (e2 & 31)) & 1u) continue;
                const uint32_t raw2 = (side == 0) ? eu_[e2] : ev_[e2];
                if (slot_of(raw2) == slot) continue;              // must use the other slot
                const uint32_t nside = 1u - side;
                const uint32_t nnode = node_of((nside == 0) ? eu_[e2] : ev_[e2]);
                if (stamp_[nnode] == cur) continue;               // simple cycle
                stamp_[nnode] = cur;
                edge_used_[e2 >> 5] |= (1u << (e2 & 31));
                ++depth;
                frames_[depth].edge = e2;
                frames_[depth].cursor = adj_begin_[nnode];
                pushed = true;
                break;
            }
            if (capped) break;
            if (!pushed) {
                if (depth == 0) break;
                stamp_[node] = 0;
                edge_used_[f.edge >> 5] &= ~(1u << (f.edge & 31));
                --depth;
            }
        }

        if (capped) break;
    }
    return solutions_found_;
}

} // namespace grin
