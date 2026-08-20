/* simd_crypto.c — AES-256-GCM and ChaCha20-Poly1305, written here.
 *
 * The point of writing these is not to beat OpenSSL. It is that a cost model
 * whose marginal term b comes out of a library is a model of the library, and
 * the moment the accelerator is masked one cannot tell whether what moved was
 * the instruction set or the library's dispatch. Two implementations of the
 * same two ciphers, masked by the same lever, separate those.
 *
 * Layout: dispatch first, then AES-GCM (x86, ARM, portable), then
 * ChaCha20-Poly1305 (x86, ARM, portable), then the public entry points.
 *
 * Every SIMD function carries its own target attribute rather than relying on
 * global -march flags, so this file compiles for a generic x86-64 target and
 * chooses its path at run time. That matters because the same source is built
 * on the measurement VM, on an ARM instance and on the laptop.
 */
#include "lr/simd_crypto.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static inline uint32_t load32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline void store32_le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void store64_be(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (56 - 8 * i));
}
static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

/* Constant-time 16-byte comparison. A tag check that leaks through timing is a
 * real defect even in a benchmark, because the benchmark is the thing other
 * people copy. */
static int ct_eq16(const uint8_t* a, const uint8_t* b) {
    uint8_t d = 0;
    for (int i = 0; i < 16; ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* ------------------------------------------------------------------------ */
/* Run-time dispatch                                                        */
/* ------------------------------------------------------------------------ */

enum { PATH_PORTABLE = 0, PATH_X86 = 1, PATH_ARM = 2 };

static int g_force_portable = 0;
/* Masking the AES accelerator is not the same as disabling vectorisation.
   OPENSSL_ia32cap clears AES-NI and PCLMULQDQ but leaves AVX2 intact, so
   OpenSSL's ChaCha20 keeps its vector path under the mask. If the
   implementations here dropped to portable for ChaCha20 as well, the masked arm
   would compare a vectorised OpenSSL against a scalar us, and the difference
   would be attributed to the AES accelerator that neither of them uses. */
static int g_mask_aes_accel = 0;
static int g_gcm_path  = -1;
static int g_chap_path = -1;

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static int cpu_has_aes_clmul(void) {
    return __builtin_cpu_supports("aes") && __builtin_cpu_supports("pclmul") &&
           __builtin_cpu_supports("ssse3");
}
static int cpu_has_avx2(void) { return __builtin_cpu_supports("avx2"); }
#elif defined(__aarch64__)
#include <arm_neon.h>
#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_AES
#define HWCAP_AES (1 << 3)
#endif
#ifndef HWCAP_PMULL
#define HWCAP_PMULL (1 << 4)
#endif
static int cpu_has_aes_clmul(void) {
    unsigned long h = getauxval(AT_HWCAP);
    return (h & HWCAP_AES) && (h & HWCAP_PMULL);
}
#else
static int cpu_has_aes_clmul(void) { return 1; }   /* Apple silicon: mandatory */
#endif
static int cpu_has_neon(void) { return 1; }        /* baseline on AArch64 */
#else
static int cpu_has_aes_clmul(void) { return 0; }
#endif

static void resolve_paths(void) {
    if (g_gcm_path >= 0 && g_chap_path >= 0) return;
#if defined(__x86_64__) || defined(__i386__)
    g_gcm_path  = cpu_has_aes_clmul() ? PATH_X86 : PATH_PORTABLE;
    g_chap_path = cpu_has_avx2()      ? PATH_X86 : PATH_PORTABLE;
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
    g_gcm_path  = cpu_has_aes_clmul() ? PATH_ARM : PATH_PORTABLE;
    g_chap_path = cpu_has_neon()      ? PATH_ARM : PATH_PORTABLE;
#elif defined(__aarch64__)
    g_gcm_path  = PATH_PORTABLE;
    g_chap_path = cpu_has_neon() ? PATH_ARM : PATH_PORTABLE;
#else
    g_gcm_path  = PATH_PORTABLE;
    g_chap_path = PATH_PORTABLE;
#endif
}

static int gcm_path(void) {
    resolve_paths();
    return (g_force_portable || g_mask_aes_accel) ? PATH_PORTABLE : g_gcm_path;
}
static int chap_path(void) {
    resolve_paths();
    return g_force_portable ? PATH_PORTABLE : g_chap_path;
}

void lr_simd_force_portable(int on)  { g_force_portable = on ? 1 : 0; }
void lr_simd_mask_aes_accel(int on)  { g_mask_aes_accel = on ? 1 : 0; }

const char* lr_simd_gcm_backend(void) {
    switch (gcm_path()) {
        case PATH_X86: return "aesni+pclmulqdq";
        case PATH_ARM: return "armv8-aes+pmull";
        default:       return "portable-c";
    }
}
const char* lr_simd_chap_backend(void) {
    switch (chap_path()) {
        case PATH_X86: return "avx2";
        case PATH_ARM: return "neon";
        default:       return "portable-c";
    }
}

/* ======================================================================== */
/* AES-256-GCM                                                              */
/* ======================================================================== */

struct lr_gcm_ctx {
    /* x86 / ARM keep expanded round keys as 16-byte blocks; the portable path
     * uses the same storage, so one struct serves all three. */
    uint8_t rk[15][16];
    uint8_t H[16];            /* AES_K(0), the GHASH subkey, network order */
    /* Powers of H for the multi-block path, in the reflected representation the
     * carry-less multiply wants. Stored as bytes so the struct has no vector
     * type and no alignment requirement the allocator cannot meet. */
    uint8_t Hp[4][16];        /* H^1 .. H^4 */
    /* Portable GHASH: 16-entry table of H * i for the low nibble, and the
     * matching high-nibble table. */
    uint64_t Hl[16], Hh[16];
    int path;
};

/* ---------------- portable AES ------------------------------------------ */

static const uint8_t kSbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static inline uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

static void aes256_expand_portable(const uint8_t key[32], uint8_t rk[15][16]) {
    uint8_t w[60][4];
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) w[i][j] = key[4 * i + j];
    uint8_t rcon = 1;
    for (int i = 8; i < 60; ++i) {
        uint8_t t[4];
        memcpy(t, w[i - 1], 4);
        if (i % 8 == 0) {
            uint8_t tmp = t[0];
            t[0] = (uint8_t)(kSbox[t[1]] ^ rcon);
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[tmp];
            rcon = xtime(rcon);
        } else if (i % 8 == 4) {
            for (int j = 0; j < 4; ++j) t[j] = kSbox[t[j]];
        }
        for (int j = 0; j < 4; ++j) w[i][j] = (uint8_t)(w[i - 8][j] ^ t[j]);
    }
    for (int r = 0; r < 15; ++r)
        for (int c = 0; c < 4; ++c)
            for (int j = 0; j < 4; ++j) rk[r][4 * c + j] = w[4 * r + c][j];
}

static void aes256_encrypt_portable(const uint8_t rk[15][16],
                                    const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = (uint8_t)(in[i] ^ rk[0][i]);
    for (int r = 1; r <= 14; ++r) {
        uint8_t t[16];
        /* SubBytes + ShiftRows, in the column-major byte order AES uses. */
        for (int c = 0; c < 4; ++c)
            for (int j = 0; j < 4; ++j)
                t[4 * c + j] = kSbox[s[4 * ((c + j) & 3) + j]];
        if (r != 14) {
            for (int c = 0; c < 4; ++c) {
                uint8_t a0 = t[4 * c + 0], a1 = t[4 * c + 1];
                uint8_t a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                uint8_t x = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                s[4 * c + 0] = (uint8_t)(a0 ^ x ^ xtime((uint8_t)(a0 ^ a1)));
                s[4 * c + 1] = (uint8_t)(a1 ^ x ^ xtime((uint8_t)(a1 ^ a2)));
                s[4 * c + 2] = (uint8_t)(a2 ^ x ^ xtime((uint8_t)(a2 ^ a3)));
                s[4 * c + 3] = (uint8_t)(a3 ^ x ^ xtime((uint8_t)(a3 ^ a0)));
            }
        } else {
            memcpy(s, t, 16);
        }
        for (int i = 0; i < 16; ++i) s[i] ^= rk[r][i];
    }
    memcpy(out, s, 16);
}

/* Portable GHASH: 4-bit tables, the standard shoup-style split. */
static void ghash_tables_init(lr_gcm_ctx* c) {
    uint64_t hi = 0, lo = 0;
    for (int i = 0; i < 8; ++i) hi = (hi << 8) | c->H[i];
    for (int i = 8; i < 16; ++i) lo = (lo << 8) | c->H[i];
    c->Hl[0] = 0; c->Hh[0] = 0;
    c->Hl[8] = lo; c->Hh[8] = hi;
    for (int i = 4; i > 0; i >>= 1) {
        uint32_t t = (uint32_t)(lo & 1) * 0xe1000000u;
        lo = (lo >> 1) | (hi << 63);
        hi = (hi >> 1) ^ ((uint64_t)t << 32);
        c->Hl[i] = lo; c->Hh[i] = hi;
    }
    for (int i = 2; i <= 8; i *= 2) {
        uint64_t* hl = c->Hl; uint64_t* hh = c->Hh;
        uint64_t vh = hh[i], vl = hl[i];
        for (int j = 1; j < i; ++j) { hh[i + j] = vh ^ hh[j]; hl[i + j] = vl ^ hl[j]; }
    }
}

static const uint64_t kLast4[16] = {
    0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0};

static void ghash_block_portable(const lr_gcm_ctx* c, uint8_t x[16]) {
    uint64_t zh, zl;
    uint8_t lo = (uint8_t)(x[15] & 0x0f);
    zh = c->Hh[lo]; zl = c->Hl[lo];
    for (int i = 15; i >= 0; --i) {
        uint8_t rem;
        uint8_t hi_n = (uint8_t)(x[i] >> 4);
        uint8_t lo_n = (uint8_t)(x[i] & 0x0f);
        if (i != 15) {
            rem = (uint8_t)(zl & 0x0f);
            zl = (zl >> 4) | (zh << 60);
            zh = (zh >> 4);
            zh ^= kLast4[rem] << 48;
            zh ^= c->Hh[lo_n]; zl ^= c->Hl[lo_n];
        }
        rem = (uint8_t)(zl & 0x0f);
        zl = (zl >> 4) | (zh << 60);
        zh = (zh >> 4);
        zh ^= kLast4[rem] << 48;
        zh ^= c->Hh[hi_n]; zl ^= c->Hl[hi_n];
    }
    for (int i = 0; i < 8; ++i) x[i] = (uint8_t)(zh >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i) x[8 + i] = (uint8_t)(zl >> (56 - 8 * i));
}

static void ghash_update_portable(const lr_gcm_ctx* c, uint8_t y[16],
                                  const uint8_t* data, size_t len) {
    while (len >= 16) {
        for (int i = 0; i < 16; ++i) y[i] ^= data[i];
        ghash_block_portable(c, y);
        data += 16; len -= 16;
    }
    if (len) {
        for (size_t i = 0; i < len; ++i) y[i] ^= data[i];
        ghash_block_portable(c, y);
    }
}

/* ---------------- x86-64 AES-NI + PCLMULQDQ ----------------------------- */

#if defined(__x86_64__) || defined(__i386__)

#define TGT_AES  __attribute__((target("aes,pclmul,ssse3,sse4.1")))
#define TGT_AVX2 __attribute__((target("avx2")))

TGT_AES static inline __m128i aes256_assist1(__m128i a, __m128i b) {
    __m128i t;
    b = _mm_shuffle_epi32(b, 0xff);
    t = _mm_slli_si128(a, 4); a = _mm_xor_si128(a, t);
    t = _mm_slli_si128(t, 4); a = _mm_xor_si128(a, t);
    t = _mm_slli_si128(t, 4); a = _mm_xor_si128(a, t);
    return _mm_xor_si128(a, b);
}
TGT_AES static inline __m128i aes256_assist2(__m128i a, __m128i c) {
    __m128i b = _mm_aeskeygenassist_si128(a, 0x00);
    __m128i t = _mm_shuffle_epi32(b, 0xaa);
    __m128i x = _mm_slli_si128(c, 4); c = _mm_xor_si128(c, x);
    x = _mm_slli_si128(x, 4); c = _mm_xor_si128(c, x);
    x = _mm_slli_si128(x, 4); c = _mm_xor_si128(c, x);
    return _mm_xor_si128(c, t);
}

TGT_AES static void aes256_expand_x86(const uint8_t key[32], uint8_t rkb[15][16]) {
    __m128i rk[15];
    rk[0] = _mm_loadu_si128((const __m128i*)key);
    rk[1] = _mm_loadu_si128((const __m128i*)(key + 16));
    rk[2]  = aes256_assist1(rk[0],  _mm_aeskeygenassist_si128(rk[1],  0x01));
    rk[3]  = aes256_assist2(rk[2],  rk[1]);
    rk[4]  = aes256_assist1(rk[2],  _mm_aeskeygenassist_si128(rk[3],  0x02));
    rk[5]  = aes256_assist2(rk[4],  rk[3]);
    rk[6]  = aes256_assist1(rk[4],  _mm_aeskeygenassist_si128(rk[5],  0x04));
    rk[7]  = aes256_assist2(rk[6],  rk[5]);
    rk[8]  = aes256_assist1(rk[6],  _mm_aeskeygenassist_si128(rk[7],  0x08));
    rk[9]  = aes256_assist2(rk[8],  rk[7]);
    rk[10] = aes256_assist1(rk[8],  _mm_aeskeygenassist_si128(rk[9],  0x10));
    rk[11] = aes256_assist2(rk[10], rk[9]);
    rk[12] = aes256_assist1(rk[10], _mm_aeskeygenassist_si128(rk[11], 0x20));
    rk[13] = aes256_assist2(rk[12], rk[11]);
    rk[14] = aes256_assist1(rk[12], _mm_aeskeygenassist_si128(rk[13], 0x40));
    for (int i = 0; i < 15; ++i) _mm_storeu_si128((__m128i*)rkb[i], rk[i]);
}

TGT_AES static inline __m128i aes256_enc_x86(const uint8_t rkb[15][16], __m128i b) {
    b = _mm_xor_si128(b, _mm_loadu_si128((const __m128i*)rkb[0]));
    for (int r = 1; r < 14; ++r)
        b = _mm_aesenc_si128(b, _mm_loadu_si128((const __m128i*)rkb[r]));
    return _mm_aesenclast_si128(b, _mm_loadu_si128((const __m128i*)rkb[14]));
}

/* Four blocks in flight. AES-NI has a latency of about four cycles and a
 * throughput of one per cycle, so a single-block loop runs at a quarter of the
 * unit's rate; four independent chains fill it. */
TGT_AES static inline void aes256_enc4_x86(const uint8_t rkb[15][16], __m128i b[4]) {
    __m128i k = _mm_loadu_si128((const __m128i*)rkb[0]);
    for (int i = 0; i < 4; ++i) b[i] = _mm_xor_si128(b[i], k);
    for (int r = 1; r < 14; ++r) {
        k = _mm_loadu_si128((const __m128i*)rkb[r]);
        for (int i = 0; i < 4; ++i) b[i] = _mm_aesenc_si128(b[i], k);
    }
    k = _mm_loadu_si128((const __m128i*)rkb[14]);
    for (int i = 0; i < 4; ++i) b[i] = _mm_aesenclast_si128(b[i], k);
}

static const uint8_t kBswapMask[16] = {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0};

/* Accumulate the 256-bit carry-less product of a and b into (lo, mid, hi).
 * Both the following shift and the reduction are linear over XOR, so several
 * products can be summed here and reduced once, which is where the four-block
 * GHASH gets its speed. */
TGT_AES static inline void clmul_acc(__m128i a, __m128i b,
                                     __m128i* lo, __m128i* mid, __m128i* hi) {
    __m128i t0 = _mm_clmulepi64_si128(a, b, 0x00);
    __m128i t1 = _mm_clmulepi64_si128(a, b, 0x01);
    __m128i t2 = _mm_clmulepi64_si128(a, b, 0x10);
    __m128i t3 = _mm_clmulepi64_si128(a, b, 0x11);
    *lo  = _mm_xor_si128(*lo, t0);
    *mid = _mm_xor_si128(*mid, _mm_xor_si128(t1, t2));
    *hi  = _mm_xor_si128(*hi, t3);
}

/* Fold the middle 128 bits in, shift left by one (the GCM convention places the
 * polynomial's coefficients in the reversed bit order) and reduce modulo
 * x^128 + x^7 + x^2 + x + 1. */
TGT_AES static inline __m128i gf_finish(__m128i lo, __m128i mid, __m128i hi) {
    lo = _mm_xor_si128(lo, _mm_slli_si128(mid, 8));
    hi = _mm_xor_si128(hi, _mm_srli_si128(mid, 8));

    __m128i t4 = _mm_srli_epi32(lo, 31);
    __m128i t5 = _mm_srli_epi32(hi, 31);
    lo = _mm_slli_epi32(lo, 1);
    hi = _mm_slli_epi32(hi, 1);
    __m128i t6 = _mm_srli_si128(t4, 12);
    t5 = _mm_slli_si128(t5, 4);
    t4 = _mm_slli_si128(t4, 4);
    lo = _mm_or_si128(lo, t4);
    hi = _mm_or_si128(hi, _mm_or_si128(t5, t6));

    __m128i t7 = _mm_slli_epi32(lo, 31);
    __m128i t8 = _mm_slli_epi32(lo, 30);
    __m128i t9 = _mm_slli_epi32(lo, 25);
    t7 = _mm_xor_si128(t7, _mm_xor_si128(t8, t9));
    t8 = _mm_srli_si128(t7, 4);
    t7 = _mm_slli_si128(t7, 12);
    lo = _mm_xor_si128(lo, t7);

    __m128i u1 = _mm_srli_epi32(lo, 1);
    __m128i u2 = _mm_srli_epi32(lo, 2);
    __m128i u3 = _mm_srli_epi32(lo, 7);
    u1 = _mm_xor_si128(u1, _mm_xor_si128(u2, u3));
    u1 = _mm_xor_si128(u1, t8);
    lo = _mm_xor_si128(lo, u1);
    return _mm_xor_si128(hi, lo);
}

TGT_AES static inline __m128i gf_mul_x86(__m128i a, __m128i b) {
    __m128i lo = _mm_setzero_si128(), mid = _mm_setzero_si128(), hi = _mm_setzero_si128();
    clmul_acc(a, b, &lo, &mid, &hi);
    return gf_finish(lo, mid, hi);
}

TGT_AES static void gcm_init_x86(lr_gcm_ctx* c) {
    const __m128i bs = _mm_loadu_si128((const __m128i*)kBswapMask);
    __m128i H = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)c->H), bs);
    __m128i p = H;
    _mm_storeu_si128((__m128i*)c->Hp[0], p);
    for (int i = 1; i < 4; ++i) {
        p = gf_mul_x86(p, H);
        _mm_storeu_si128((__m128i*)c->Hp[i], p);
    }
}

