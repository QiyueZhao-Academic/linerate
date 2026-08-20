// aead_factory.cpp — the one place that maps a cipher name to an implementation,
// and the null control.
//
// The null cipher satisfies the interface exactly but copies instead of
// encrypting. It keeps the tag length so that packets are the same size on the
// wire and the transport does exactly the same work; the only thing removed is
// the cipher. Running the whole hot path with it measures the fixed per-packet
// cost a directly, rather than inferring it from the intercept of a fit.
//
// A memcpy is not free, so this control measures a plus one memory pass, not a
// alone. The overstatement is bounded by the copy cost, which E4 measures
// separately; the report states the correction rather than ignoring it.
#include "lr/aead.hpp"

#include <cstring>

namespace lr {

std::unique_ptr<Aead> make_aead_openssl(const std::string& name, const uint8_t* key);
std::unique_ptr<Aead> make_aead_simd(const std::string& name, const uint8_t* key);

namespace {

class NullAead final : public Aead {
public:
    size_t seal(const uint8_t* nonce, const uint8_t* aad, size_t aad_len,
                const uint8_t* pt, size_t pt_len, uint8_t* out) override {
        (void)aad; (void)aad_len;
        std::memmove(out, pt, pt_len);
        // The tag region is written so the output packet is byte-identical in
        // length and the send path is unchanged. The contents are the nonce,
        // repeated, so the write is not optimised away.
        for (size_t i = 0; i < kTag; ++i) out[pt_len + i] = nonce[i % 12];
        return pt_len + kTag;
    }

    long open(const uint8_t* nonce, const uint8_t* aad, size_t aad_len,
              const uint8_t* ct, size_t ct_len, uint8_t* out) override {
        (void)nonce; (void)aad; (void)aad_len;
        if (ct_len < kTag) return -1;
        const size_t body = ct_len - kTag;
        std::memmove(out, ct, body);
        return (long)body;
    }

    const char* name()      const override { return "null"; }
    size_t      tag_len()   const override { return kTag; }
    size_t      key_len()   const override { return 32; }
    size_t      nonce_len() const override { return 12; }
    const char* backend()   const override { return "memcpy"; }

private:
    static constexpr size_t kTag = 16;
};

const std::vector<std::string> kNames = {
    "aes-256-gcm", "chacha20-poly1305",
    "lr-aes-256-gcm", "lr-chacha20-poly1305",
};

} // namespace

std::unique_ptr<Aead> make_aead(const std::string& name, const uint8_t* key) {
    if (name == "null") return std::make_unique<NullAead>();
    if (name.rfind("lr-", 0) == 0) return make_aead_simd(name, key);
    return make_aead_openssl(name, key);
}

size_t aead_key_len(const std::string& name) {
    (void)name;
    return 32;   // every backend here takes a 256-bit key
}

const std::vector<std::string>& aead_names() { return kNames; }

bool aead_is_local(const std::string& name) { return name.rfind("lr-", 0) == 0; }

} // namespace lr
