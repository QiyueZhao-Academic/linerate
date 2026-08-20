// lr_selftest.cpp — correctness, before any timing.
//
// Runs on every platform, including the development machine. Nothing in this
// file measures anything; it establishes that each cipher is the cipher it
// claims to be, that the two implementations written here agree with OpenSSL
// byte for byte, that a forged packet is rejected, that the replay window and
// the rekey schedule behave, that the handshake agrees on a key, that every
// transport backend round-trips, and it prints the host classification so an
// operator can see at a glance whether this machine may produce numbers.
//
// A performance study of a cipher that computes the wrong ciphertext measures
// nothing, so this gate runs first and run.sh stops if it fails.
#include "lr/aead.hpp"
#include "lr/clock.hpp"
#include "lr/platform.hpp"
#include "lr/proto.hpp"
#include "lr/simd_crypto.h"
#include "lr/transport.hpp"
#include "lr/worker.hpp"
#include "build_info.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace lr;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? "pass" : "FAIL", what.c_str());
    if (!ok) failures++;
}

std::vector<uint8_t> unhex(const char* h) {
    std::vector<uint8_t> v;
    for (const char* p = h; *p; ) {
        while (*p == ' ' || *p == '\n') ++p;
        if (!*p) break;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nib(p[0]), lo = nib(p[1]);
        if (hi < 0 || lo < 0) break;
        v.push_back((uint8_t)((hi << 4) | lo));
        p += 2;
    }
    return v;
}

// A known-answer test: encrypt the vector's plaintext under the vector's key and
// nonce and require the ciphertext and tag to match byte for byte.
void kat(const char* label, const char* cipher, const char* key_h, const char* nonce_h,
         const char* aad_h, const char* pt_h, const char* ct_h, const char* tag_h) {
    auto key = unhex(key_h), nonce = unhex(nonce_h), aad = unhex(aad_h);
    auto pt = unhex(pt_h), ct = unhex(ct_h), tag = unhex(tag_h);
    auto a = make_aead(cipher, key.data());
    if (!a) { check(false, label); return; }

    std::vector<uint8_t> out(pt.size() + a->tag_len() + 16, 0);
    size_t n = a->seal(nonce.data(), aad.data(), aad.size(), pt.data(), pt.size(), out.data());

    bool ok = (n == ct.size() + tag.size()) &&
              std::memcmp(out.data(), ct.data(), ct.size()) == 0 &&
              std::memcmp(out.data() + ct.size(), tag.data(), tag.size()) == 0;
    check(ok, label);

    std::vector<uint8_t> back(pt.size() + 16, 0);
    long r = a->open(nonce.data(), aad.data(), aad.size(), out.data(), n, back.data());
    check(r == (long)pt.size() && std::memcmp(back.data(), pt.data(), pt.size()) == 0,
          std::string(label) + ": round trip");

    if (!pt.empty()) {
        out[0] ^= 0x01;
        check(a->open(nonce.data(), aad.data(), aad.size(), out.data(), n, back.data()) < 0,
              std::string(label) + ": tampered ciphertext rejected");
        out[0] ^= 0x01;
    }
    if (!aad.empty()) {
        aad[0] ^= 0x01;
        check(a->open(nonce.data(), aad.data(), aad.size(), out.data(), n, back.data()) < 0,
              std::string(label) + ": tampered associated data rejected");
        aad[0] ^= 0x01;
    }
    out[n - 1] ^= 0x80;
    check(a->open(nonce.data(), aad.data(), aad.size(), out.data(), n, back.data()) < 0,
          std::string(label) + ": tampered tag rejected");
}

