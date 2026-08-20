// worker.cpp — the shard and the timed hot path.
#include "lr/worker.hpp"
#include "lr/clock.hpp"
#include "lr/platform.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lr {
namespace {

// Open a UDP socket bound to an ephemeral port on the given address.
int open_udp(const char* addr, uint16_t* port, int rcvbuf, int sndbuf) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    if (rcvbuf > 0) ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    if (sndbuf > 0) ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    if (!addr || !addr[0] || std::strcmp(addr, "127.0.0.1") == 0)
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (::inet_pton(AF_INET, addr, &a.sin_addr) != 1)
        a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;
    if (::bind(fd, (sockaddr*)&a, sizeof(a)) != 0) { ::close(fd); return -1; }
    socklen_t l = sizeof(a);
    ::getsockname(fd, (sockaddr*)&a, &l);
    if (port) *port = ntohs(a.sin_port);
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

} // namespace

Shard::Shard(const ShardConfig& cfg, const uint8_t* secret, std::string* err) : cfg_(cfg) {
    if (cfg_.payload > LR_MAX_PAYLOAD) cfg_.payload = LR_MAX_PAYLOAD;
    if (cfg_.cpu >= 0) plat::pin_to_cpu(cfg_.cpu);

    proto::Key k{};
    std::memcpy(k.data(), secret, proto::kKeyLen);
    keys_ = std::make_unique<proto::KeySchedule>(k, cfg_.cipher, cfg_.epoch_packets);
    if (!keys_->for_epoch(0)) { if (err) *err = "unknown cipher: " + cfg_.cipher; return; }
    if (cfg_.replay) replay_ = std::make_unique<proto::ReplayWindow>(1024);

    TransportConfig tc;
    tc.backend   = cfg_.backend;
    tc.batch     = cfg_.batch;
    tc.reuseport = false;

    if (cfg_.source == Source::Wire) {
        // Bind the real interface. Re-encrypted packets go back to the peer, so
        // the transmit side is a genuine driver transmit rather than a loopback
        // write into a black hole.
        tc.bind_addr = cfg_.bind_addr.empty() ? "0.0.0.0" : cfg_.bind_addr;
        tc.bind_port = cfg_.bind_port;
        tc.peer_addr = cfg_.peer_addr.empty() ? "127.0.0.1" : cfg_.peer_addr;
        tc.peer_port = cfg_.peer_port;
    } else {
        // The transmit side points at a black-hole socket: bound, never drained,
        // and given the smallest receive buffer the kernel will accept. It
        // reaches steady state after a few packets and drops every one
        // thereafter, which keeps the cost of a send constant for the whole run.
        // Sending to a closed port instead would generate ICMP unreachable
        // traffic and an error return path whose cost varies with rate.
        hole_fd_ = open_udp("127.0.0.1", &hole_port_, 1024, 1024);
        if (hole_fd_ < 0) { if (err) *err = "black-hole socket failed"; return; }
        tc.bind_addr = "127.0.0.1";
        tc.peer_addr = "127.0.0.1";
        tc.peer_port = hole_port_;
    }

    tx_ = make_transport(tc, err);
    if (!tx_) return;
    port_ = transport_local_port(*tx_);

    if (cfg_.source == Source::Loopback) {
        loader_fd_ = open_udp("127.0.0.1", nullptr, 1024, 32 << 20);
        if (loader_fd_ < 0) { if (err) *err = "loader socket failed"; return; }
    }

    const size_t wl    = wire_len();
    const int    slots = tx_->max_batch();
    arena_.assign((size_t)slots * (wl + 64), 0);
    out_.assign((size_t)slots * (wl + 64), 0);
    plain_.assign(cfg_.payload + 64, 0);
    tmpl_.assign(wl + 64, 0);
    rslots_.resize(slots);
    sslots_.resize(slots);
    for (int i = 0; i < slots; ++i) {
        rslots_[i].base = arena_.data() + (size_t)i * (wl + 64);
        rslots_[i].cap  = wl + 64;
        sslots_[i].base = out_.data() + (size_t)i * (wl + 64);
        sslots_[i].cap  = wl + 64;
    }
    // Deterministic plaintext: the same bytes on every run, so a cipher's cost
    // cannot depend on the data and two runs are comparable byte for byte.
    for (size_t i = 0; i < cfg_.payload; ++i) plain_[i] = (uint8_t)(i * 31u + 7u);
}

Shard::~Shard() {
    if (loader_fd_ >= 0) ::close(loader_fd_);
    if (hole_fd_   >= 0) ::close(hole_fd_);
}

void Shard::reset_stats() {
    st_ = ShardStats{};
    syscall_base_ = tx_ ? tx_->syscalls() : 0;
}

const char* Shard::aead_backend() const {
    return keys_ ? keys_->cipher_backend() : "none";
}

