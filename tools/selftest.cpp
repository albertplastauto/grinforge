// Crypto/parameter self-test. Deliberately CUDA-free so it can be built and run
// before the CUDA toolkit is present, and so the header/key derivation can be
// cross-checked against an independent BLAKE2b implementation (Python hashlib).
//
//   selftest                 run the built-in checks
//   selftest --dump <hex> <n> print BLAKE2b(pre_pow || be64(nonce)) for comparison

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/solver/grin_verify.hpp"

namespace {

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void print_hex(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) std::printf("%02x", p[i]);
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--dump") == 0) {
        const std::string hex = argv[2];
        const uint64_t nonce = std::strtoull(argv[3], nullptr, 10);
        if (hex.size() != GRIN_PRE_POW_SIZE * 2) {
            std::printf("pre_pow must be %zu hex chars, got %zu\n", GRIN_PRE_POW_SIZE * 2,
                        hex.size());
            return 2;
        }
        std::vector<uint8_t> pre(GRIN_PRE_POW_SIZE);
        for (size_t i = 0; i < pre.size(); ++i) {
            const int hi = hex_nibble(hex[2 * i]);
            const int lo = hex_nibble(hex[2 * i + 1]);
            if (hi < 0 || lo < 0) { std::printf("bad hex\n"); return 2; }
            pre[i] = (uint8_t)((hi << 4) | lo);
        }
        uint8_t header[GRIN_HEADER_LEN];
        grin_build_header(pre.data(), pre.size(), nonce, header);
        uint8_t digest[32];
        blake2b(digest, sizeof(digest), header, GRIN_HEADER_LEN, nullptr, 0);
        std::printf("nonce=%" PRIu64 "\ndigest=", nonce);
        print_hex(digest, sizeof(digest));
        std::printf("\nheader_len=%zu\n", GRIN_HEADER_LEN);
        return 0;
    }

    int failures = 0;

    // --- BLAKE2b-512("abc") known-answer test -------------------------------
    static const uint8_t expected[64] = {
        0xba, 0x80, 0xa5, 0x3f, 0x98, 0x1c, 0x4d, 0x0d, 0x6a, 0x27, 0x97, 0xb6, 0x9f,
        0x12, 0xf6, 0xe9, 0x4c, 0x21, 0x2f, 0x14, 0x68, 0x5a, 0xc4, 0xb7, 0x4b, 0x12,
        0xbb, 0x6f, 0xdb, 0xff, 0xa2, 0xd1, 0x7d, 0x87, 0xc5, 0x39, 0x2a, 0xab, 0x79,
        0x2d, 0xc2, 0x52, 0xd5, 0xde, 0x45, 0x33, 0xcc, 0x95, 0x18, 0xd3, 0x8a, 0xa8,
        0xdb, 0xf1, 0x92, 0x5a, 0xb9, 0x23, 0x86, 0xed, 0xd4, 0x00, 0x99, 0x23,
    };
    uint8_t digest[64];
    blake2b(digest, sizeof(digest), "abc", 3, nullptr, 0);
    if (std::memcmp(digest, expected, sizeof(expected)) != 0) {
        std::printf("FAIL: BLAKE2b-512 known-answer test mismatch\n  got ");
        print_hex(digest, 64);
        std::printf("\n");
        ++failures;
    } else {
        std::printf("ok   BLAKE2b-512(\"abc\") known-answer test\n");
    }

    // --- parameter sanity (FIX-2 regression guard) --------------------------
    std::printf("info EDGEBITS=%d PROOFSIZE=%d sizeof(word_t)=%zu NEDGES=%" PRIu64 "\n",
                EDGEBITS, PROOFSIZE, sizeof(word_t), (uint64_t)NEDGES);
    if ((uint64_t)NODEMASK + 1 != (uint64_t)NEDGES) {
        std::printf("FAIL: NODEMASK does not cover the full edge space\n");
        ++failures;
    } else {
        std::printf("ok   NODEMASK covers the full 2^%d edge space\n", EDGEBITS);
    }
    if (SIZEMASK != 63u) {
        std::printf("FAIL: SIZEMASK = %u, expected 63 for PROOFSIZE=42\n", SIZEMASK);
        ++failures;
    } else {
        std::printf("ok   SIZEMASK == 63\n");
    }
    if (grin::grin_graph_weight(32) != 16384ULL) {
        std::printf("FAIL: graph_weight(32) = %" PRIu64 ", expected 16384\n",
                    grin::grin_graph_weight(32));
        ++failures;
    } else {
        std::printf("ok   graph_weight(32) == 16384 (GRIN consensus)\n");
    }

    // --- GRIN header layout -------------------------------------------------
    uint8_t pre[GRIN_PRE_POW_SIZE];
    for (size_t i = 0; i < sizeof(pre); ++i) pre[i] = (uint8_t)(i & 0xFF);
    uint8_t header[GRIN_HEADER_LEN];
    grin_build_header(pre, sizeof(pre), 0x0102030405060708ULL, header);
    static const uint8_t expectNonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    if (std::memcmp(header + GRIN_PRE_POW_SIZE, expectNonce, 8) != 0) {
        std::printf("FAIL: nonce is not appended big-endian\n");
        ++failures;
    } else {
        std::printf("ok   nonce appended big-endian at offset %zu\n", GRIN_PRE_POW_SIZE);
    }
    if (std::memcmp(header, pre, sizeof(pre)) != 0) {
        std::printf("FAIL: pre_pow bytes were modified while building the header\n");
        ++failures;
    } else {
        std::printf("ok   pre_pow copied verbatim (no byte swapping)\n");
    }

    // --- siphash key derivation is deterministic and endianness-stable ------
    siphash_keys keys;
    grin::grin_setheader(header, (uint32_t)GRIN_HEADER_LEN, &keys);
    std::printf("info keys k0=%016" PRIx64 " k1=%016" PRIx64 " k2=%016" PRIx64
                " k3=%016" PRIx64 "\n", keys.k0, keys.k1, keys.k2, keys.k3);
    const uint64_t e0 = sipnode(&keys, 0, 0);
    const uint64_t e1 = sipnode(&keys, 0, 1);
    std::printf("info sipnode(edge=0) u=%" PRIu64 " v=%" PRIu64 "\n", e0, e1);
    if (e0 > (uint64_t)NODEMASK || e1 > (uint64_t)NODEMASK) {
        std::printf("FAIL: sipnode exceeded NODEMASK\n");
        ++failures;
    } else {
        std::printf("ok   sipnode output is masked to %u bits\n", EDGEBITS);
    }

    std::printf("selftest %s (%d failure(s))\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