TGT_AES static void ghash_x86(const lr_gcm_ctx* c, uint8_t y[16],
                              const uint8_t* data, size_t len) {
    const __m128i bs = _mm_loadu_si128((const __m128i*)kBswapMask);
    const __m128i H1 = _mm_loadu_si128((const __m128i*)c->Hp[0]);
    const __m128i H2 = _mm_loadu_si128((const __m128i*)c->Hp[1]);
    const __m128i H3 = _mm_loadu_si128((const __m128i*)c->Hp[2]);
    const __m128i H4 = _mm_loadu_si128((const __m128i*)c->Hp[3]);
    __m128i X = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)y), bs);

    while (len >= 64) {
        __m128i b0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data +  0)), bs);
        __m128i b1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 16)), bs);
        __m128i b2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 32)), bs);
        __m128i b3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 48)), bs);
        b0 = _mm_xor_si128(b0, X);
        __m128i lo = _mm_setzero_si128(), mid = _mm_setzero_si128(), hi = _mm_setzero_si128();
        clmul_acc(b0, H4, &lo, &mid, &hi);
        clmul_acc(b1, H3, &lo, &mid, &hi);
        clmul_acc(b2, H2, &lo, &mid, &hi);
        clmul_acc(b3, H1, &lo, &mid, &hi);
        X = gf_finish(lo, mid, hi);
        data += 64; len -= 64;
    }
    while (len >= 16) {
        __m128i b = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)data), bs);
        X = gf_mul_x86(_mm_xor_si128(X, b), H1);
        data += 16; len -= 16;
    }
    if (len) {
        uint8_t tail[16] = {0};
        memcpy(tail, data, len);
        __m128i b = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)tail), bs);
        X = gf_mul_x86(_mm_xor_si128(X, b), H1);
    }
    _mm_storeu_si128((__m128i*)y, _mm_shuffle_epi8(X, bs));
}

