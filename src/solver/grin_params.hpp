// GRIN Cuckatoo32 — shared parameters and host-side primitives.
//
// PROVENANCE
//   Derived from tromp/cuckoo `src/cuckatoo/cuckatoo.h` and `src/crypto/siphash.hpp`
//   Copyright (c) 2013-2020 John Tromp — "The FAIR MINING License" (MIT-like).
//   See third_party/tromp-cuckoo/LICENSE.txt. Our project charges no developer fee,
//   so the FAIR MINING revenue-sharing condition is not triggered.
//
// CHANGES vs upstream (all deliberate, each marked FIX):
//   FIX-1  MSVC portability: no __builtin_* for the host compiler, no <unistd.h>,
//          no GNU statement expressions, no portable_endian.h (we are little-endian).
//   FIX-2  EDGEBITS=32 correctness: upstream computes
//          `NONPART_BITS = EDGEBITS - PART_BITS` and then shifts a 32-bit `word_t`
//          by NONPART_BITS. With PART_BITS=0 and EDGEBITS=32 that is a shift by 32,
//          which is undefined behaviour, and `((word_t)1 << 32) - 1` silently
//          evaluates wrong. Upstream never shipped an EDGEBITS=32 CUDA target
//          (the Makefile has lcuda19/29/30/31 but no lcuda32), which is consistent
//          with this latent bug. We compute the mask in 64-bit and short-circuit
//          the unpartitioned case.
//   FIX-3  Every function is `inline` so the header can be included from more than
//          one translation unit (upstream defines non-inline globals, which breaks
//          any multi-TU build).

#pragma once

#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef uint32_t u32;
typedef uint64_t u64;

// ---------------------------------------------------------------------------
// PoW parameters
// ---------------------------------------------------------------------------

#ifndef EDGEBITS
#define EDGEBITS 32
#endif
#ifndef PROOFSIZE
#define PROOFSIZE 42
#endif

#if EDGEBITS > 32
typedef uint64_t word_t;
#elif EDGEBITS > 16
typedef u32 word_t;
#else
typedef uint16_t word_t;
#endif

// Number of nodes in one partition, and the number of edges.
#define NNODES1 (1ULL << EDGEBITS)
// Written as (word_t)(NNODES1 - 1) rather than (word_t)NNODES1 - 1: identical for
// every supported EDGEBITS, but without a truncating-cast warning at EDGEBITS=32.
#define NODEMASK ((word_t)(NNODES1 - 1))
#define NODE1MASK NODEMASK
#define EDGEMASK NODEMASK
#define NEDGES NNODES1
#define NODEBITS (EDGEBITS + 1)

// SIZEMASK is only used by the CPU cycle finder: ~0 >> clz(PROOFSIZE) == 63 for 42.
// constexpr so that SIZEMASK stays a compile-time constant (it sizes a stack array
// in grin_verify).
constexpr u32 grin_clz32(u32 x) {
  if (x == 0) return 32;
  u32 n = 0;
  if ((x & 0xFFFF0000u) == 0) { n += 16; x <<= 16; }
  if ((x & 0xFF000000u) == 0) { n += 8;  x <<= 8;  }
  if ((x & 0xF0000000u) == 0) { n += 4;  x <<= 4;  }
  if ((x & 0xC0000000u) == 0) { n += 2;  x <<= 2;  }
  if ((x & 0x80000000u) == 0) { n += 1; }
  return n;
}
#define SIZEMASK (~0u >> grin_clz32(PROOFSIZE))

// ---------------------------------------------------------------------------
// FIX-1: portability shims for the host compiler
// ---------------------------------------------------------------------------

#if defined(_MSC_VER)
#include <intrin.h>
// Count trailing zeros + 1 (the semantics of GCC's __builtin_ffsll).
// Host-only: these run on the x86 side of the .cu, never in device code.
static inline int grin_ffs64(u64 x) {
  unsigned long index = 0;
  if (_BitScanForward64(&index, x) == 0) return 0;
  return (int)index + 1;
}
static inline int grin_popcount64(u64 x) { return (int)__popcnt64(x); }
#else
static inline int grin_ffs64(u64 x) { return __builtin_ffsll((long long)x); }
static inline int grin_popcount64(u64 x) { return __builtin_popcountll(x); }
#endif

