// worker.hpp — the shard model and the one hot path every experiment measures.
//
// A shard owns a socket, a key schedule, a replay window and an arena. It shares
// nothing mutable with any other shard: no lock, no atomic, no shared counter in
// the per-packet path. If a profile of a scaling run shows contention, that is a
// bug in this file, not a finding about scalability.
//
// The hot path is identical across all experiments, so that a difference between
// two runs is attributable to the factor that was varied and not to a different
// amount of work:
//
//     recv_batch -> replay check -> aead.open -> rekey check -> aead.seal
//                -> send_batch
//
// This is the shape of a decrypt-then-re-encrypt forwarding element: a tunnel
// endpoint that terminates one security association and originates another. It
// passes over each payload byte twice under the cipher, which makes the
// per-byte term b large enough to separate cleanly from the per-packet term a.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lr/aead.hpp"
#include "lr/proto.hpp"
#include "lr/stats.hpp"
#include "lr/transport.hpp"

namespace lr {

// Header carried ahead of the ciphertext in every packet.
//
// There is no explicit nonce. The nonce is derived from (epoch, seq) through
// the key schedule, which is what deployed protocols do and what lets this
// header be 24 bytes rather than 36. The whole header is authenticated as
// associated data, so a tampered epoch, sequence number or timestamp is
// rejected along with a tampered payload.
#pragma pack(push, 1)
struct WireHdr {
    uint32_t magic;      // LR_MAGIC
    uint8_t  type;       // WireType
    uint8_t  epoch;      // rekey epoch, low 8 bits
    uint16_t flags;      // reserved; authenticated, so it cannot be rewritten
    uint64_t seq;        // 64-bit: the replay window and the nonce both need it
    uint64_t tsc;        // origin tick on the sender's clock, used by E5
};
#pragma pack(pop)

enum WireType : uint8_t { WT_DATA = 1, WT_HS_INIT = 2, WT_HS_RESP = 3, WT_ECHO = 4 };

static constexpr uint32_t LR_MAGIC = 0x4C525032u;   // "LRP2"
static constexpr size_t   LR_HDR   = sizeof(WireHdr);   // 24
static constexpr size_t   LR_TAG   = 16;

// Largest plaintext payload that keeps the UDP datagram within a 1500-byte
// Ethernet MTU: 1500 - 20 (IPv4) - 8 (UDP) - LR_HDR - LR_TAG.
static constexpr size_t   LR_MAX_PAYLOAD = 1472 - LR_HDR - LR_TAG;   // 1432

// Where the packets a shard processes come from.
enum class Source {
    Loopback,   // a loader socket on this host stages them; used by the self-test
                // and by any host without a peer
    Wire,       // a remote generator sends them across a physical interface
};

struct ShardConfig {
    std::string cipher   = "aes-256-gcm";
    std::string backend  = "posix";
    int         batch    = 1;
    size_t      payload  = 512;     // plaintext bytes carried per packet
    int         cpu      = -1;      // -1 = do not pin
    bool        latency  = false;   // record per-packet latency in the histogram
    bool        replay   = true;    // run the anti-replay window
    uint64_t    epoch_packets = 0;  // rekey period; 0 = never rekey

    Source      source   = Source::Loopback;
    std::string bind_addr = "127.0.0.1";
    uint16_t    bind_port = 0;
    // Where re-encrypted packets go. Empty means a local black hole, which is
    // what the loopback mode uses; in wire mode this is the generator host, so
    // the egress path is a real transmit through a real driver.
    std::string peer_addr = "";
    uint16_t    peer_port = 0;
};

struct ShardStats {
    uint64_t packets    = 0;
    uint64_t bytes      = 0;    // plaintext payload bytes
    uint64_t auth_fail  = 0;
    uint64_t replay_drop= 0;
    uint64_t rekeys     = 0;
    uint64_t ticks_busy = 0;    // ticks inside the hot path only
    uint64_t syscalls   = 0;
    stats::LatencyHist lat;
};

class Shard {
public:
    Shard(const ShardConfig& cfg, const uint8_t* secret, std::string* err);
    ~Shard();

    // Build one wire packet into `out`, which must hold wire_len() bytes.
    // Sealing happens here, outside every timed region, so the loader's cost
    // never enters a measurement of the data plane.
    size_t make_packet(uint64_t seq, uint8_t* out);

    // Stage up to n packets into this shard's own receive queue. Loopback mode
    // only; returns 0 in wire mode, where a remote generator is the source.
    int preload(int n);

    // Process everything queued, up to `budget` packets. Returns the count
    // processed and accumulates hot-path ticks into stats().ticks_busy.
    int drain(int budget);

    const ShardStats& stats() const { return st_; }
    void     reset_stats();
    uint16_t local_port() const { return port_; }
    size_t   wire_len()   const { return LR_HDR + cfg_.payload + LR_TAG; }
    const ShardConfig& config() const { return cfg_; }
    const char* aead_backend() const;

private:
    ShardConfig cfg_;
    std::unique_ptr<proto::KeySchedule> keys_;
    std::unique_ptr<proto::ReplayWindow> replay_;
    std::unique_ptr<Transport> tx_;
    std::vector<uint8_t> arena_;     // receive slots
    std::vector<uint8_t> plain_;     // decrypted staging
    std::vector<uint8_t> out_;       // re-sealed packets
    std::vector<uint8_t> tmpl_;      // loader scratch
    std::vector<PacketBuf> rslots_, sslots_;
    int      loader_fd_ = -1;        // sends into our own port, untimed
    int      hole_fd_   = -1;        // black hole for the transmit side
    uint16_t port_      = 0;
    uint16_t hole_port_ = 0;
    // The transport counts syscalls from construction, but a measurement starts
    // at reset_stats(). Without this baseline the warm-up's syscalls would be
    // charged to the measured packets, which inflates the ratio by whatever the
    // warm-up happened to cost.
    uint64_t syscall_base_ = 0;
    uint64_t seq_       = 0;
    ShardStats st_;
};

// A standalone open-loop generator, used where the load has to come from this
// process. It reuses a shard's sealing so the packets it emits are
// indistinguishable from those a remote generator sends.
class Generator {
public:
    Generator(Shard& proto, const std::string& dst_addr, uint16_t dst_port, std::string* err);
    ~Generator();
    bool     emit(uint64_t seq);
    uint64_t sent()    const { return sent_; }
    uint64_t refused() const { return refused_; }
private:
    Shard&   proto_;
    int      fd_ = -1;
    std::vector<uint8_t> buf_;
    std::vector<uint8_t> dst_;   // sockaddr_in, kept opaque to avoid a header
    uint64_t sent_ = 0, refused_ = 0;
};

} // namespace lr