/* AES-CTR over the GCM counter, four blocks at a time. */
TGT_AES static void aes_ctr_x86(const uint8_t rkb[15][16], uint8_t ctr[16],
                                const uint8_t* in, uint8_t* out, size_t len) {
    while (len >= 64) {
        __m128i b[4];
        for (int i = 0; i < 4; ++i) {
            b[i] = _mm_loadu_si128((const __m128i*)ctr);
            uint32_t v = ((uint32_t)ctr[12] << 24) | ((uint32_t)ctr[13] << 16) |
                         ((uint32_t)ctr[14] << 8) | ctr[15];
            v++;
            ctr[12] = (uint8_t)(v >> 24); ctr[13] = (uint8_t)(v >> 16);
            ctr[14] = (uint8_t)(v >> 8);  ctr[15] = (uint8_t)v;
        }
        aes256_enc4_x86(rkb, b);
        for (int i = 0; i < 4; ++i) {
            __m128i d = _mm_loadu_si128((const __m128i*)(in + 16 * i));
            _mm_storeu_si128((__m128i*)(out + 16 * i), _mm_xor_si128(d, b[i]));
        }
        in += 64; out += 64; len -= 64;
    }
    while (len) {
        __m128i ks = aes256_enc_x86(rkb, _mm_loadu_si128((const __m128i*)ctr));
        uint32_t v = ((uint32_t)ctr[12] << 24) | ((uint32_t)ctr[13] << 16) |
                     ((uint32_t)ctr[14] << 8) | ctr[15];
        v++;
        ctr[12] = (uint8_t)(v >> 24); ctr[13] = (uint8_t)(v >> 16);
        ctr[14] = (uint8_t)(v >> 8);  ctr[15] = (uint8_t)v;
        uint8_t kb[16];
        _mm_storeu_si128((__m128i*)kb, ks);
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(in[i] ^ kb[i]);
        in += n; out += n; len -= n;
    }
}

/* Counter mode and GHASH in one pass.
 *
 * Running them separately costs two traversals of the payload and leaves the
 * AES unit idle while the carry-less multiplier works and the other way round.
 * Interleaving them halves the memory traffic and lets the two units overlap,
 * which on this hot path is worth more than any further tuning inside either
 * one. The AES pipeline wants four independent blocks in flight, and four
 * ciphertext blocks are exactly what the aggregated GHASH consumes, so the two
 * unroll factors coincide.
 */