// Every implementation written here is required to agree with OpenSSL at every
// length the sweep will use, not only at the one length a published vector
// happens to cover. Boundary lengths around the SIMD unroll factors are what
// catch a tail-handling bug, so they are enumerated explicitly.
void cross_check(const char* ours, const char* reference) {
    uint8_t key[32], nonce[12];
    for (int i = 0; i < 32; ++i) key[i] = (uint8_t)(0xA5u ^ (i * 17u));
    for (int i = 0; i < 12; ++i) nonce[i] = (uint8_t)(i * 13u + 5u);
    std::vector<uint8_t> aad(LR_HDR, 0x5A), pt(LR_MAX_PAYLOAD);
    for (size_t i = 0; i < pt.size(); ++i) pt[i] = (uint8_t)(i * 31u + 7u);

    auto A = make_aead(ours, key);
    auto B = make_aead(reference, key);
    if (!A || !B) { check(false, std::string(ours) + " vs " + reference); return; }

    const size_t lens[] = {0, 1, 15, 16, 17, 31, 32, 33, 47, 63, 64, 65, 95, 127, 128, 129,
                           191, 200, 255, 256, 257, 383, 511, 512, 700, 1023, 1024, 1200,
                           1400, LR_MAX_PAYLOAD};
    size_t bad = 0, worst = 0;
    for (size_t L : lens) {
        if (L > pt.size()) continue;
        std::vector<uint8_t> oa(L + 32), ob(L + 32), back(L + 32);
        for (int with_aad = 0; with_aad < 2; ++with_aad) {
            const size_t al = with_aad ? aad.size() : 0;
            size_t na = A->seal(nonce, aad.data(), al, pt.data(), L, oa.data());
            size_t nb = B->seal(nonce, aad.data(), al, pt.data(), L, ob.data());
            if (na != nb || std::memcmp(oa.data(), ob.data(), na) != 0) { bad++; worst = L; }
            long r = A->open(nonce, aad.data(), al, ob.data(), nb, back.data());
            if (r != (long)L || std::memcmp(back.data(), pt.data(), L) != 0) { bad++; worst = L; }
        }
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%s (%s) matches %s at %zu lengths, both directions",
                  ours, A->backend(), reference, sizeof(lens) / sizeof(lens[0]));
    if (bad) std::snprintf(buf, sizeof(buf), "%s vs %s: %zu mismatches (e.g. length %zu)",
                           ours, reference, bad, worst);
    check(bad == 0, buf);
}

void protocol_checks() {
    // Replay window: fresh sequence numbers accepted, duplicates and packets
    // older than the window rejected, and a large forward jump resynchronises.
    // One sequence number is deliberately skipped on the way up, so that the
    // reordering case tests a genuine gap rather than a number the window has
    // already recorded.
    const uint64_t kGap = 4950;
    proto::ReplayWindow w(1024);
    bool ok = true;
    for (uint64_t i = 1; i <= 5000; ++i) {
        if (i == kGap) continue;
        ok = ok && w.accept(i);
    }
    check(ok, "replay window: 4999 in-order packets accepted");
    check(!w.accept(5000), "replay window: duplicate rejected");
    // 3000 is 2000 behind the highest seen, and the window is 1024 wide, so it
    // is unverifiable rather than merely a repeat.
    check(!w.accept(3000), "replay window: packet older than the window rejected");
    check(w.accept(kGap), "replay window: in-window reordering accepted");
    check(!w.accept(kGap), "replay window: in-window duplicate rejected");
    check(w.accept(1000000), "replay window: forward jump resynchronises");
    check(w.duplicates() == 2 && w.too_old() == 1,
          "replay window: counters agree with the decisions taken");

    // Rekey: a packet from the previous epoch still verifies; one from two
    // epochs back does not.
    proto::Key secret{};
    for (size_t i = 0; i < secret.size(); ++i) secret[i] = (uint8_t)(i * 11u + 3u);
    proto::KeySchedule ks(secret, "aes-256-gcm", 64);
    std::vector<uint8_t> pt(128, 0x33), ct(160), back(160);
    uint8_t n0[12];
    ks.nonce(ks.epoch(), 1, n0);
    size_t n = ks.for_epoch(ks.epoch())->seal(n0, nullptr, 0, pt.data(), pt.size(), ct.data());
    const uint8_t e0 = ks.epoch();
    check(ks.maybe_rekey(64), "rekey: epoch advances at the configured period");
    check(ks.epoch() != e0, "rekey: epoch number changed");
    uint8_t n0b[12];
    ks.nonce(e0, 1, n0b);
    Aead* prev = ks.for_epoch(e0);
    check(prev != nullptr, "rekey: previous epoch retained for reordered packets");
    check(prev && prev->open(n0b, nullptr, 0, ct.data(), n, back.data()) == (long)pt.size(),
          "rekey: a packet from the previous epoch still verifies");
    ks.maybe_rekey(128);
    check(ks.for_epoch(e0) == nullptr, "rekey: two epochs back is retired");

    // Handshake: both ends derive the same directional keys, and a wrong PSK
    // fails rather than silently producing different keys.
    proto::Key itx{}, irx{}, rtx{}, rrx{};
    auto hr = proto::handshake_pair(proto::bench_psk(), &itx, &irx, &rtx, &rrx);
    check(hr.ok, "handshake: completes");
    check(itx == rrx && irx == rtx, "handshake: both ends agree on both directions");
    check(itx != irx, "handshake: the two directions use different keys");

    uint8_t bad_psk[proto::kPskLen];
    std::memcpy(bad_psk, proto::bench_psk(), sizeof(bad_psk));
    bad_psk[0] ^= 0xFF;
    proto::Handshake init(proto::bench_psk()), resp(bad_psk);
    uint8_t m1[proto::kHsInitLen], m2[proto::kHsRespLen];
    proto::HandshakeResult r2;
    init.write_init(m1);
    check(!resp.read_init_write_resp(m1, m2, &r2),
          "handshake: a wrong pre-shared key is refused");
}

