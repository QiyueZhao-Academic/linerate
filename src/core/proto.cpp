// proto.cpp — handshake, anti-replay and rekey.
#include "lr/proto.hpp"
#include "lr/clock.hpp"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <cstring>

namespace lr::proto {

// ---------------------------------------------------------------------------
// HKDF-SHA256, the only key-derivation primitive used here.
// ---------------------------------------------------------------------------
namespace {

bool hkdf(const uint8_t* salt, size_t salt_len,
          const uint8_t* ikm, size_t ikm_len,
          const char* info, uint8_t* out, size_t out_len) {
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!c) return false;
    bool ok = EVP_PKEY_derive_init(c) == 1 &&
              EVP_PKEY_CTX_set_hkdf_md(c, EVP_sha256()) == 1 &&
              EVP_PKEY_CTX_set1_hkdf_salt(c, salt, (int)salt_len) == 1 &&
              EVP_PKEY_CTX_set1_hkdf_key(c, ikm, (int)ikm_len) == 1 &&
              EVP_PKEY_CTX_add1_hkdf_info(c, (const unsigned char*)info,
                                          (int)std::strlen(info)) == 1 &&
              EVP_PKEY_derive(c, out, &out_len) == 1;
    EVP_PKEY_CTX_free(c);
    return ok;
}

} // namespace

const uint8_t* bench_psk() {
    static uint8_t psk[kPskLen];
    static bool init = false;
    if (!init) {
        // A benchmark, not a deployment: the value is fixed so that two runs
        // are comparable and a result is reproducible.
        for (size_t i = 0; i < kPskLen; ++i) psk[i] = (uint8_t)(0x5Au ^ (i * 29u + 3u));
        init = true;
    }
    return psk;
}

// ---------------------------------------------------------------------------
// Anti-replay
// ---------------------------------------------------------------------------
ReplayWindow::ReplayWindow(size_t width_bits) {
    if (width_bits < 64) width_bits = 64;
    width_ = (width_bits + 63) / 64 * 64;
    bits_.assign(width_ / 64, 0);
}

void ReplayWindow::reset() {
    std::fill(bits_.begin(), bits_.end(), 0ull);
    high_ = 0; seen_ = false; dup_ = 0; old_ = 0;
}

bool ReplayWindow::accept(uint64_t seq) {
    if (!seen_) { seen_ = true; high_ = seq; bits_[0] = 1ull; return true; }
    if (seq > high_) {
        const uint64_t shift = seq - high_;
        if (shift >= width_) {
            std::fill(bits_.begin(), bits_.end(), 0ull);
        } else {
            // Shift the bitmap left by `shift` positions, word at a time. Bit i
            // of the map is "packet high_ - i has been seen".
            const size_t words = bits_.size();
            const size_t ws = (size_t)(shift / 64);
            const unsigned bs = (unsigned)(shift % 64);
            if (bs == 0) {
                for (size_t i = words; i-- > 0;)
                    bits_[i] = (i >= ws) ? bits_[i - ws] : 0ull;
            } else {
                for (size_t i = words; i-- > 0;) {
                    uint64_t v = (i >= ws) ? (bits_[i - ws] << bs) : 0ull;
                    if (i >= ws + 1) v |= bits_[i - ws - 1] >> (64 - bs);
                    bits_[i] = v;
                }
            }
        }
        high_ = seq;
        bits_[0] |= 1ull;
        return true;
    }
    const uint64_t back = high_ - seq;
    if (back >= width_) { old_++; return false; }
    const size_t   w = (size_t)(back / 64);
    const uint64_t m = 1ull << (back % 64);
    if (bits_[w] & m) { dup_++; return false; }
    bits_[w] |= m;
    return true;
}

// ---------------------------------------------------------------------------
// Key schedule
// ---------------------------------------------------------------------------
KeySchedule::KeySchedule(const Key& secret, const std::string& cipher,
                         uint64_t packets_per_epoch)
    : cipher_(cipher), per_epoch_(packets_per_epoch), chain_(secret) {
    derive(0);
    // Epoch 0 has no predecessor; point the spare slot at the same key so that
    // for_epoch() never has to special-case the first epoch.
    aead_[1] = nullptr;
    tag_[1]  = 0;
}