TGT_AES static void gcm_stitch_x86(const lr_gcm_ctx* c, uint8_t ctr[16], uint8_t y[16],
                                   const uint8_t* in, uint8_t* out, size_t len,
                                   int encrypt) {
    const __m128i bs = _mm_loadu_si128((const __m128i*)kBswapMask);
    const __m128i H1 = _mm_loadu_si128((const __m128i*)c->Hp[0]);
    const __m128i H2 = _mm_loadu_si128((const __m128i*)c->Hp[1]);
    const __m128i H3 = _mm_loadu_si128((const __m128i*)c->Hp[2]);
    const __m128i H4 = _mm_loadu_si128((const __m128i*)c->Hp[3]);
    __m128i X = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)y), bs);

    uint32_t ctr32 = ((uint32_t)ctr[12] << 24) | ((uint32_t)ctr[13] << 16) |
                     ((uint32_t)ctr[14] << 8) | ctr[15];
    __m128i base = _mm_loadu_si128((const __m128i*)ctr);

    while (len >= 64) {
        __m128i b[4];
        for (int i = 0; i < 4; ++i) {
            uint32_t v = ctr32 + (uint32_t)i;
            b[i] = _mm_insert_epi32(base, (int)__builtin_bswap32(v), 3);
        }
        ctr32 += 4;
        aes256_enc4_x86(c->rk, b);

        __m128i g[4];
        for (int i = 0; i < 4; ++i) {
            __m128i d = _mm_loadu_si128((const __m128i*)(in + 16 * i));
            __m128i e = _mm_xor_si128(d, b[i]);
            _mm_storeu_si128((__m128i*)(out + 16 * i), e);
            g[i] = _mm_shuffle_epi8(encrypt ? e : d, bs);
        }
        g[0] = _mm_xor_si128(g[0], X);
        __m128i lo = _mm_setzero_si128(), mid = _mm_setzero_si128(), hi = _mm_setzero_si128();
        clmul_acc(g[0], H4, &lo, &mid, &hi);
        clmul_acc(g[1], H3, &lo, &mid, &hi);
        clmul_acc(g[2], H2, &lo, &mid, &hi);
        clmul_acc(g[3], H1, &lo, &mid, &hi);
        X = gf_finish(lo, mid, hi);

        in += 64; out += 64; len -= 64;
    }
    while (len) {
        __m128i cb = _mm_insert_epi32(base, (int)__builtin_bswap32(ctr32), 3);
        ctr32++;
        __m128i ks = aes256_enc_x86(c->rk, cb);
        uint8_t kb[16], blk[16] = {0};
        _mm_storeu_si128((__m128i*)kb, ks);
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; ++i) {
            uint8_t o = (uint8_t)(in[i] ^ kb[i]);
            blk[i] = encrypt ? o : in[i];
            out[i] = o;
        }
        __m128i g0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)blk), bs);
        X = gf_mul_x86(_mm_xor_si128(X, g0), H1);
        in += n; out += n; len -= n;
    }
    ctr[12] = (uint8_t)(ctr32 >> 24); ctr[13] = (uint8_t)(ctr32 >> 16);
    ctr[14] = (uint8_t)(ctr32 >> 8);  ctr[15] = (uint8_t)ctr32;
    _mm_storeu_si128((__m128i*)y, _mm_shuffle_epi8(X, bs));
}
#endif /* x86 */

/* ---------------- AArch64 AES + PMULL ----------------------------------- */

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)

static void aes256_expand_arm(const uint8_t key[32], uint8_t rkb[15][16]) {
    /* The ARM AES instructions do not include a key-expansion helper, so the
     * schedule is computed with the portable code and only the rounds are
     * accelerated. Expansion happens once per connection; the rounds happen
     * once per block. */
    aes256_expand_portable(key, rkb);
}

static inline uint8x16_t aes256_enc_arm(const uint8_t rkb[15][16], uint8x16_t b) {
    for (int r = 0; r < 13; ++r) {
        b = vaeseq_u8(b, vld1q_u8(rkb[r]));
        b = vaesmcq_u8(b);
    }
    b = vaeseq_u8(b, vld1q_u8(rkb[13]));
    return veorq_u8(b, vld1q_u8(rkb[14]));
}

static inline void aes256_enc4_arm(const uint8_t rkb[15][16], uint8x16_t b[4]) {
    for (int r = 0; r < 13; ++r) {
        uint8x16_t k = vld1q_u8(rkb[r]);
        for (int i = 0; i < 4; ++i) { b[i] = vaeseq_u8(b[i], k); b[i] = vaesmcq_u8(b[i]); }
    }
    uint8x16_t k13 = vld1q_u8(rkb[13]), k14 = vld1q_u8(rkb[14]);
    for (int i = 0; i < 4; ++i) b[i] = veorq_u8(vaeseq_u8(b[i], k13), k14);
}

/* GHASH with PMULL.
 *
 * The x86 path above was verified byte for byte against OpenSSL, so rather than
 * transcribing a second, differently-structured ARM algorithm and hoping, this
 * mirrors it exactly: the same byte-swapped representation, the same four
 * carry-less products accumulated before a single reduction, and the same
 * shift-by-one and reduction sequence written in NEON. Two implementations that
 * differ only in which instruction set spells each step are far easier to trust
 * than two that differ in mathematics as well, and the self-test checks the
 * result against OpenSSL on whichever machine it runs.
 */
static inline uint8x16_t neon_bswap128(uint8x16_t x) {
    uint8x16_t r = vrev64q_u8(x);
    return vextq_u8(r, r, 8);
}

static inline void clmul_acc_arm(uint8x16_t a8, uint8x16_t b8,
                                 uint8x16_t* lo, uint8x16_t* mid, uint8x16_t* hi) {
    poly64x2_t ap = vreinterpretq_p64_u8(a8), bp = vreinterpretq_p64_u8(b8);
    poly64_t a0 = (poly64_t)vgetq_lane_u64(vreinterpretq_u64_u8(a8), 0);
    poly64_t a1 = (poly64_t)vgetq_lane_u64(vreinterpretq_u64_u8(a8), 1);
    poly64_t b0 = (poly64_t)vgetq_lane_u64(vreinterpretq_u64_u8(b8), 0);
    poly64_t b1 = (poly64_t)vgetq_lane_u64(vreinterpretq_u64_u8(b8), 1);
    uint8x16_t t0 = vreinterpretq_u8_p128(vmull_p64(a0, b0));
    uint8x16_t t1 = vreinterpretq_u8_p128(vmull_p64(a1, b0));
    uint8x16_t t2 = vreinterpretq_u8_p128(vmull_p64(a0, b1));
    uint8x16_t t3 = vreinterpretq_u8_p128(vmull_high_p64(ap, bp));
    *lo  = veorq_u8(*lo, t0);
    *mid = veorq_u8(*mid, veorq_u8(t1, t2));
    *hi  = veorq_u8(*hi, t3);
}

static inline uint8x16_t gf_finish_arm(uint8x16_t lo, uint8x16_t mid, uint8x16_t hi) {
    const uint8x16_t z = vdupq_n_u8(0);
    lo = veorq_u8(lo, vextq_u8(z, mid, 8));
    hi = veorq_u8(hi, vextq_u8(mid, z, 8));

    uint32x4_t l32 = vreinterpretq_u32_u8(lo), h32 = vreinterpretq_u32_u8(hi);
    uint8x16_t t4 = vreinterpretq_u8_u32(vshrq_n_u32(l32, 31));
    uint8x16_t t5 = vreinterpretq_u8_u32(vshrq_n_u32(h32, 31));
    l32 = vshlq_n_u32(l32, 1);
    h32 = vshlq_n_u32(h32, 1);
    uint8x16_t t6 = vextq_u8(t4, z, 12);
    t5 = vextq_u8(z, t5, 12);
    t4 = vextq_u8(z, t4, 12);
    lo = vorrq_u8(vreinterpretq_u8_u32(l32), t4);
    hi = vorrq_u8(vreinterpretq_u8_u32(h32), vorrq_u8(t5, t6));

    l32 = vreinterpretq_u32_u8(lo);
    uint8x16_t t7 = vreinterpretq_u8_u32(
        veorq_u32(vshlq_n_u32(l32, 31),
                  veorq_u32(vshlq_n_u32(l32, 30), vshlq_n_u32(l32, 25))));
    uint8x16_t t8 = vextq_u8(t7, z, 4);
    t7 = vextq_u8(z, t7, 4);
    lo = veorq_u8(lo, t7);

    l32 = vreinterpretq_u32_u8(lo);
    uint8x16_t u = vreinterpretq_u8_u32(
        veorq_u32(vshrq_n_u32(l32, 1),
                  veorq_u32(vshrq_n_u32(l32, 2), vshrq_n_u32(l32, 7))));
    u  = veorq_u8(u, t8);
    lo = veorq_u8(lo, u);
    return veorq_u8(hi, lo);
}

static inline uint8x16_t gf_mul_arm(uint8x16_t a, uint8x16_t b) {
    uint8x16_t lo = vdupq_n_u8(0), mid = vdupq_n_u8(0), hi = vdupq_n_u8(0);
    clmul_acc_arm(a, b, &lo, &mid, &hi);
    return gf_finish_arm(lo, mid, hi);
}

static void gcm_init_arm(lr_gcm_ctx* c) {
    uint8x16_t H = neon_bswap128(vld1q_u8(c->H));
    uint8x16_t p = H;
    vst1q_u8(c->Hp[0], p);
    for (int i = 1; i < 4; ++i) { p = gf_mul_arm(p, H); vst1q_u8(c->Hp[i], p); }
}