void transport_round_trip() {
    for (const std::string& backend : transport_backends()) {
        for (const char* cipher : {"aes-256-gcm", "chacha20-poly1305",
                                   "lr-aes-256-gcm", "lr-chacha20-poly1305", "null"}) {
            ShardConfig cfg;
            cfg.cipher  = cipher;
            cfg.backend = backend;
            cfg.batch   = (backend == "posix") ? 1 : 16;
            cfg.payload = 512;
            cfg.source  = Source::Loopback;
            std::string err;
            Shard sh(cfg, proto::bench_psk(), &err);
            if (!err.empty()) { check(false, backend + "/" + cipher + ": " + err); continue; }

            const int want = 256;
            sh.preload(want);
            int got = sh.drain(want);
            const ShardStats& st = sh.stats();
            bool ok = got > 0 && st.packets > 0 && st.auth_fail == 0 &&
                      st.replay_drop == 0 && st.bytes == st.packets * cfg.payload;
            char label[192];
            std::snprintf(label, sizeof(label),
                          "transport %s / %s: %llu packets, %llu auth failures, %llu replay drops",
                          backend.c_str(), cipher, (unsigned long long)st.packets,
                          (unsigned long long)st.auth_fail,
                          (unsigned long long)st.replay_drop);
            check(ok, label);
        }
    }
    for (const std::string& missing : {std::string("uring"), std::string("uring-sqpoll")}) {
        const auto& b = transport_backends();
        if (std::find(b.begin(), b.end(), missing) == b.end()) {
            std::string why;
            (missing == "uring") ? uring_available(&why) : uring_sqpoll_available(&why);
            printf("  [skip] transport '%s': %s\n", missing.c_str(), why.c_str());
        }
    }
}

void clock_checks() {
    check(clk::tick_hz() > 1000000, "tick counter frequency is plausible");
    check(clk::invariant(), "timestamp counter is invariant");

    // Monotonicity across a short interval, and agreement with the wall clock to
    // within a few per cent. A counter that drifts against CLOCK_MONOTONIC_RAW
    // is not usable for cycle-level work no matter what its flags say.
    const uint64_t t0 = clk::ticks();
    timespec a{}, b{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &a);
    double target = a.tv_sec + a.tv_nsec * 1e-9 + 0.05;
    double now;
    do {
        clock_gettime(CLOCK_MONOTONIC_RAW, &b);
        now = b.tv_sec + b.tv_nsec * 1e-9;
    } while (now < target);
    const uint64_t t1 = clk::ticks();
    check(t1 > t0, "counter is monotonic");
    const double measured = clk::ticks_to_ns(t1 - t0) / 1e9;
    const double wall = now - (a.tv_sec + a.tv_nsec * 1e-9);
    const double rel = wall > 0 ? std::abs(measured - wall) / wall : 1.0;
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "counter agrees with CLOCK_MONOTONIC_RAW to %.3f%%", rel * 100);
    check(rel < 0.02, buf);
}

