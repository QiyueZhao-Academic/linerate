// aead.hpp — the cipher interface.
//
// One virtual call per packet, resolved once at startup. The interface is
// deliberately narrow so that the null control (a memcpy of the same length)
// satisfies it exactly, and any experiment can swap ciphers without the harness
// knowing which one it is holding.
//
// Four families are registered behind it:
//
//   openssl:aes-256-gcm          EVP, the reference implementation
//   openssl:chacha20-poly1305    EVP, the reference implementation
//   lr:aes-256-gcm               written here: AES-NI + PCLMULQDQ on x86-64,
//                                ARMv8 AES + PMULL on aarch64, portable C
//                                elsewhere
//   lr:chacha20-poly1305         written here: ChaCha20 vectorised with AVX2 or
//                                NEON, Poly1305 with 130-bit limb arithmetic
//
// Having two independent implementations of the same two ciphers is what turns
// "our cipher is fast" into a measurement: the self-test cross-checks every
// output against OpenSSL byte for byte at 40 lengths, and E1 sweeps all four as
// four levels of one factor.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lr {

class Aead {
public:
    virtual ~Aead() = default;

    // Encrypt-and-authenticate. Writes ciphertext followed by the tag into `out`,
    // which must have room for pt_len + tag_len() bytes. Returns bytes written,
    // or 0 on failure.
    virtual size_t seal(const uint8_t* nonce, const uint8_t* aad, size_t aad_len,
                        const uint8_t* pt, size_t pt_len, uint8_t* out) = 0;

    // Verify-and-decrypt. `ct_len` includes the trailing tag. Returns plaintext
    // length, or -1 when the tag does not verify. A rejected packet must cost
    // roughly what an accepted one costs, so tamper rejection cannot be used as
    // a side channel or as a cheap path through the hot loop.
    virtual long open(const uint8_t* nonce, const uint8_t* aad, size_t aad_len,
                      const uint8_t* ct, size_t ct_len, uint8_t* out) = 0;

    virtual const char* name()      const = 0;
    virtual size_t      tag_len()   const = 0;
    virtual size_t      key_len()   const = 0;
    virtual size_t      nonce_len() const = 0;

    // Which code path the object actually took, e.g. "aesni+pclmulqdq",
    // "armv8-aes+pmull", "avx2", "portable", "evp". Recorded with every
    // measurement so that a number stays interpretable.
    virtual const char* backend() const = 0;
};

// Factory. Returns nullptr when the name is unknown. `key` must be key_len()
// bytes for that cipher.
//
//   "aes-256-gcm"          | "chacha20-poly1305"          -> OpenSSL EVP
//   "lr-aes-256-gcm"       | "lr-chacha20-poly1305"       -> written here
//   "null"                                                -> the control
std::unique_ptr<Aead> make_aead(const std::string& name, const uint8_t* key);

// Key length for a cipher name, without constructing it.
size_t aead_key_len(const std::string& name);

// Every registered cipher name, in sweep order.
const std::vector<std::string>& aead_names();

// True when `name` names an implementation written in this repository rather
// than one supplied by OpenSSL.
bool aead_is_local(const std::string& name);

} // namespace lr