static void ghash_arm(const lr_gcm_ctx* c, uint8_t y[16],
                      const uint8_t* data, size_t len) {
    const uint8x16_t H1 = vld1q_u8(c->Hp[0]);
    const uint8x16_t H2 = vld1q_u8(c->Hp[1]);
    const uint8x16_t H3 = vld1q_u8(c->Hp[2]);
    const uint8x16_t H4 = vld1q_u8(c->Hp[3]);
    uint8x16_t X = neon_bswap128(vld1q_u8(y));
    while (len >= 64) {
        uint8x16_t b0 = neon_bswap128(vld1q_u8(data +  0));
        uint8x16_t b1 = neon_bswap128(vld1q_u8(data + 16));
        uint8x16_t b2 = neon_bswap128(vld1q_u8(data + 32));
        uint8x16_t b3 = neon_bswap128(vld1q_u8(data + 48));
        b0 = veorq_u8(b0, X);
        uint8x16_t lo = vdupq_n_u8(0), mid = vdupq_n_u8(0), hi = vdupq_n_u8(0);
        clmul_acc_arm(b0, H4, &lo, &mid, &hi);
        clmul_acc_arm(b1, H3, &lo, &mid, &hi);
        clmul_acc_arm(b2, H2, &lo, &mid, &hi);
        clmul_acc_arm(b3, H1, &lo, &mid, &hi);
        X = gf_finish_arm(lo, mid, hi);
        data += 64; len -= 64;
    }
    while (len >= 16) {
        X = gf_mul_arm(veorq_u8(X, neon_bswap128(vld1q_u8(data))), H1);
        data += 16; len -= 16;
    }
    if (len) {
        uint8_t tail[16] = {0};
        memcpy(tail, data, len);
        X = gf_mul_arm(veorq_u8(X, neon_bswap128(vld1q_u8(tail))), H1);
    }
    vst1q_u8(y, neon_bswap128(X));
}

static void aes_ctr_arm(const uint8_t rkb[15][16], uint8_t ctr[16],
                        const uint8_t* in, uint8_t* out, size_t len) {
    while (len >= 64) {
        uint8x16_t b[4];
        for (int i = 0; i < 4; ++i) {
            b[i] = vld1q_u8(ctr);
            uint32_t v = ((uint32_t)ctr[12] << 24) | ((uint32_t)ctr[13] << 16) |
                         ((uint32_t)ctr[14] << 8) | ctr[15];
            v++;
            ctr[12] = (uint8_t)(v >> 24); ctr[13] = (uint8_t)(v >> 16);
            ctr[14] = (uint8_t)(v >> 8);  ctr[15] = (uint8_t)v;
        }
        aes256_enc4_arm(rkb, b);
        for (int i = 0; i < 4; ++i)
            vst1q_u8(out + 16 * i, veorq_u8(vld1q_u8(in + 16 * i), b[i]));
        in += 64; out += 64; len -= 64;
    }
    while (len) {
        uint8x16_t ks = aes256_enc_arm(rkb, vld1q_u8(ctr));
        uint32_t v = ((uint32_t)ctr[12] << 24) | ((uint32_t)ctr[13] << 16) |
                     ((uint32_t)ctr[14] << 8) | ctr[15];
        v++;
        ctr[12] = (uint8_t)(v >> 24); ctr[13] = (uint8_t)(v >> 16);
        ctr[14] = (uint8_t)(v >> 8);  ctr[15] = (uint8_t)v;
        uint8_t kb[16];
        vst1q_u8(kb, ks);
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(in[i] ^ kb[i]);
        in += n; out += n; len -= n;
    }
}
#endif /* aarch64 + crypto */

/* ---------------- portable CTR ------------------------------------------ */

static void aes_ctr_portable(const uint8_t rkb[15][16], uint8_t ctr[16],
                             const uint8_t* in, uint8_t* out, size_t len) {
    while (len) {
        uint8_t ks[16];
        aes256_encrypt_portable(rkb, ctr, ks);
        uint32_t v = ((uint32_t)ctr[12] << 24) | ((uint32_t)ctr[13] << 16) |
                     ((uint32_t)ctr[14] << 8) | ctr[15];
        v++;
        ctr[12] = (uint8_t)(v >> 24); ctr[13] = (uint8_t)(v >> 16);
        ctr[14] = (uint8_t)(v >> 8);  ctr[15] = (uint8_t)v;
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(in[i] ^ ks[i]);
        in += n; out += n; len -= n;
    }
}

/* ---------------- dispatch wrappers ------------------------------------- */

static void gcm_ctr(const lr_gcm_ctx* c, uint8_t ctr[16],
                    const uint8_t* in, uint8_t* out, size_t len) {
#if defined(__x86_64__) || defined(__i386__)
    if (c->path == PATH_X86) { aes_ctr_x86(c->rk, ctr, in, out, len); return; }
#endif
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
    if (c->path == PATH_ARM) { aes_ctr_arm(c->rk, ctr, in, out, len); return; }
#endif
    aes_ctr_portable(c->rk, ctr, in, out, len);
}

static void gcm_ghash(const lr_gcm_ctx* c, uint8_t y[16],
                      const uint8_t* data, size_t len) {
#if defined(__x86_64__) || defined(__i386__)
    if (c->path == PATH_X86) { ghash_x86(c, y, data, len); return; }
#endif
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
    if (c->path == PATH_ARM) { ghash_arm(c, y, data, len); return; }
#endif
    ghash_update_portable(c, y, data, len);
}

/* ---------------- GCM public entry points ------------------------------- */

lr_gcm_ctx* lr_gcm_new(const uint8_t key[32]) {
    lr_gcm_ctx* c = (lr_gcm_ctx*)calloc(1, sizeof(lr_gcm_ctx));
    if (!c) return NULL;
    c->path = gcm_path();
    uint8_t zero[16] = {0};
#if defined(__x86_64__) || defined(__i386__)
    if (c->path == PATH_X86) {
        aes256_expand_x86(key, c->rk);
        __m128i h = aes256_enc_x86(c->rk, _mm_setzero_si128());
        _mm_storeu_si128((__m128i*)c->H, h);
        gcm_init_x86(c);
        return c;
    }
#endif
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
    if (c->path == PATH_ARM) {
        aes256_expand_arm(key, c->rk);
        vst1q_u8(c->H, aes256_enc_arm(c->rk, vld1q_u8(zero)));
        gcm_init_arm(c);
        return c;
    }
#endif
    aes256_expand_portable(key, c->rk);
    aes256_encrypt_portable(c->rk, zero, c->H);
    ghash_tables_init(c);
    return c;
}

void lr_gcm_free(lr_gcm_ctx* c) { free(c); }

static void gcm_lengths(uint8_t out[16], size_t aad_len, size_t ct_len) {
    store64_be(out, (uint64_t)aad_len * 8);
    store64_be(out + 8, (uint64_t)ct_len * 8);
}

/* One core routine for both directions. The order matters: on encryption GHASH
 * runs over the ciphertext the counter mode has just produced, on decryption it
 * runs over the ciphertext that is still the input. Doing it in the wrong order
 * would work only as long as the caller never decrypts in place, which is the
 * kind of latent aliasing bug that survives every test until someone reuses a
 * buffer. */
static void gcm_core(lr_gcm_ctx* c, const uint8_t nonce[12],
                     const uint8_t* aad, size_t aad_len,
                     const uint8_t* in, uint8_t* out, size_t len,
                     int encrypt, uint8_t tag[16]) {
    uint8_t j0[16], ctr[16], mask[16], y[16] = {0}, lenblk[16];
    uint8_t zero[16] = {0};
    memcpy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    memcpy(ctr, j0, 16);
    /* E_K(J0) masks the tag; the keystream starts one counter later. Producing
     * it through the same counter-mode routine leaves ctr at inc32(J0). */
    gcm_ctr(c, ctr, zero, mask, 16);

    if (aad_len) gcm_ghash(c, y, aad, aad_len);
#if defined(__x86_64__) || defined(__i386__)
    if (c->path == PATH_X86) {
        if (len) gcm_stitch_x86(c, ctr, y, in, out, len, encrypt);
    } else
#endif
    {
        if (!encrypt && len) gcm_ghash(c, y, in, len);
        if (len)             gcm_ctr(c, ctr, in, out, len);
        if (encrypt && len)  gcm_ghash(c, y, out, len);
    }

    gcm_lengths(lenblk, aad_len, len);
    gcm_ghash(c, y, lenblk, 16);
    for (int i = 0; i < 16; ++i) tag[i] = (uint8_t)(y[i] ^ mask[i]);
}

