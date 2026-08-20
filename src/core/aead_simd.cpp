// aead_simd.cpp — the Aead adapter over the implementations written here.
//
// A thin wrapper on purpose. The C side owns every intrinsic and every
// dispatch decision; this file exists only so that the harness can hold our
// AES-256-GCM and OpenSSL's behind the same pointer and swap them as one factor
// level of one experiment.
#include "lr/aead.hpp"
#include "lr/platform.hpp"
#include "lr/simd_crypto.h"

#include <cstring>

namespace lr {
namespace {

class SimdGcm final : public Aead {
public:
    explicit SimdGcm(const uint8_t* key) { c_ = lr_gcm_new(key); }
    ~SimdGcm() override { if (c_) lr_gcm_free(c_); }
    size_t seal(const uint8_t* n, const uint8_t* aad, size_t al,
                const uint8_t* pt, size_t pl, uint8_t* out) override {
        return c_ ? lr_gcm_seal(c_, n, aad, al, pt, pl, out) : 0;
    }
    long open(const uint8_t* n, const uint8_t* aad, size_t al,
              const uint8_t* ct, size_t cl, uint8_t* out) override {
        return c_ ? lr_gcm_open(c_, n, aad, al, ct, cl, out) : -1;
    }
    const char* name()      const override { return "lr-aes-256-gcm"; }
    size_t      tag_len()   const override { return 16; }
    size_t      key_len()   const override { return 32; }
    size_t      nonce_len() const override { return 12; }
    const char* backend()   const override { return lr_simd_gcm_backend(); }
private:
    lr_gcm_ctx* c_ = nullptr;
};

class SimdChaPoly final : public Aead {
public:
    explicit SimdChaPoly(const uint8_t* key) { c_ = lr_chap_new(key); }
    ~SimdChaPoly() override { if (c_) lr_chap_free(c_); }
    size_t seal(const uint8_t* n, const uint8_t* aad, size_t al,
                const uint8_t* pt, size_t pl, uint8_t* out) override {
        return c_ ? lr_chap_seal(c_, n, aad, al, pt, pl, out) : 0;
    }
    long open(const uint8_t* n, const uint8_t* aad, size_t al,
              const uint8_t* ct, size_t cl, uint8_t* out) override {
        return c_ ? lr_chap_open(c_, n, aad, al, ct, cl, out) : -1;
    }
    const char* name()      const override { return "lr-chacha20-poly1305"; }
    size_t      tag_len()   const override { return 16; }
    size_t      key_len()   const override { return 32; }
    size_t      nonce_len() const override { return 12; }
    const char* backend()   const override { return lr_simd_chap_backend(); }
private:
    lr_chap_ctx* c_ = nullptr;
};

} // namespace

std::unique_ptr<Aead> make_aead_simd(const std::string& name, const uint8_t* key) {
    // The masked half of the hardware-crypto factor has to reach our
    // implementation too. OpenSSL reads its capability word from the
    // environment at load time; we read the same signal, so both halves of the
    // comparison are masked by one lever rather than one of them quietly
    // keeping its accelerator and making the comparison meaningless.
    // Exactly what OPENSSL_ia32cap does to libcrypto: the AES accelerator goes,
    // the vector unit stays. Forcing everything portable here would make the
    // masked ChaCha20 comparison a comparison of vectorisation rather than of
    // the accelerator, which is not the factor being varied.
    lr_simd_mask_aes_accel(plat::hw_crypto_masked() ? 1 : 0);
    if (name == "lr-aes-256-gcm")        return std::make_unique<SimdGcm>(key);
    if (name == "lr-chacha20-poly1305")  return std::make_unique<SimdChaPoly>(key);
    return nullptr;
}

} // namespace lr
