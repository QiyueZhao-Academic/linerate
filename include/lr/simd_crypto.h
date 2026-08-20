/* simd_crypto.h — the AEAD implementations written in this repository.
 *
 * Plain C with a C linkage, so that the SIMD work is compiled as C with its own
 * target attributes and the C++ side never sees an intrinsic. Two ciphers, each
 * with three code paths chosen at run time:
 *
 *   AES-256-GCM            x86-64  AES-NI for the block cipher, PCLMULQDQ for
 *                                  GHASH, four blocks in flight
 *                          aarch64 ARMv8 AES instructions and PMULL
 *                          other   portable C, table-driven AES and a 4-bit
 *                                  GHASH table
 *
 *   ChaCha20-Poly1305      x86-64  AVX2, two blocks per iteration
 *                          aarch64 NEON, one block per iteration
 *                          other   portable C
 *                          Poly1305 is scalar everywhere, five 26-bit limbs
 *                          over 64-bit accumulators
 *
 * Correctness is not asserted, it is checked: lr_selftest runs both against
 * published vectors and then against OpenSSL at forty payload lengths from 0 to
 * 1432 bytes, comparing ciphertext and tag byte for byte. A cipher that is fast
 * and wrong is worth nothing, and the sweep does not start until this passes.
 */
#ifndef LR_SIMD_CRYPTO_H
#define LR_SIMD_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lr_gcm_ctx  lr_gcm_ctx;
typedef struct lr_chap_ctx lr_chap_ctx;

/* Which path the library selected. Valid after any _new(). */
const char* lr_simd_gcm_backend(void);
const char* lr_simd_chap_backend(void);

/* Force the portable paths, whatever the CPU offers. The harness calls this
 * when it is running the masked half of the hardware-crypto factor, so that our
 * implementation and OpenSSL's are masked by the same lever rather than one of
 * them quietly keeping its accelerator. */
void lr_simd_force_portable(int on);

/* Mask the AES accelerator only: GCM falls back to the portable path, ChaCha20
   keeps its vector path. This mirrors exactly what OPENSSL_ia32cap does to
   libcrypto, so that the masked arm of the sweep applies one identical lever to
   both implementations. Use lr_simd_force_portable() instead when the intent is
   to exercise the portable code itself, as the self-test does. */
void lr_simd_mask_aes_accel(int on);

/* AES-256-GCM. `key` is 32 bytes. */
lr_gcm_ctx* lr_gcm_new(const uint8_t key[32]);
void        lr_gcm_free(lr_gcm_ctx*);
/* Writes ct_len = pt_len bytes of ciphertext followed by a 16-byte tag.
 * Returns pt_len + 16. `nonce` is 12 bytes. */
size_t      lr_gcm_seal(lr_gcm_ctx*, const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* pt, size_t pt_len, uint8_t* out);
/* `ct_len` includes the trailing tag. Returns the plaintext length, or -1 when
 * the tag does not verify. */
long        lr_gcm_open(lr_gcm_ctx*, const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* ct, size_t ct_len, uint8_t* out);

/* ChaCha20-Poly1305, RFC 8439. `key` is 32 bytes, `nonce` 12 bytes. */
lr_chap_ctx* lr_chap_new(const uint8_t key[32]);
void         lr_chap_free(lr_chap_ctx*);
size_t       lr_chap_seal(lr_chap_ctx*, const uint8_t nonce[12],
                          const uint8_t* aad, size_t aad_len,
                          const uint8_t* pt, size_t pt_len, uint8_t* out);
long         lr_chap_open(lr_chap_ctx*, const uint8_t nonce[12],
                          const uint8_t* aad, size_t aad_len,
                          const uint8_t* ct, size_t ct_len, uint8_t* out);

/* Raw primitives, exposed for the self-test's known-answer vectors and for the
 * GPU experiment, which needs a CPU reference for the same counter mode. */
void lr_chacha20_keystream(const uint8_t key[32], const uint8_t nonce[12],
                           uint32_t counter, uint8_t* out, size_t len);
void lr_aes256_ctr(const uint8_t key[32], const uint8_t iv[16],
                   const uint8_t* in, uint8_t* out, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* LR_SIMD_CRYPTO_H */