size_t lr_gcm_seal(lr_gcm_ctx* c, const uint8_t nonce[12],
                   const uint8_t* aad, size_t aad_len,
                   const uint8_t* pt, size_t pt_len, uint8_t* out) {
    uint8_t tag[16];
    gcm_core(c, nonce, aad, aad_len, pt, out, pt_len, 1, tag);
    memcpy(out + pt_len, tag, 16);
    return pt_len + 16;
}

long lr_gcm_open(lr_gcm_ctx* c, const uint8_t nonce[12],
                 const uint8_t* aad, size_t aad_len,
                 const uint8_t* ct, size_t ct_len, uint8_t* out) {
    if (ct_len < 16) return -1;
    const size_t body = ct_len - 16;
    uint8_t tag[16];
    gcm_core(c, nonce, aad, aad_len, ct, out, body, 0, tag);
    if (!ct_eq16(tag, ct + body)) return -1;
    return (long)body;
}

void lr_aes256_ctr(const uint8_t key[32], const uint8_t iv[16],
                   const uint8_t* in, uint8_t* out, size_t len) {
    lr_gcm_ctx* c = lr_gcm_new(key);
    if (!c) return;
    uint8_t ctr[16];
    memcpy(ctr, iv, 16);
    gcm_ctr(c, ctr, in, out, len);
    lr_gcm_free(c);
}

/* ======================================================================== */
/* ChaCha20-Poly1305                                                        */
/* ======================================================================== */

struct lr_chap_ctx {
    uint8_t key[32];
    int     path;
};

static const char kSigma[17] = "expand 32-byte k";

/* ---------------- portable ChaCha20 ------------------------------------- */

#define QR(a, b, c, d)                        \
    a += b; d ^= a; d = rotl32(d, 16);        \
    c += d; b ^= c; b = rotl32(b, 12);        \
    a += b; d ^= a; d = rotl32(d, 8);         \
    c += d; b ^= c; b = rotl32(b, 7);

static void chacha_block_portable(const uint32_t st[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, st, sizeof(x));
    for (int i = 0; i < 10; ++i) {
        QR(x[0], x[4], x[8],  x[12])
        QR(x[1], x[5], x[9],  x[13])
        QR(x[2], x[6], x[10], x[14])
        QR(x[3], x[7], x[11], x[15])
        QR(x[0], x[5], x[10], x[15])
        QR(x[1], x[6], x[11], x[12])
        QR(x[2], x[7], x[8],  x[13])
        QR(x[3], x[4], x[9],  x[14])
    }
    for (int i = 0; i < 16; ++i) store32_le(out + 4 * i, x[i] + st[i]);
}

static void chacha_state(uint32_t st[16], const uint8_t key[32],
                         const uint8_t nonce[12], uint32_t counter) {
    for (int i = 0; i < 4; ++i) st[i] = load32_le((const uint8_t*)kSigma + 4 * i);
    for (int i = 0; i < 8; ++i) st[4 + i] = load32_le(key + 4 * i);
    st[12] = counter;
    for (int i = 0; i < 3; ++i) st[13 + i] = load32_le(nonce + 4 * i);
}

static void chacha_xor_portable(const uint8_t key[32], const uint8_t nonce[12],
                                uint32_t counter, const uint8_t* in,
                                uint8_t* out, size_t len) {
    uint32_t st[16];
    uint8_t ks[64];
    chacha_state(st, key, nonce, counter);
    while (len) {
        chacha_block_portable(st, ks);
        st[12]++;
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(in[i] ^ ks[i]);
        in += n; out += n; len -= n;
    }
}

/* ---------------- AVX2 ChaCha20, two blocks per iteration --------------- */

#if defined(__x86_64__) || defined(__i386__)

TGT_AVX2 static inline __m256i rotl_avx2_16(__m256i x) {
    const __m256i m = _mm256_set_epi8(13,12,15,14, 9,8,11,10, 5,4,7,6, 1,0,3,2,
                                      13,12,15,14, 9,8,11,10, 5,4,7,6, 1,0,3,2);
    return _mm256_shuffle_epi8(x, m);
}
TGT_AVX2 static inline __m256i rotl_avx2_8(__m256i x) {
    const __m256i m = _mm256_set_epi8(14,13,12,15, 10,9,8,11, 6,5,4,7, 2,1,0,3,
                                      14,13,12,15, 10,9,8,11, 6,5,4,7, 2,1,0,3);
    return _mm256_shuffle_epi8(x, m);
}
TGT_AVX2 static inline __m256i rotl_avx2(__m256i x, int n) {
    return _mm256_or_si256(_mm256_slli_epi32(x, n), _mm256_srli_epi32(x, 32 - n));
}

/* Each register holds one row of two independent ChaCha states: the low
 * 128-bit lane is block n, the high lane block n+1. All the shuffles used for
 * the diagonal step act within a lane, so two blocks come out for the price of
 * the shuffles of one. */
TGT_AVX2 static void chacha_2blocks_avx2(const uint32_t st[16], uint8_t out[128]) {
    __m128i s0 = _mm_loadu_si128((const __m128i*)(st + 0));
    __m128i s1 = _mm_loadu_si128((const __m128i*)(st + 4));
    __m128i s2 = _mm_loadu_si128((const __m128i*)(st + 8));
    __m128i s3 = _mm_loadu_si128((const __m128i*)(st + 12));
    __m128i s3b = _mm_add_epi32(s3, _mm_set_epi32(0, 0, 0, 1));

    __m256i a = _mm256_broadcastsi128_si256(s0);
    __m256i b = _mm256_broadcastsi128_si256(s1);
    __m256i c = _mm256_broadcastsi128_si256(s2);
    __m256i d = _mm256_inserti128_si256(_mm256_castsi128_si256(s3), s3b, 1);
    const __m256i a0 = a, b0 = b, c0 = c, d0 = d;

    for (int i = 0; i < 10; ++i) {
        a = _mm256_add_epi32(a, b); d = _mm256_xor_si256(d, a); d = rotl_avx2_16(d);
        c = _mm256_add_epi32(c, d); b = _mm256_xor_si256(b, c); b = rotl_avx2(b, 12);
        a = _mm256_add_epi32(a, b); d = _mm256_xor_si256(d, a); d = rotl_avx2_8(d);
        c = _mm256_add_epi32(c, d); b = _mm256_xor_si256(b, c); b = rotl_avx2(b, 7);

        b = _mm256_shuffle_epi32(b, 0x39);
        c = _mm256_shuffle_epi32(c, 0x4E);
        d = _mm256_shuffle_epi32(d, 0x93);

        a = _mm256_add_epi32(a, b); d = _mm256_xor_si256(d, a); d = rotl_avx2_16(d);
        c = _mm256_add_epi32(c, d); b = _mm256_xor_si256(b, c); b = rotl_avx2(b, 12);
        a = _mm256_add_epi32(a, b); d = _mm256_xor_si256(d, a); d = rotl_avx2_8(d);
        c = _mm256_add_epi32(c, d); b = _mm256_xor_si256(b, c); b = rotl_avx2(b, 7);

        b = _mm256_shuffle_epi32(b, 0x93);
        c = _mm256_shuffle_epi32(c, 0x4E);
        d = _mm256_shuffle_epi32(d, 0x39);
    }
    a = _mm256_add_epi32(a, a0);
    b = _mm256_add_epi32(b, b0);
    c = _mm256_add_epi32(c, c0);
    d = _mm256_add_epi32(d, d0);

    _mm_storeu_si128((__m128i*)(out +  0), _mm256_castsi256_si128(a));
    _mm_storeu_si128((__m128i*)(out + 16), _mm256_castsi256_si128(b));
    _mm_storeu_si128((__m128i*)(out + 32), _mm256_castsi256_si128(c));
    _mm_storeu_si128((__m128i*)(out + 48), _mm256_castsi256_si128(d));
    _mm_storeu_si128((__m128i*)(out + 64), _mm256_extracti128_si256(a, 1));
    _mm_storeu_si128((__m128i*)(out + 80), _mm256_extracti128_si256(b, 1));
    _mm_storeu_si128((__m128i*)(out + 96), _mm256_extracti128_si256(c, 1));
    _mm_storeu_si128((__m128i*)(out +112), _mm256_extracti128_si256(d, 1));
}

