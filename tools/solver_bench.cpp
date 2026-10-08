// GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
// Copyright (c) 2026 albertplastauto
// SPDX-License-Identifier: MIT
//
// Standalone Cuckatoo32 solver benchmark and validation tool.
//
// Modes:
//   --selftest                  BLAKE2b known-answer test + header/key derivation check
//   --dump-keys <hex476> <n>    print BLAKE2b(pre_pow || be64(nonce)) for independent
//                               cross-checking against a separate BLAKE2b implementation
//   (default)                   run the GPU solver over a range of nonces and report GPS
//
// Every solution the GPU reports is re-verified on the host with grin_verify(), an
// independent implementation of GRIN's cycle check, before it is counted.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include "../src/solver/grin_verify.hpp"
#include "../src/solver/lean_solver.hpp"

namespace {

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_decode(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2) return false;
    out.resize(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = hex_nibble(hex[2 * i]);
        const int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// Pull "pre_pow":"<hex>" out of a captured stratum line or a bare hex string.
bool extract_pre_pow(const std::string& text, std::string& hex) {
    const std::string key = "\"pre_pow\":";
    size_t at = text.find(key);
    if (at == std::string::npos) {
        // Treat the whole (trimmed) input as hex if it looks like hex.
        std::string t;
        for (char c : text) {
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
            t.push_back(c);
        }
        if (t.size() == 476) { hex = t; return true; }
        return false;
    }
    at += key.size();
    while (at < text.size() && (text[at] == ' ' || text[at] == '"')) ++at;
    size_t end = at;
    while (end < text.size() && hex_nibble(text[end]) >= 0) ++end;
    hex = text.substr(at, end - at);
    return !hex.empty();
}

void print_hex(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) std::printf("%02x", p[i]);
}

int selftest() {
    int failures = 0;

    // BLAKE2b-512("abc") — well-known test vector.
    static const uint8_t expected[64] = {
        0xba, 0x80, 0xa5, 0x3f, 0x98, 0x1c, 0x4d, 0x0d, 0x6a, 0x27, 0x97, 0xb6, 0x9f,
        0x12, 0xf6, 0xe9, 0x4c, 0x21, 0x2f, 0x14, 0x68, 0x5a, 0xc4, 0xb7, 0x4b, 0x12,
        0xbb, 0x6f, 0xdb, 0xff, 0xa2, 0xd1, 0x7d, 0x87, 0xc5, 0x39, 0x2a, 0xab, 0x79,
        0x2d, 0xc2, 0x52, 0xd5, 0xde, 0x45, 0x33, 0xcc, 0x95, 0x18, 0xd3, 0x8a, 0xa8,
        0xdb, 0xf1, 0x92, 0x5a, 0xb9, 0x23, 0x86, 0xed, 0xd4, 0x00, 0x99, 0x23,
    };
    uint8_t digest[64];
    blake2b(digest, sizeof(digest), "abc", 3, nullptr, 0);
    std::printf("BLAKE2b-512(\"abc\") = ");
    print_hex(digest, 32);
    std::printf("...\n");
    if (std::memcmp(digest, expected, sizeof(expected)) != 0) {
        std::printf("  FAIL: BLAKE2b known-answer test mismatch\n");
        ++failures;
    } else {
        std::printf("  OK: BLAKE2b known-answer test\n");
    }

    // FIX-2 regression guard: at EDGEBITS=32 the partition mask must be all ones.
    std::printf("EDGEBITS=%d PROOFSIZE=%d word_t=%zu bytes NEDGES=%" PRIu64 "\n",
                EDGEBITS, PROOFSIZE, sizeof(word_t), (uint64_t)NEDGES);
    if ((uint64_t)NODEMASK + 1 != (uint64_t)NEDGES) {
        std::printf("  FAIL: NODEMASK overflow for EDGEBITS=%d\n", EDGEBITS);
        ++failures;
    } else {
        std::printf("  OK: NODEMASK covers the full edge space\n");
    }

    // Header layout: big-endian nonce must land in the last 8 bytes.
    uint8_t pre[GRIN_PRE_POW_SIZE];
    for (size_t i = 0; i < sizeof(pre); ++i) pre[i] = (uint8_t)(i & 0xFF);
    uint8_t header[GRIN_HEADER_LEN];
    grin_build_header(pre, sizeof(pre), 0x0102030405060708ULL, header);
    const uint8_t expect_nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    if (std::memcmp(header + GRIN_PRE_POW_SIZE, expect_nonce, 8) != 0) {
        std::printf("  FAIL: nonce is not appended big-endian\n");
        ++failures;
    } else {
        std::printf("  OK: nonce appended big-endian at offset %zu\n", GRIN_PRE_POW_SIZE);
    }
    if (std::memcmp(header, pre, sizeof(pre)) != 0) {
        std::printf("  FAIL: pre_pow bytes were modified\n");
        ++failures;
    } else {
        std::printf("  OK: pre_pow copied verbatim\n");
    }

    // Graph weight for C32 must be 16384 per GRIN consensus.
    if (grin::grin_graph_weight(32) != 16384ULL) {
        std::printf("  FAIL: graph_weight(32) = %" PRIu64 ", expected 16384\n",
                    grin::grin_graph_weight(32));
        ++failures;
    } else {
        std::printf("  OK: graph_weight(32) == 16384\n");
    }

    std::printf("selftest: %s (%d failure(s))\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    grin::SolverConfig cfg;
    uint64_t nonceStart = 0;
    uint64_t nonceCount = 10;
    double   seconds = 0.0;
    bool     verifyOnly = false;
    bool     deviceCheck = false;
    std::string prePowHex;
    std::string prePowFile;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::printf("%s requires a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--selftest") return selftest();
        if (a == "--device") cfg.device = std::atoi(next("--device"));
        else if (a == "--ntrims") cfg.ntrims = (uint32_t)std::strtoul(next("--ntrims"), nullptr, 10);
        else if (a == "--blocks") cfg.blocks = (uint32_t)std::strtoul(next("--blocks"), nullptr, 10);
        else if (a == "--tpb") cfg.tpb = (uint32_t)std::strtoul(next("--tpb"), nullptr, 10);
        else if (a == "--nonce-start") nonceStart = std::strtoull(next("--nonce-start"), nullptr, 10);
        else if (a == "--nonce-count") nonceCount = std::strtoull(next("--nonce-count"), nullptr, 10);
        else if (a == "--seconds") seconds = std::strtod(next("--seconds"), nullptr);
        else if (a == "--pre-pow") prePowHex = next("--pre-pow");
        else if (a == "--pre-pow-file") prePowFile = next("--pre-pow-file");
        else if (a == "--dump-keys") {
            const std::string hex = next("--dump-keys");
            const uint64_t n = std::strtoull(next("nonce"), nullptr, 10);
            std::vector<uint8_t> pre;
            if (!hex_decode(hex, pre) || pre.size() != GRIN_PRE_POW_SIZE) {
                std::printf("pre_pow must be 476 hex chars (%zu bytes)\n", GRIN_PRE_POW_SIZE);
                return 2;
            }
            uint8_t d[32];
            grin::LeanSolver::derive_keys(pre.data(), pre.size(), n, d);
            std::printf("nonce=%" PRIu64 "\ndigest=", n);
            print_hex(d, sizeof(d));
            std::printf("\n");
            return 0;
        } else if (a == "--verify-only") verifyOnly = true;
        else if (a == "--device-check") deviceCheck = true;
        else { std::printf("unknown option: %s\n", a.c_str()); return 2; }
    }

    if (!prePowFile.empty()) {
        FILE* f = std::fopen(prePowFile.c_str(), "rb");
        if (!f) { std::printf("cannot open %s\n", prePowFile.c_str()); return 2; }
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
        if (!extract_pre_pow(text, prePowHex)) {
            std::printf("no 476-char pre_pow found in %s\n", prePowFile.c_str());
            return 2;
        }
    }

    if (prePowHex.empty()) {
        std::printf(
            "usage: solver_bench --pre-pow <476 hex> [--device N] [--ntrims N] [--blocks N]\n"
            "                    [--tpb N] [--nonce-start N] [--nonce-count N] [--seconds S]\n"
            "       solver_bench --pre-pow-file <path with a captured job line>\n"
            "       solver_bench --selftest\n"
            "       solver_bench --dump-keys <476 hex> <nonce>\n");
        return 2;
    }

    std::vector<uint8_t> pre;
    if (!hex_decode(prePowHex, pre) || pre.size() != GRIN_PRE_POW_SIZE) {
        std::printf("pre_pow must be 476 hex chars (%zu bytes), got %zu\n",
                    GRIN_PRE_POW_SIZE, pre.size());
        return 2;
    }

    std::printf("=== GRIN Cuckatoo32 lean solver benchmark ===\n");
    std::printf("device_count=%d\n", grin::LeanSolver::device_count());
    std::string name;
    uint64_t vram = 0;
    int cmaj = 0, cmin = 0;
    if (grin::LeanSolver::device_info(cfg.device, name, vram, cmaj, cmin)) {
        std::printf("device[%d] = %s, %" PRIu64 " MiB VRAM, sm_%d%d\n", cfg.device,
                    name.c_str(), vram / (1024 * 1024), cmaj, cmin);
    } else {
        std::printf("device[%d] unavailable\n", cfg.device);
        return 1;
    }
    std::printf("config: ntrims=%u blocks=%u tpb=%u, device image ~%u MiB\n",
                cfg.ntrims, cfg.blocks, cfg.tpb,
                (unsigned)(grin::LeanSolver::device_bytes_for(cfg) / (1024 * 1024)));

    if (verifyOnly) {
        std::printf("pre_pow accepted (%zu bytes)\n", pre.size());
        uint8_t d[32];
        grin::LeanSolver::derive_keys(pre.data(), pre.size(), nonceStart, d);
        std::printf("digest(nonce=%" PRIu64 ") = ", nonceStart);
        print_hex(d, sizeof(d));
        std::printf("\n");
        return 0;
    }

    // Prove that the device siphash (uint2/PTX path in the trim kernels) and the
    // host siphash (used to rebuild the graph for the cycle search) agree. If they
    // did not, the solver would trim one graph and search another, and would never
    // find a 42-cycle.
    if (deviceCheck) {
        const uint32_t N = 64;
        std::vector<uint32_t> dev(2 * N);
        std::string probeError;
        if (!grin::LeanSolver::device_probe(pre.data(), pre.size(), nonceStart, N, dev.data(),
                                            probeError)) {
            std::printf("device_probe failed: %s\n", probeError.c_str());
            return 1;
        }
        siphash_keys hk;
        uint8_t hdr[GRIN_HEADER_LEN];
        grin_build_header(pre.data(), pre.size(), nonceStart, hdr);
        grin::grin_setheader(hdr, (uint32_t)GRIN_HEADER_LEN, &hk);
        int mismatches = 0;
        for (uint32_t i = 0; i < N; ++i) {
            const uint32_t hu = (uint32_t)sipnode(&hk, (word_t)i, 0);
            const uint32_t hv = (uint32_t)sipnode(&hk, (word_t)i, 1);
            if (hu != dev[i] || hv != dev[N + i]) {
                if (mismatches < 5) {
                    std::printf("  MISMATCH edge %u: host u=%u v=%u | device u=%u v=%u\n", i, hu,
                                hv, dev[i], dev[N + i]);
                }
                ++mismatches;
            }
        }
        std::printf("device-vs-host siphash: %s (%d/%u mismatches)\n",
                    mismatches ? "MISMATCH" : "agree", mismatches, N);
        return mismatches ? 4 : 0;
    }

    grin::LeanSolver solver(cfg);
    std::string error;
    if (!solver.set_pre_pow(pre.data(), pre.size(), error)) {
        std::printf("set_pre_pow failed: %s\n", error.c_str());
        return 1;
    }

    uint64_t attempted = 0, found = 0, verified = 0;
    const double t0 = now_ms();
    uint64_t nonce = nonceStart;

    while (true) {
        if (seconds > 0.0 && (now_ms() - t0) / 1000.0 >= seconds) break;
        if (seconds <= 0.0 && attempted >= nonceCount) break;

        grin::FoundSolution out[8];
        int nfound = 0;
        const grin::SolveStatus st = solver.solve(nonce, out, 8, nfound, error);
        const auto last = solver.last_run();
        ++attempted;

        if (st == grin::SolveStatus::CudaError) {
            std::printf("nonce %" PRIu64 ": CUDA ERROR: %s\n", nonce, error.c_str());
            return 1;
        }

        const char* status = "ok";
        if (st == grin::SolveStatus::Overloaded) status = "OVERLOADED";
        else if (st == grin::SolveStatus::Aborted) status = "aborted";

        std::printf("nonce %-10" PRIu64 " %-10s trim=%7.1fms cycles=%6.1fms total=%7.1fms "
                    "edges=%9" PRIu64 " raw_cycles=%u vfail=%u sols=%d\n",
                    nonce, status, last.trim_ms, last.find_cycles_ms, last.total_ms,
                    last.edges_after_trim, (unsigned)last.raw_cycles,
                    (unsigned)last.verify_failures, nfound);

        for (int s = 0; s < nfound; ++s) {
            // Re-verify independently and report the achieved difficulty.
            siphash_keys keys;
            uint8_t header[GRIN_HEADER_LEN];
            grin_build_header(pre.data(), pre.size(), out[s].nonce, header);
            grin::grin_setheader(header, (uint32_t)GRIN_HEADER_LEN, &keys);
            const int rc = grin::grin_verify((const word_t*)out[s].proof, &keys);
            const bool ok = (rc == grin::POW_OK);
            if (ok) ++verified;
            std::printf("  solution %d: ", s);
            for (int i = 0; i < PROOFSIZE; ++i) std::printf("%u ", out[s].proof[i]);
            std::printf("\n    verify=%s lz=%" PRIu64 " difficulty=%" PRIu64 "\n",
                        ok ? "OK" : grin::verify_str(rc),
                        out[s].cyclehash_leading_zeros,
                        grin::grin_solution_difficulty(out[s].proof, EDGEBITS));
            ++found;
        }
        ++nonce;
    }

    const double elapsed = (now_ms() - t0) / 1000.0;
    std::printf("--- summary ---\n");
    std::printf("attempts=%" PRIu64 " elapsed=%.2fs gps=%.4f solutions=%" PRIu64
                " verified=%" PRIu64 "\n",
                attempted, elapsed, elapsed > 0 ? attempted / elapsed : 0.0, found, verified);
    if (found != verified) {
        std::printf("WARNING: %" PRIu64 " solution(s) failed independent verification\n",
                    found - verified);
        return 3;
    }
    return 0;
}