void KeySchedule::derive(uint8_t epoch_index) {
    uint8_t material[kKeyLen + kSaltLen];
    const uint8_t salt[1] = {epoch_index};
    if (!hkdf(salt, 1, chain_.data(), chain_.size(), "linerate traffic",
              material, sizeof(material))) {
        std::memset(material, 0, sizeof(material));
    }
    // Slot 0 is always the current epoch; the previous one moves to slot 1.
    aead_[1] = std::move(aead_[0]);
    salt_[1] = salt_[0];
    tag_[1]  = tag_[0];

    Key k{};
    std::memcpy(k.data(), material, kKeyLen);
    std::memcpy(salt_[0].data(), material + kKeyLen, kSaltLen);
    aead_[0] = make_aead(cipher_, k.data());
    tag_[0]  = epoch_index;

    // Ratchet the chain so the next epoch cannot be derived from this one's
    // traffic key alone.
    uint8_t next[kKeyLen];
    if (hkdf(salt, 1, chain_.data(), chain_.size(), "linerate ratchet", next, kKeyLen))
        std::memcpy(chain_.data(), next, kKeyLen);
}

Aead* KeySchedule::for_epoch(uint8_t epoch) {
    if (aead_[0] && epoch == tag_[0]) return aead_[0].get();
    if (aead_[1] && epoch == tag_[1]) return aead_[1].get();
    return nullptr;
}

void KeySchedule::nonce(uint8_t epoch, uint64_t seq, uint8_t out[12]) const {
    const Salt& s = (aead_[1] && epoch == tag_[1]) ? salt_[1] : salt_[0];
    std::memcpy(out, s.data(), kSaltLen);
    for (int i = 0; i < 8; ++i) out[kSaltLen + i] = (uint8_t)(seq >> (56 - 8 * i));
}

bool KeySchedule::maybe_rekey(uint64_t seq) {
    if (per_epoch_ == 0) return false;
    const uint8_t want = (uint8_t)((seq / per_epoch_) & 0xFF);
    if (want == epoch_) return false;
    epoch_ = want;
    derive(epoch_);
    rekeys_++;
    return true;
}

const char* KeySchedule::cipher_backend() const {
    return aead_[0] ? aead_[0]->backend() : "none";
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------
struct Handshake::Impl {
    uint8_t  psk[kPskLen];
    EVP_PKEY* eph = nullptr;
    uint8_t  my_pub[kPubLen]{};
    uint8_t  peer_pub[kPubLen]{};
    bool     initiator = false;
    ~Impl() { if (eph) EVP_PKEY_free(eph); }
};

namespace {

bool gen_ephemeral(EVP_PKEY** out, uint8_t pub[kPubLen]) {
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!c) return false;
    EVP_PKEY* k = nullptr;
    bool ok = EVP_PKEY_keygen_init(c) == 1 && EVP_PKEY_keygen(c, &k) == 1;
    EVP_PKEY_CTX_free(c);
    if (!ok) return false;
    size_t len = kPubLen;
    ok = EVP_PKEY_get_raw_public_key(k, pub, &len) == 1 && len == kPubLen;
    if (!ok) { EVP_PKEY_free(k); return false; }
    *out = k;
    return true;
}

bool x25519(EVP_PKEY* mine, const uint8_t peer[kPubLen], uint8_t shared[32]) {
    EVP_PKEY* pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer, kPubLen);
    if (!pk) return false;
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(mine, nullptr);
    size_t len = 32;
    bool ok = c && EVP_PKEY_derive_init(c) == 1 &&
              EVP_PKEY_derive_set_peer(c, pk) == 1 &&
              EVP_PKEY_derive(c, shared, &len) == 1 && len == 32;
    if (c) EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(pk);
    return ok;
}

// A 16-byte binder over the transcript so far, keyed by the PSK. This is what
// authenticates the exchange: an attacker without the PSK cannot produce it,
// so an unauthenticated ephemeral key cannot be substituted.
bool binder(const uint8_t psk[kPskLen], const uint8_t* transcript, size_t len,
            uint8_t out[16]) {
    uint8_t full[32];
    if (!hkdf(psk, kPskLen, transcript, len, "linerate binder", full, sizeof(full)))
        return false;
    std::memcpy(out, full, 16);
    return true;
}

bool ct_eq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d = (uint8_t)(d | (a[i] ^ b[i]));
    return d == 0;
}

bool traffic_keys(const uint8_t shared[32], const uint8_t psk[kPskLen],
                  const uint8_t* transcript, size_t tlen,
                  Key* i2r, Key* r2i) {
    uint8_t ikm[32 + kPskLen];
    std::memcpy(ikm, shared, 32);
    std::memcpy(ikm + 32, psk, kPskLen);
    uint8_t out[64];
    if (!hkdf(transcript, tlen, ikm, sizeof(ikm), "linerate directions", out, sizeof(out)))
        return false;
    std::memcpy(i2r->data(), out, 32);
    std::memcpy(r2i->data(), out + 32, 32);
    return true;
}

} // namespace