size_t Shard::make_packet(uint64_t seq, uint8_t* outp) {
    keys_->maybe_rekey(seq);
    Aead* a = keys_->for_epoch(keys_->epoch());
    if (!a) return 0;
    WireHdr h{};
    h.magic = LR_MAGIC;
    h.type  = WT_DATA;
    h.epoch = keys_->epoch();
    h.flags = 0;
    h.seq   = seq;
    h.tsc   = clk::ticks();
    std::memcpy(outp, &h, LR_HDR);
    uint8_t nonce[12];
    keys_->nonce(h.epoch, h.seq, nonce);
    // The header is authenticated as associated data, so a tampered sequence
    // number, epoch or timestamp is rejected along with a tampered payload.
    size_t n = a->seal(nonce, outp, LR_HDR, plain_.data(), cfg_.payload, outp + LR_HDR);
    return n ? LR_HDR + n : 0;
}

int Shard::preload(int n) {
    if (cfg_.source != Source::Loopback || loader_fd_ < 0) return 0;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(port_);
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        size_t len = make_packet(seq_++, tmpl_.data());
        if (!len) break;
        if (::sendto(loader_fd_, tmpl_.data(), len, 0, (sockaddr*)&dst, sizeof(dst)) > 0) ok++;
    }
    return ok;
}

int Shard::drain(int budget) {
    const int  slots = tx_->max_batch();
    const bool lat   = cfg_.latency;
    int processed = 0;

    const uint64_t t0 = clk::ticks();
    while (processed < budget) {
        int want = budget - processed;
        if (want > slots) want = slots;
        int n = tx_->recv_batch(rslots_.data(), want);
        if (n <= 0) break;

        int to_send = 0;
        for (int i = 0; i < n; ++i) {
            const uint8_t* p   = rslots_[i].base;
            const size_t   len = rslots_[i].len;
            if (len < LR_HDR + LR_TAG) continue;

            WireHdr h{};
            std::memcpy(&h, p, LR_HDR);
            if (h.magic != LR_MAGIC) continue;

            // Anti-replay before the cipher. A replayed packet must not cost a
            // decryption, which is the whole point of putting the window first;
            // it is also why the window's cost lands in the fixed term a.
            if (replay_ && !replay_->accept(h.seq)) { st_.replay_drop++; continue; }

            Aead* a = keys_->for_epoch(h.epoch);
            if (!a) { st_.auth_fail++; continue; }
            uint8_t nonce[12];
            keys_->nonce(h.epoch, h.seq, nonce);
            long pl = a->open(nonce, p, LR_HDR, p + LR_HDR, len - LR_HDR, plain_.data());
            if (pl < 0) { st_.auth_fail++; continue; }

            // Re-encrypt under a fresh nonce, as a tunnel endpoint originating
            // a second security association would.
            const uint64_t oseq = seq_++;
            if (keys_->maybe_rekey(oseq)) st_.rekeys++;
            Aead* ao = keys_->for_epoch(keys_->epoch());
            if (!ao) continue;
            uint8_t* o = sslots_[to_send].base;
            WireHdr oh = h;
            oh.epoch = keys_->epoch();
            oh.seq   = oseq;
            std::memcpy(o, &oh, LR_HDR);
            uint8_t onon[12];
            keys_->nonce(oh.epoch, oh.seq, onon);
            size_t sl = ao->seal(onon, o, LR_HDR, plain_.data(), (size_t)pl, o + LR_HDR);
            if (!sl) continue;
            sslots_[to_send].len = LR_HDR + sl;
            to_send++;

            st_.packets++;
            st_.bytes += (uint64_t)pl;
            if (lat) st_.lat.add(clk::ticks_to_ns(clk::ticks() - h.tsc));
        }
        if (to_send) tx_->send_batch(sslots_.data(), to_send);
        processed += n;
    }
    st_.ticks_busy += clk::ticks() - t0;
    st_.syscalls = tx_->syscalls() - syscall_base_;
    return processed;
}

// ---------------------------------------------------------------------------

Generator::Generator(Shard& proto, const std::string& dst_addr, uint16_t dst_port,
                     std::string* err)
    : proto_(proto) {
    const bool loop = dst_addr.empty() || dst_addr == "127.0.0.1";
    fd_ = open_udp(loop ? "127.0.0.1" : "0.0.0.0", nullptr, 1024, 32 << 20);
    if (fd_ < 0) { if (err) *err = "generator socket failed"; return; }
    buf_.assign(proto.wire_len() + 64, 0);

    sockaddr_in d{};
    d.sin_family = AF_INET;
    d.sin_port   = htons(dst_port);
    if (loop) d.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (::inet_pton(AF_INET, dst_addr.c_str(), &d.sin_addr) != 1) {
        if (err) *err = "bad destination address: " + dst_addr;
        return;
    }
    dst_.assign((uint8_t*)&d, (uint8_t*)&d + sizeof(d));
}

Generator::~Generator() { if (fd_ >= 0) ::close(fd_); }

bool Generator::emit(uint64_t seq) {
    if (fd_ < 0 || dst_.size() != sizeof(sockaddr_in)) { refused_++; return false; }
    size_t len = proto_.make_packet(seq, buf_.data());
    if (!len) { refused_++; return false; }
    if (::sendto(fd_, buf_.data(), len, 0, (const sockaddr*)dst_.data(),
                 (socklen_t)dst_.size()) > 0) { sent_++; return true; }
    refused_++;
    return false;
}

} // namespace lr