/* Four blocks per iteration: two independent two-block chains, so the shuffle
 * and add chains of one interleave with the other. The keystream is XORed
 * straight out of the registers rather than through a staging buffer, which
 * removes a store and a reload of every byte. */
TGT_AVX2 static inline void chacha_4blocks_avx2(const uint32_t st[16],
                                                const uint8_t* in, uint8_t* out) {
    __m128i s0 = _mm_loadu_si128((const __m128i*)(st + 0));
    __m128i s1 = _mm_loadu_si128((const __m128i*)(st + 4));
    __m128i s2 = _mm_loadu_si128((const __m128i*)(st + 8));
    __m128i s3 = _mm_loadu_si128((const __m128i*)(st + 12));

    __m256i a0 = _mm256_broadcastsi128_si256(s0);
    __m256i b0 = _mm256_broadcastsi128_si256(s1);
    __m256i c0 = _mm256_broadcastsi128_si256(s2);
    __m256i d0 = _mm256_inserti128_si256(_mm256_castsi128_si256(s3),
                                         _mm_add_epi32(s3, _mm_set_epi32(0,0,0,1)), 1);
    __m256i d1 = _mm256_add_epi32(d0, _mm256_set_epi32(0,0,0,2, 0,0,0,2));
    __m256i a = a0, b = b0, c = c0, d = d0;
    __m256i A = a0, B = b0, C = c0, D = d1;

    for (int i = 0; i < 10; ++i) {
        a = _mm256_add_epi32(a, b); d = _mm256_xor_si256(d, a); d = rotl_avx2_16(d);
        A = _mm256_add_epi32(A, B); D = _mm256_xor_si256(D, A); D = rotl_avx2_16(D);
        c = _mm256_add_epi32(c, d); b = _mm256_xor_si256(b, c); b = rotl_avx2(b, 12);
        C = _mm256_add_epi32(C, D); B = _mm256_xor_si256(B, C); B = rotl_avx2(B, 12);
        a = _mm256_add_epi32(a, b); d = _mm256_xor_si256(d, a); d = rotl_avx2_8(d);
        A = _mm256_add_epi32(A, B); D = _mm256_xor_si256(D, A); D = rotl_avx2_8(D);
        c = _mm256_add_epi32(c, d); b = _mm256_xor_si256(b, c); b = rotl_avx2(b, 7);
        C = _mm256_add_epi32(C, D); B = _mm256_xor_si256(B, C); B = rotl_avx2(B, 7);

        b = _mm256_shuffle_epi32(b, 0x39); c = _mm256_shuffle_epi32(c, 0x4E); d = _mm256_shuffle_epi32(d, 0x93);
        B = _mm256_shuffle_epi32(B, 0x39); C = _mm256_shuffle_epi32(C, 0x4E); D = _mm256_shuffle_epi32(D, 0x93);

        a = _mm256_add_epi32(a, b); d = _mm256_xor_si256(d, a); d = rotl_avx2_16(d);
        A = _mm256_add_epi32(A, B); D = _mm256_xor_si256(D, A); D = rotl_avx2_16(D);
        c = _mm256_add_epi32(c, d); b = _mm256_xor_si256(b, c); b = rotl_avx2(b, 12);
        C = _mm256_add_epi32(C, D); B = _mm256_xor_si256(B, C); B = rotl_avx2(B, 12);
        a = _mm256_add_epi32(a, b); d = _mm256_xor_si256(d, a); d = rotl_avx2_8(d);
        A = _mm256_add_epi32(A, B); D = _mm256_xor_si256(D, A); D = rotl_avx2_8(D);
        c = _mm256_add_epi32(c, d); b = _mm256_xor_si256(b, c); b = rotl_avx2(b, 7);
        C = _mm256_add_epi32(C, D); B = _mm256_xor_si256(B, C); B = rotl_avx2(B, 7);

        b = _mm256_shuffle_epi32(b, 0x93); c = _mm256_shuffle_epi32(c, 0x4E); d = _mm256_shuffle_epi32(d, 0x39);
        B = _mm256_shuffle_epi32(B, 0x93); C = _mm256_shuffle_epi32(C, 0x4E); D = _mm256_shuffle_epi32(D, 0x39);
    }
    a = _mm256_add_epi32(a, a0); b = _mm256_add_epi32(b, b0);
    c = _mm256_add_epi32(c, c0); d = _mm256_add_epi32(d, d0);
    A = _mm256_add_epi32(A, a0); B = _mm256_add_epi32(B, b0);
    C = _mm256_add_epi32(C, c0); D = _mm256_add_epi32(D, d1);

    /* Each register holds one row of two blocks; a pair of lane permutes turns
     * that back into consecutive 64-byte blocks. */
    __m256i o[8];
    o[0] = _mm256_permute2x128_si256(a, b, 0x20);
    o[1] = _mm256_permute2x128_si256(c, d, 0x20);
    o[2] = _mm256_permute2x128_si256(a, b, 0x31);
    o[3] = _mm256_permute2x128_si256(c, d, 0x31);
    o[4] = _mm256_permute2x128_si256(A, B, 0x20);
    o[5] = _mm256_permute2x128_si256(C, D, 0x20);
    o[6] = _mm256_permute2x128_si256(A, B, 0x31);
    o[7] = _mm256_permute2x128_si256(C, D, 0x31);
    for (int i = 0; i < 8; ++i)
        _mm256_storeu_si256((__m256i*)(out + 32 * i),
            _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(in + 32 * i)), o[i]));
}

TGT_AVX2 static void chacha_xor_avx2(const uint8_t key[32], const uint8_t nonce[12],
                                     uint32_t counter, const uint8_t* in,
                                     uint8_t* out, size_t len) {
    uint32_t st[16];
    chacha_state(st, key, nonce, counter);
    while (len >= 256) {
        chacha_4blocks_avx2(st, in, out);
        st[12] += 4;
        in += 256; out += 256; len -= 256;
    }
    if (len) {
        uint8_t zin[256] = {0}, zout[256];
        memcpy(zin, in, len);
        chacha_4blocks_avx2(st, zin, zout);
        st[12] += 4;
        memcpy(out, zout, len);
    }
}
#endif /* x86 */

/* ---------------- NEON ChaCha20 ----------------------------------------- */

#if defined(__aarch64__)
#define ROTL_NEON(x, n) vsriq_n_u32(vshlq_n_u32((x), (n)), (x), 32 - (n))

static void chacha_block_neon(const uint32_t st[16], uint8_t out[64]) {
    uint32x4_t a = vld1q_u32(st + 0), b = vld1q_u32(st + 4);
    uint32x4_t c = vld1q_u32(st + 8), d = vld1q_u32(st + 12);
    const uint32x4_t a0 = a, b0 = b, c0 = c, d0 = d;
    for (int i = 0; i < 10; ++i) {
        a = vaddq_u32(a, b); d = veorq_u32(d, a); d = ROTL_NEON(d, 16);
        c = vaddq_u32(c, d); b = veorq_u32(b, c); b = ROTL_NEON(b, 12);
        a = vaddq_u32(a, b); d = veorq_u32(d, a); d = ROTL_NEON(d, 8);
        c = vaddq_u32(c, d); b = veorq_u32(b, c); b = ROTL_NEON(b, 7);
        b = vextq_u32(b, b, 1); c = vextq_u32(c, c, 2); d = vextq_u32(d, d, 3);
        a = vaddq_u32(a, b); d = veorq_u32(d, a); d = ROTL_NEON(d, 16);
        c = vaddq_u32(c, d); b = veorq_u32(b, c); b = ROTL_NEON(b, 12);
        a = vaddq_u32(a, b); d = veorq_u32(d, a); d = ROTL_NEON(d, 8);
        c = vaddq_u32(c, d); b = veorq_u32(b, c); b = ROTL_NEON(b, 7);
        b = vextq_u32(b, b, 3); c = vextq_u32(c, c, 2); d = vextq_u32(d, d, 1);
    }
    vst1q_u32((uint32_t*)(out +  0), vaddq_u32(a, a0));
    vst1q_u32((uint32_t*)(out + 16), vaddq_u32(b, b0));
    vst1q_u32((uint32_t*)(out + 32), vaddq_u32(c, c0));
    vst1q_u32((uint32_t*)(out + 48), vaddq_u32(d, d0));
}