void header_checks() {
    check(LR_HDR == 24, "wire header is 24 bytes");
    check(LR_MAX_PAYLOAD == 1472 - LR_HDR - LR_TAG,
          "maximum payload fills an Ethernet MTU exactly");
    // The nonce is derived from (epoch, seq) rather than carried, so two packets
    // in one epoch must never share one.
    proto::Key secret{};
    proto::KeySchedule ks(secret, "aes-256-gcm", 0);
    uint8_t n1[12], n2[12];
    ks.nonce(0, 1, n1);
    ks.nonce(0, 2, n2);
    check(std::memcmp(n1, n2, 12) != 0, "derived nonces are distinct within an epoch");
}

} // namespace

int main() {
    printf("linerate self-test  (%s, %s)\n", LR_GIT_COMMIT, LR_COMPILER);
    const auto& c = plat::caps();
    printf("  host: %s %s, %d physical cores, class '%s', isolation '%s'\n",
           c.os.c_str(), c.arch.c_str(), c.physical_cores,
           plat::host_class_name(c.klass()), c.isolation_tier.c_str());
    printf("  crypto: openssl %s | ours gcm=%s chacha=%s\n",
           c.openssl_version.c_str(), lr_simd_gcm_backend(), lr_simd_chap_backend());
    printf("  interface: %s (%s) %s\n",
           c.nic_name.empty() ? "none" : c.nic_name.c_str(),
           c.nic_driver.c_str(), c.nic_addr.c_str());
    {
        std::string backends;
        for (const auto& b : transport_backends()) backends += b + " ";
        printf("  transports: %s\n", backends.c_str());
    }
    printf("\n");

    // RFC 8439 §2.8.2, the ChaCha20-Poly1305 AEAD example.
    kat("chacha20-poly1305 (RFC 8439 2.8.2)", "chacha20-poly1305",
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f",
        "070000004041424344454647",
        "50515253c0c1c2c3c4c5c6c7",
        "4c616469657320616e642047656e746c656d656e206f662074686520636c617373206f66"
        "202739393a204966204920636f756c64206f6666657220796f75206f6e6c79206f6e6520"
        "74697020666f7220746865206675747572652c2073756e73637265656e20776f756c6420"
        "62652069742e",
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e"
        "8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c"
        "9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d265"
        "86cec64b6116",
        "1ae10b594f09e26a7e902ecbd0600691");

    // The same vector must produce the same answer through the implementation
    // written here.
    kat("lr-chacha20-poly1305 (RFC 8439 2.8.2)", "lr-chacha20-poly1305",
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f",
        "070000004041424344454647",
        "50515253c0c1c2c3c4c5c6c7",
        "4c616469657320616e642047656e746c656d656e206f662074686520636c617373206f66"
        "202739393a204966204920636f756c64206f6666657220796f75206f6e6c79206f6e6520"
        "74697020666f7220746865206675747572652c2073756e73637265656e20776f756c6420"
        "62652069742e",
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e"
        "8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c"
        "9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d265"
        "86cec64b6116",
        "1ae10b594f09e26a7e902ecbd0600691");

    // GCM specification, test case 16 (AES-256, 60-byte plaintext, 20-byte AAD).
    kat("aes-256-gcm (GCM spec case 16)", "aes-256-gcm",
        "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
        "cafebabefacedbaddecaf888",
        "feedfacedeadbeeffeedfacedeadbeefabaddad2",
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95"
        "956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
        "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48"
        "590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
        "76fc6ece0f4e1768cddf8853bb2d551b");

    kat("lr-aes-256-gcm (GCM spec case 16)", "lr-aes-256-gcm",
        "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
        "cafebabefacedbaddecaf888",
        "feedfacedeadbeeffeedfacedeadbeefabaddad2",
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95"
        "956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
        "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48"
        "590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
        "76fc6ece0f4e1768cddf8853bb2d551b");

    cross_check("lr-aes-256-gcm", "aes-256-gcm");
    cross_check("lr-chacha20-poly1305", "chacha20-poly1305");

    protocol_checks();
    header_checks();
    transport_round_trip();
    clock_checks();

    printf("\n");
    if (failures == 0) {
        printf("OK  all checks passed\n");
        return 0;
    }
    printf("FAILED  %d check(s)\n", failures);
    return 1;
}
