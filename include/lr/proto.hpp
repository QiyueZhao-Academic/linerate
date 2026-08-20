// proto.hpp — the three things that make a data plane a protocol endpoint
// rather than an encryption benchmark: a key agreement, an anti-replay window
// and a rekey schedule.
//
// Why they belong in a cost study. Each one adds work to the per-packet path or
// to the connection, and each one is routinely left out of "how fast is AES"
// measurements, which is exactly why those measurements do not predict what a
// tunnel endpoint achieves. The replay window is a per-packet cost that scales
// with nothing, so it lands squarely in the fixed term a and the harness can
// measure it by turning it off. Rekeying is amortised over an epoch, so it
// appears as a periodic spike that a median hides and a tail percentile does
// not. The handshake is a per-connection cost that never appears in a
// steady-state number at all, so it is reported separately, in microseconds
// per connection rather than cycles per packet.
//
// The design follows what deployed protocols do, at the smallest size that is
// still honest:
//
//   handshake   ephemeral X25519 on both sides, authenticated by a
//               pre-shared key, mixed through HKDF-SHA256 into two
//               directional traffic secrets. This is the shape of a Noise
//               NKpsk0 pattern. It is not a substitute for a reviewed
//               protocol and the report says so; it is enough to have a real
//               key agreement in the path instead of a hard-coded key.
//
//   replay      a sliding bitmap over 64-bit sequence numbers, RFC 4303 §3.4.3.
//               Width is configurable; the default is 1024 packets.
//
//   rekey       every epoch the traffic secret is ratcheted forward with HKDF
//               and the epoch number in the header selects the key. One
//               previous epoch is retained so that packets reordered across a
//               rekey boundary still verify.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lr/aead.hpp"

namespace lr::proto {

static constexpr size_t kKeyLen    = 32;
static constexpr size_t kPubLen    = 32;
static constexpr size_t kPskLen    = 32;
static constexpr size_t kSaltLen   = 4;    // per-epoch nonce salt

using Key  = std::array<uint8_t, kKeyLen>;
using Salt = std::array<uint8_t, kSaltLen>;

// ---------------------------------------------------------------------------
// Anti-replay
// ---------------------------------------------------------------------------
//
// A packet is accepted when its sequence number is ahead of the window, or
// inside the window and not already seen. Everything else is dropped. The cost
// is a shift and two bit operations in the common case, which is what makes it
// worth measuring rather than assuming.
class ReplayWindow {
public:
    explicit ReplayWindow(size_t width_bits = 1024);

    // Returns true when the packet is admissible, and records it. Returns false
    // for a duplicate or a packet older than the window.
    bool accept(uint64_t seq);

    void     reset();
    uint64_t highest()   const { return high_; }
    uint64_t duplicates()const { return dup_; }
    uint64_t too_old()   const { return old_; }
    size_t   width()     const { return width_; }

private:
    size_t                width_;
    std::vector<uint64_t> bits_;      // width_/64 words, bit i = high_ - i
    uint64_t              high_ = 0;
    bool                  seen_ = false;
    uint64_t              dup_ = 0, old_ = 0;
};

// ---------------------------------------------------------------------------
// Key schedule
// ---------------------------------------------------------------------------
//
// One traffic secret per direction, ratcheted per epoch. The AEAD object for an
// epoch is built once and cached; a rekey builds one new object and retires the
// oldest, so the steady-state per-packet path never allocates.
class KeySchedule {
public:
    // `secret` is the directional traffic secret produced by the handshake.
    KeySchedule(const Key& secret, const std::string& cipher, uint64_t packets_per_epoch);

    // The cipher for an epoch, or nullptr when the epoch is neither current nor
    // the one immediately before it.
    Aead* for_epoch(uint8_t epoch);

    // Nonce for (epoch, seq): the epoch salt followed by the big-endian
    // sequence number. Derived rather than carried, which is what lets the wire
    // header be 24 bytes instead of 36.
    void nonce(uint8_t epoch, uint64_t seq, uint8_t out[12]) const;

    // Advance if this sequence number has crossed the epoch boundary. Returns
    // true when a rekey happened.
    bool maybe_rekey(uint64_t seq);

    uint8_t  epoch()          const { return epoch_; }
    uint64_t rekeys()         const { return rekeys_; }
    uint64_t packets_per_epoch() const { return per_epoch_; }
    const char* cipher_backend() const;

private:
    void derive(uint8_t epoch_index);

    std::string cipher_;
    uint64_t    per_epoch_;
    Key         chain_{};
    uint8_t     epoch_ = 0;
    uint64_t    rekeys_ = 0;
    // Two live epochs: the current one and its predecessor.
    std::unique_ptr<Aead> aead_[2];
    Salt                  salt_[2]{};
    uint8_t               tag_[2] = {0, 0};
};

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------
struct HandshakeResult {
    Key  tx{};              // secret for data this endpoint sends
    Key  rx{};              // secret for data this endpoint receives
    bool ok = false;
    std::string error;
    double micros = 0;      // wall time of the local half, for reporting
};

// Message sizes on the wire. Both messages are fixed-length, so a handshake
// cannot be used to probe for a length oracle and the parser has no loop.
static constexpr size_t kHsInitLen = kPubLen + 16;   // ephemeral public + PSK tag
static constexpr size_t kHsRespLen = kPubLen + 16;

class Handshake {
public:
    explicit Handshake(const uint8_t psk[kPskLen]);
    ~Handshake();

    // Initiator: writes kHsInitLen bytes.
    bool write_init(uint8_t* out);
    // Responder: consumes an init message and writes kHsRespLen bytes.
    bool read_init_write_resp(const uint8_t* in, uint8_t* out, HandshakeResult* r);
    // Initiator: consumes the response and completes.
    bool read_resp(const uint8_t* in, HandshakeResult* r);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// A local, in-process handshake between two fresh endpoints. Used by the
// self-test, and by the experiment that reports handshake cost.
HandshakeResult handshake_pair(const uint8_t psk[kPskLen], Key* initiator_tx,
                               Key* initiator_rx, Key* responder_tx, Key* responder_rx);

// The fixed pre-shared key the benchmark uses. A benchmark, not a deployment:
// the value is a constant so two runs are comparable, and the report says so.
const uint8_t* bench_psk();

} // namespace lr::proto
