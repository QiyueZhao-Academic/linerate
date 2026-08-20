// aead_openssl.cpp — the two real ciphers, through OpenSSL 3 EVP.
//
// Both contexts are created once and re-keyed once. Per packet the only
// initialisation is the nonce, which is the fast path a real data plane uses;
// re-deriving the key schedule per packet would move key setup into the
// per-packet term a and would measure the wrong thing.
#include "lr/aead.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>

#include <cstring>

namespace lr {
namespace {

class OpenSslAead final : public Aead {
public:
    OpenSslAead(const EVP_CIPHER* c, const char* nm, const uint8_t* key,
                size_t klen, size_t nlen, size_t tlen)
        : name_(nm), key_len_(klen), nonce_len_(nlen), tag_len_(tlen) {
        enc_ = EVP_CIPHER_CTX_new();
        dec_ = EVP_CIPHER_CTX_new();
        // Key is bound now; only the IV changes per packet.
        EVP_EncryptInit_ex(enc_, c, nullptr, key, nullptr);
        EVP_DecryptInit_ex(dec_, c, nullptr, key, nullptr);
        EVP_CIPHER_CTX_set_padding(enc_, 0);
        EVP_CIPHER_CTX_set_padding(dec_, 0);
    }
    ~OpenSslAead() override {
        EVP_CIPHER_CTX_free(enc_);
        EVP_CIPHER_CTX_free(dec_);
    }

    size_t seal(const uint8_t* nonce, const uint8_t* aad, size_t aad_len,
                const uint8_t* pt, size_t pt_len, uint8_t* out) override {
        int len = 0, total = 0;
        if (EVP_EncryptInit_ex(enc_, nullptr, nullptr, nullptr, nonce) != 1) return 0;
        if (aad_len && EVP_EncryptUpdate(enc_, nullptr, &len, aad, (int)aad_len) != 1) return 0;
        if (EVP_EncryptUpdate(enc_, out, &len, pt, (int)pt_len) != 1) return 0;
        total = len;
        if (EVP_EncryptFinal_ex(enc_, out + total, &len) != 1) return 0;
        total += len;
        if (EVP_CIPHER_CTX_ctrl(enc_, EVP_CTRL_AEAD_GET_TAG, (int)tag_len_, out + total) != 1) return 0;
        return (size_t)total + tag_len_;
    }

    long open(const uint8_t* nonce, const uint8_t* aad, size_t aad_len,
              const uint8_t* ct, size_t ct_len, uint8_t* out) override {
        if (ct_len < tag_len_) return -1;
        const size_t body = ct_len - tag_len_;
        int len = 0, total = 0;
        if (EVP_DecryptInit_ex(dec_, nullptr, nullptr, nullptr, nonce) != 1) return -1;
        if (aad_len && EVP_DecryptUpdate(dec_, nullptr, &len, aad, (int)aad_len) != 1) return -1;
        if (EVP_DecryptUpdate(dec_, out, &len, ct, (int)body) != 1) return -1;
        total = len;
        // The tag is supplied before Final, which is where verification happens.
        // A forged packet therefore costs a full pass over the ciphertext, the
        // same as a valid one.
        if (EVP_CIPHER_CTX_ctrl(dec_, EVP_CTRL_AEAD_SET_TAG, (int)tag_len_,
                                (void*)(ct + body)) != 1) return -1;
        if (EVP_DecryptFinal_ex(dec_, out + total, &len) != 1) return -1;
        return (long)(total + len);
    }

    const char* name()      const override { return name_; }
    // EVP resolves its own path from the capability word; naming it here keeps
    // the dataset's backend column meaningful for every cipher rather than only
    // for the two written in this repository.
    const char* backend()   const override { return "openssl-evp"; }
    size_t      tag_len()   const override { return tag_len_; }
    size_t      key_len()   const override { return key_len_; }
    size_t      nonce_len() const override { return nonce_len_; }

private:
    EVP_CIPHER_CTX* enc_ = nullptr;
    EVP_CIPHER_CTX* dec_ = nullptr;
    const char* name_;
    size_t key_len_, nonce_len_, tag_len_;
};

} // namespace

std::unique_ptr<Aead> make_aead_openssl(const std::string& name, const uint8_t* key) {
    if (name == "aes-256-gcm")
        return std::make_unique<OpenSslAead>(EVP_aes_256_gcm(), "aes-256-gcm", key, 32, 12, 16);
    if (name == "chacha20-poly1305")
        return std::make_unique<OpenSslAead>(EVP_chacha20_poly1305(), "chacha20-poly1305", key, 32, 12, 16);
    return nullptr;
}

} // namespace lr