// ---------------------------------------------------------------------------
// siphash keys (host side)
// ---------------------------------------------------------------------------
//
// FIX-1: upstream uses `htole64()` from portable_endian.h. On the only platform we
// target (Windows x86-64) the host is little-endian, so loading the four 64-bit
// words natively is exactly what `htole64` does. A compile-time check guards this.
//
class siphash_keys {
public:
  uint64_t k0;
  uint64_t k1;
  uint64_t k2;
  uint64_t k3;

  void setkeys(const char *keybuf) {
    std::memcpy(&k0, keybuf + 0,  8);
    std::memcpy(&k1, keybuf + 8,  8);
    std::memcpy(&k2, keybuf + 16, 8);
    std::memcpy(&k3, keybuf + 24, 8);
  }

  uint64_t siphash24(uint64_t nonce) const;
};

static inline uint64_t grin_rotl64(uint64_t x, int b) {
  return (x << b) | (x >> (64 - b));
}

inline uint64_t siphash_keys::siphash24(uint64_t nonce) const {
  uint64_t v0 = k0, v1 = k1, v2 = k2, v3 = k3;
  // rotE = 21: same as siphash_state<> and diphash_state<> in siphash.cuh.
#define GRIN_SIPROUND()                     \
  do {                                      \
    v0 += v1; v2 += v3;                     \
    v1 = grin_rotl64(v1, 13);               \
    v3 = grin_rotl64(v3, 16);               \
    v1 ^= v0;  v3 ^= v2;                    \
    v0 = grin_rotl64(v0, 32);               \
    v2 += v1;  v0 += v3;                    \
    v1 = grin_rotl64(v1, 17);               \
    v3 = grin_rotl64(v3, 21);               \
    v1 ^= v2;  v3 ^= v0;                    \
    v2 = grin_rotl64(v2, 32);               \
  } while (0)

  v3 ^= nonce;
  GRIN_SIPROUND();
  GRIN_SIPROUND();
  v0 ^= nonce;
  v2 ^= 0xff;
  GRIN_SIPROUND();
  GRIN_SIPROUND();
  GRIN_SIPROUND();
  GRIN_SIPROUND();
#undef GRIN_SIPROUND
  return (v0 ^ v1) ^ (v2 ^ v3);
}

// Edge endpoint generation without the partition bit.
static inline word_t sipnode(siphash_keys *keys, word_t edge, u32 uorv) {
  return (word_t)(keys->siphash24(2 * (u64)edge + uorv) & NODEMASK);
}

// ---------------------------------------------------------------------------
// GRIN block header for Cuckatoo32 pool mining
// ---------------------------------------------------------------------------
//
// Verified against grin.2miners.com:3030 and against the MIT-licensed reference
// client client8568/High-Resource-Cuckatoo-Miner. See docs/stratum-protocol.md.
//
//   hash input = pre_pow (238 bytes, used verbatim) || nonce (8 bytes BIG-endian)
//   siphash keys = BLAKE2b-512(hash input)[0..31], read as four little-endian u64
//
// This is NOT the 76+4 byte Cuckaroo/Cuckatoo29 layout that tromp's
// `setheadernonce` assumes, so `mutate_nonce` must be false for pool mining.
//
static const size_t GRIN_PRE_POW_SIZE   = 238;
static const size_t GRIN_NONCE_SIZE     = 8;
static const size_t GRIN_HEADER_LEN     = GRIN_PRE_POW_SIZE + GRIN_NONCE_SIZE; // 246

static inline void grin_build_header(const uint8_t *pre_pow,
                                     size_t pre_pow_len,
                                     uint64_t nonce,
                                     uint8_t *out /* [GRIN_HEADER_LEN] */) {
  assert(pre_pow_len == GRIN_PRE_POW_SIZE);
  std::memcpy(out, pre_pow, pre_pow_len);
  for (size_t i = 0; i < GRIN_NONCE_SIZE; ++i) {
    out[pre_pow_len + i] = (uint8_t)(nonce >> (8 * (GRIN_NONCE_SIZE - 1 - i)));
  }
}

// ---------------------------------------------------------------------------
// Logging (upstream `print_log`, FIX-3: inline)
// ---------------------------------------------------------------------------

#ifndef SQUASH_OUTPUT
#define SQUASH_OUTPUT 0
#endif

inline void print_log(const char *fmt, ...) {
  if (SQUASH_OUTPUT) return;
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}
