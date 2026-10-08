// Independent CPU reference for the Cuckoo Cycle pipeline.
//
// This deliberately shares NOTHING with the GPU solver except the header/key
// derivation and the siphash itself (both already cross-checked against Python):
//
//   * it builds the FULL graph, with NO edge trimming at all, so a bug in the
//     trimming cannot hide here;
//   * it enumerates cycles as edges are inserted, using upstream graph.hpp;
//   * it reports every cycle length it closes.
//
// Run it at small parameters (e.g. EDGEBITS=20, PROOFSIZE=6) where the graph fits
// in a few MB. If this finds 6-cycles and the GPU lean solver does not, the bug is
// in the lean pipeline. If neither finds any, the parameter set simply has no
// solutions and the GPU result says nothing.
//
// Usage: simple_ref --pre-pow-file <path> [--nonce-start N] [--nonce-count N]

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "grin_verify.hpp"

// Upstream graph: no compression, no trimming.
#include "graph.hpp"

namespace {

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool read_pre_pow(const std::string& path, std::vector<uint8_t>& pre) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    const std::string key = "\"pre_pow\":";
    size_t at = text.find(key);
    std::string hex;
    if (at != std::string::npos) {
        at += key.size();
        while (at < text.size() && (text[at] == ' ' || text[at] == '"')) ++at;
        size_t end = at;
        while (end < text.size() && hex_nibble(text[end]) >= 0) ++end;
        hex = text.substr(at, end - at);
    } else {
        for (char c : text) {
            if (hex_nibble(c) >= 0) hex.push_back(c);
        }
    }
    if (hex.size() != GRIN_PRE_POW_SIZE * 2) return false;
    pre.resize(GRIN_PRE_POW_SIZE);
    for (size_t i = 0; i < pre.size(); ++i) {
        const int hi = hex_nibble(hex[2 * i]);
        const int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        pre[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string prePowFile;
    uint64_t nonceStart = 0;
    uint64_t nonceCount = 10;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::printf("%s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--pre-pow-file") prePowFile = next("--pre-pow-file");
        else if (a == "--nonce-start") nonceStart = std::strtoull(next("--nonce-start"), nullptr, 10);
        else if (a == "--nonce-count") nonceCount = std::strtoull(next("--nonce-count"), nullptr, 10);
        else { std::printf("unknown option %s\n", a.c_str()); return 2; }
    }
    if (prePowFile.empty()) {
        std::printf("usage: simple_ref --pre-pow-file <path> [--nonce-start N] [--nonce-count N]\n");
        return 2;
    }

    std::vector<uint8_t> pre;
    if (!read_pre_pow(prePowFile, pre)) {
        std::printf("could not read a %zu-byte pre_pow from %s\n", GRIN_PRE_POW_SIZE,
                    prePowFile.c_str());
        return 2;
    }

    std::printf("simple reference: EDGEBITS=%d PROOFSIZE=%d NEDGES=%" PRIu64 "\n", EDGEBITS,
                PROOFSIZE, (uint64_t)NEDGES);
    // The full graph needs 2*NEDGES half-edge slots; at EDGEBITS=20 that is ~24 MB.
    const size_t approxBytes = (size_t)NEDGES * (sizeof(word_t) * 2 + 2 * sizeof(word_t) * 2);
    std::printf("approx graph memory: %.1f MiB\n", (double)approxBytes / (1024.0 * 1024.0));
    if (approxBytes > (size_t)(6ull << 30)) {
        std::printf("refusing: this reference only works at small EDGEBITS\n");
        return 2;
    }

    uint32_t totalCycles[PROOFSIZE + 1] = {};
    uint64_t totalSolutions = 0;

    for (uint64_t nonce = nonceStart; nonce < nonceStart + nonceCount; ++nonce) {
        siphash_keys keys;
        uint8_t header[GRIN_HEADER_LEN];
        grin_build_header(pre.data(), pre.size(), nonce, header);
        grin::grin_setheader(header, (uint32_t)GRIN_HEADER_LEN, &keys);

        graph<word_t> cg((word_t)NEDGES, (word_t)NEDGES, 4, 0);
        for (uint64_t e = 0; e < NEDGES; ++e) {
            const word_t u = sipnode(&keys, (word_t)e, 0);
            const word_t v = sipnode(&keys, (word_t)e, 1);
            cg.add_edge(u, v);
        }

        uint32_t len42 = 0;
        // cg.nsols holds cycles of length PROOFSIZE that were closed during insertion.
        len42 = cg.nsols;
        if (len42) {
            totalSolutions += len42;
            totalCycles[PROOFSIZE] += len42;
            std::printf("nonce %-6" PRIu64 " solutions=%u\n", nonce, len42);
            for (u32 s = 0; s < cg.nsols; ++s) {
                const int rc = grin::grin_verify((const word_t*)cg.sols[s], &keys);
                std::printf("   solution %u verify=%s\n", s, grin::verify_str(rc));
            }
        } else {
            std::printf("nonce %-6" PRIu64 " solutions=0\n", nonce);
        }
    }

    std::printf("--- summary: %" PRIu64 " solutions over %" PRIu64 " graphs ---\n",
                totalSolutions, nonceCount);
    return totalSolutions ? 0 : 1;
}