static void chacha_xor_neon(const uint8_t key[32], const uint8_t nonce[12],
                            uint32_t counter, const uint8_t* in,
                            uint8_t* out, size_t len) {
    uint32_t st[16];
    uint8_t ks[64];
    chacha_state(st, key, nonce, counter);
    while (len) {
        chacha_block_neon(st, ks);
        st[12]++;
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(in[i] ^ ks[i]);
        in += n; out += n; len -= n;
    }
}
#endif /* aarch64 */

static void chacha_xor(int path, const uint8_t key[32], const uint8_t nonce[12],
                       uint32_t counter, const uint8_t* in, uint8_t* out, size_t len) {
#if defined(__x86_64__) || defined(__i386__)
    if (path == PATH_X86) { chacha_xor_avx2(key, nonce, counter, in, out, len); return; }
#endif
#if defined(__aarch64__)
    if (path == PATH_ARM) { chacha_xor_neon(key, nonce, counter, in, out, len); return; }
#endif
    (void)path;
    chacha_xor_portable(key, nonce, counter, in, out, len);
}

void lr_chacha20_keystream(const uint8_t key[32], const uint8_t nonce[12],
                           uint32_t counter, uint8_t* out, size_t len) {
    memset(out, 0, len);
    chacha_xor(chap_path(), key, nonce, counter, out, out, len);
}

/* ---------------- Poly1305 ---------------------------------------------- */

typedef struct {
    uint32_t r[5], h[5], pad[4];
    size_t   leftover;
    uint8_t  buffer[16];
    int      final;
} poly1305_state;

static void poly1305_init(poly1305_state* st, const uint8_t key[32]) {
    st->r[0] = (load32_le(key + 0)     ) & 0x3ffffff;
    st->r[1] = (load32_le(key + 3) >> 2) & 0x3ffff03;
    st->r[2] = (load32_le(key + 6) >> 4) & 0x3ffc0ff;
    st->r[3] = (load32_le(key + 9) >> 6) & 0x3f03fff;
    st->r[4] = (load32_le(key +12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; ++i) st->h[i] = 0;
    for (int i = 0; i < 4; ++i) st->pad[i] = load32_le(key + 16 + 4 * i);
    st->leftover = 0;
    st->final = 0;
}

static void poly1305_blocks(poly1305_state* st, const uint8_t* m, size_t bytes) {
    const uint32_t hibit = st->final ? 0 : (1UL << 24);
    uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];

    while (bytes >= 16) {
        h0 += (load32_le(m + 0)     ) & 0x3ffffff;
        h1 += (load32_le(m + 3) >> 2) & 0x3ffffff;
        h2 += (load32_le(m + 6) >> 4) & 0x3ffffff;
        h3 += (load32_le(m + 9) >> 6) & 0x3ffffff;
        h4 += (load32_le(m +12) >> 8) | hibit;

        unsigned long long d0 = (unsigned long long)h0 * r0 + (unsigned long long)h1 * s4 +
                                (unsigned long long)h2 * s3 + (unsigned long long)h3 * s2 +
                                (unsigned long long)h4 * s1;
        unsigned long long d1 = (unsigned long long)h0 * r1 + (unsigned long long)h1 * r0 +
                                (unsigned long long)h2 * s4 + (unsigned long long)h3 * s3 +
                                (unsigned long long)h4 * s2;
        unsigned long long d2 = (unsigned long long)h0 * r2 + (unsigned long long)h1 * r1 +
                                (unsigned long long)h2 * r0 + (unsigned long long)h3 * s4 +
                                (unsigned long long)h4 * s3;
        unsigned long long d3 = (unsigned long long)h0 * r3 + (unsigned long long)h1 * r2 +
                                (unsigned long long)h2 * r1 + (unsigned long long)h3 * r0 +
                                (unsigned long long)h4 * s4;
        unsigned long long d4 = (unsigned long long)h0 * r4 + (unsigned long long)h1 * r3 +
                                (unsigned long long)h2 * r2 + (unsigned long long)h3 * r1 +
                                (unsigned long long)h4 * r0;

        unsigned long long c;
        c = (d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += (uint32_t)(c * 5); c = (h0 >> 26); h0 &= 0x3ffffff;
        h1 += (uint32_t)c;

        m += 16; bytes -= 16;
    }
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

static void poly1305_update(poly1305_state* st, const uint8_t* m, size_t bytes) {
    if (st->leftover) {
        size_t want = 16 - st->leftover;
        if (want > bytes) want = bytes;
        memcpy(st->buffer + st->leftover, m, want);
        bytes -= want; m += want; st->leftover += want;
        if (st->leftover < 16) return;
        poly1305_blocks(st, st->buffer, 16);
        st->leftover = 0;
    }
    if (bytes >= 16) {
        size_t want = bytes & ~((size_t)15);
        poly1305_blocks(st, m, want);
        m += want; bytes -= want;
    }
    if (bytes) { memcpy(st->buffer + st->leftover, m, bytes); st->leftover += bytes; }
}

static void poly1305_finish(poly1305_state* st, uint8_t mac[16]) {
    if (st->leftover) {
        size_t i = st->leftover;
        st->buffer[i++] = 1;
        for (; i < 16; ++i) st->buffer[i] = 0;
        st->final = 1;
        poly1305_blocks(st, st->buffer, 16);
    }
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    uint32_t c;
    c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1UL << 26);

    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    h0 = (h0 | (h1 << 26)) & 0xffffffff;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;

    unsigned long long f;
    f = (unsigned long long)h0 + st->pad[0]; h0 = (uint32_t)f;
    f = (unsigned long long)h1 + st->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (unsigned long long)h2 + st->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (unsigned long long)h3 + st->pad[3] + (f >> 32); h3 = (uint32_t)f;

    store32_le(mac + 0, h0);
    store32_le(mac + 4, h1);
    store32_le(mac + 8, h2);
    store32_le(mac +12, h3);
}

static const uint8_t kZeroPad[16] = {0};

static void chap_tag(const uint8_t mac_key[32], const uint8_t* aad, size_t aad_len,
                     const uint8_t* ct, size_t ct_len, uint8_t tag[16]) {
    poly1305_state st;
    uint8_t lens[16];
    poly1305_init(&st, mac_key);
    if (aad_len) {
        poly1305_update(&st, aad, aad_len);
        if (aad_len % 16) poly1305_update(&st, kZeroPad, 16 - (aad_len % 16));
    }
    if (ct_len) {
        poly1305_update(&st, ct, ct_len);
        if (ct_len % 16) poly1305_update(&st, kZeroPad, 16 - (ct_len % 16));
    }
    for (int i = 0; i < 8; ++i) lens[i]     = (uint8_t)((uint64_t)aad_len >> (8 * i));
    for (int i = 0; i < 8; ++i) lens[8 + i] = (uint8_t)((uint64_t)ct_len  >> (8 * i));
    poly1305_update(&st, lens, 16);
    poly1305_finish(&st, tag);
}

lr_chap_ctx* lr_chap_new(const uint8_t key[32]) {
    lr_chap_ctx* c = (lr_chap_ctx*)calloc(1, sizeof(lr_chap_ctx));
    if (!c) return NULL;
    memcpy(c->key, key, 32);
    c->path = chap_path();
    return c;
}
void lr_chap_free(lr_chap_ctx* c) { free(c); }

size_t lr_chap_seal(lr_chap_ctx* c, const uint8_t nonce[12],
                    const uint8_t* aad, size_t aad_len,
                    const uint8_t* pt, size_t pt_len, uint8_t* out) {
    uint8_t mac_key[64] = {0};
    chacha_xor(c->path, c->key, nonce, 0, mac_key, mac_key, 64);
    if (pt_len) chacha_xor(c->path, c->key, nonce, 1, pt, out, pt_len);
    chap_tag(mac_key, aad, aad_len, out, pt_len, out + pt_len);
    return pt_len + 16;
}

long lr_chap_open(lr_chap_ctx* c, const uint8_t nonce[12],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* ct, size_t ct_len, uint8_t* out) {
    if (ct_len < 16) return -1;
    const size_t body = ct_len - 16;
    uint8_t mac_key[64] = {0}, tag[16];
    chacha_xor(c->path, c->key, nonce, 0, mac_key, mac_key, 64);
    chap_tag(mac_key, aad, aad_len, ct, body, tag);
    if (!ct_eq16(tag, ct + body)) return -1;
    if (body) chacha_xor(c->path, c->key, nonce, 1, ct, out, body);
    return (long)body;
}