Handshake::Handshake(const uint8_t psk[kPskLen]) : p_(new Impl) {
    std::memcpy(p_->psk, psk, kPskLen);
}
Handshake::~Handshake() = default;

bool Handshake::write_init(uint8_t* out) {
    p_->initiator = true;
    if (!gen_ephemeral(&p_->eph, p_->my_pub)) return false;
    std::memcpy(out, p_->my_pub, kPubLen);
    return binder(p_->psk, out, kPubLen, out + kPubLen);
}

bool Handshake::read_init_write_resp(const uint8_t* in, uint8_t* out, HandshakeResult* r) {
    const uint64_t t0 = clk::ticks();
    p_->initiator = false;
    uint8_t expect[16];
    if (!binder(p_->psk, in, kPubLen, expect)) { r->error = "binder"; return false; }
    if (!ct_eq(expect, in + kPubLen, 16)) { r->error = "psk binder mismatch"; return false; }
    std::memcpy(p_->peer_pub, in, kPubLen);

    if (!gen_ephemeral(&p_->eph, p_->my_pub)) { r->error = "keygen"; return false; }
    std::memcpy(out, p_->my_pub, kPubLen);

    uint8_t transcript[2 * kPubLen];
    std::memcpy(transcript, p_->peer_pub, kPubLen);
    std::memcpy(transcript + kPubLen, p_->my_pub, kPubLen);
    if (!binder(p_->psk, transcript, sizeof(transcript), out + kPubLen)) {
        r->error = "binder"; return false;
    }

    uint8_t shared[32];
    if (!x25519(p_->eph, p_->peer_pub, shared)) { r->error = "x25519"; return false; }
    Key i2r{}, r2i{};
    if (!traffic_keys(shared, p_->psk, transcript, sizeof(transcript), &i2r, &r2i)) {
        r->error = "kdf"; return false;
    }
    r->tx = r2i;   // the responder sends on responder-to-initiator
    r->rx = i2r;
    r->ok = true;
    r->micros = clk::ticks_to_ns(clk::ticks() - t0) / 1000.0;
    return true;
}

bool Handshake::read_resp(const uint8_t* in, HandshakeResult* r) {
    const uint64_t t0 = clk::ticks();
    std::memcpy(p_->peer_pub, in, kPubLen);
    uint8_t transcript[2 * kPubLen];
    std::memcpy(transcript, p_->my_pub, kPubLen);
    std::memcpy(transcript + kPubLen, p_->peer_pub, kPubLen);

    uint8_t expect[16];
    if (!binder(p_->psk, transcript, sizeof(transcript), expect)) { r->error = "binder"; return false; }
    if (!ct_eq(expect, in + kPubLen, 16)) { r->error = "response binder mismatch"; return false; }

    uint8_t shared[32];
    if (!x25519(p_->eph, p_->peer_pub, shared)) { r->error = "x25519"; return false; }
    Key i2r{}, r2i{};
    if (!traffic_keys(shared, p_->psk, transcript, sizeof(transcript), &i2r, &r2i)) {
        r->error = "kdf"; return false;
    }
    r->tx = i2r;
    r->rx = r2i;
    r->ok = true;
    r->micros = clk::ticks_to_ns(clk::ticks() - t0) / 1000.0;
    return true;
}

HandshakeResult handshake_pair(const uint8_t psk[kPskLen], Key* itx, Key* irx,
                               Key* rtx, Key* rrx) {
    HandshakeResult total;
    const uint64_t t0 = clk::ticks();
    Handshake initiator(psk), responder(psk);
    uint8_t m1[kHsInitLen], m2[kHsRespLen];
    HandshakeResult ri, rr;
    if (!initiator.write_init(m1))                    { total.error = "init";  return total; }
    if (!responder.read_init_write_resp(m1, m2, &rr)) { total.error = rr.error; return total; }
    if (!initiator.read_resp(m2, &ri))                { total.error = ri.error; return total; }
    if (itx) *itx = ri.tx;
    if (irx) *irx = ri.rx;
    if (rtx) *rtx = rr.tx;
    if (rrx) *rrx = rr.rx;
    total.ok = true;
    total.tx = ri.tx;
    total.rx = ri.rx;
    total.micros = clk::ticks_to_ns(clk::ticks() - t0) / 1000.0;
    return total;
}

} // namespace lr::proto
