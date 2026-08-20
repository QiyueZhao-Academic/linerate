// transport_mmsg.cpp — the batched backend: one syscall per B packets.
//
// Linux only. recvmmsg and sendmmsg amortise the syscall entry, the socket lock
// and the credential check across a batch, leaving the per-packet work in the
// kernel unchanged. That separation is exactly what E3 exploits: changing only
// the backend moves the syscall term of a and leaves the cipher term alone, so
// the difference between backends is a direct measurement of the syscall share.
#include "lr/transport.hpp"

#if defined(__linux__)

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace lr {

int  lr_open_udp_socket(const TransportConfig& cfg, uint16_t* bound_port, std::string* err);
void lr_fill_peer(const TransportConfig& cfg, sockaddr_in* out);

namespace {

class MmsgTransport final : public Transport {
public:
    MmsgTransport(int fd, const TransportConfig& cfg)
        : fd_(fd), batch_(cfg.batch < 1 ? 1 : cfg.batch) {
        lr_fill_peer(cfg, &peer_);
        msgs_.resize(batch_);
        iov_.resize(batch_);
    }
    ~MmsgTransport() override { if (fd_ >= 0) ::close(fd_); }

    int recv_batch(PacketBuf* bufs, int max_n) override {
        int n = max_n > batch_ ? batch_ : max_n;
        // The iovec array is rebuilt per call because the caller owns the arena
        // and may hand back different slots. This is a handful of stores; it is
        // charged to the backend and is part of what E3 measures.
        for (int i = 0; i < n; ++i) {
            iov_[i].iov_base = bufs[i].base;
            iov_[i].iov_len  = bufs[i].cap;
            std::memset(&msgs_[i], 0, sizeof(mmsghdr));
            msgs_[i].msg_hdr.msg_iov    = &iov_[i];
            msgs_[i].msg_hdr.msg_iovlen = 1;
        }
        int r = ::recvmmsg(fd_, msgs_.data(), n, MSG_DONTWAIT, nullptr);
        sys_++;
        if (r <= 0) return 0;
        for (int i = 0; i < r; ++i) bufs[i].len = msgs_[i].msg_len;
        return r;
    }

    int send_batch(PacketBuf* bufs, int n) override {
        int total = 0;
        while (total < n) {
            int chunk = n - total;
            if (chunk > batch_) chunk = batch_;
            for (int i = 0; i < chunk; ++i) {
                iov_[i].iov_base = bufs[total + i].base;
                iov_[i].iov_len  = bufs[total + i].len;
                std::memset(&msgs_[i], 0, sizeof(mmsghdr));
                msgs_[i].msg_hdr.msg_iov     = &iov_[i];
                msgs_[i].msg_hdr.msg_iovlen  = 1;
                msgs_[i].msg_hdr.msg_name    = &peer_;
                msgs_[i].msg_hdr.msg_namelen = sizeof(peer_);
            }
            int s = ::sendmmsg(fd_, msgs_.data(), chunk, 0);
            sys_++;
            if (s < 0) {
                if (errno == ECONNREFUSED) { total += chunk; continue; }
                break;
            }
            total += s;
            if (s < chunk) break;
        }
        return total;
    }

    int         max_batch() const override { return batch_; }
    const char* name()      const override { return "mmsg"; }
    int         fd()        const override { return fd_; }
    uint64_t    syscalls()  const override { return sys_; }

private:
    int fd_;
    int batch_;
    sockaddr_in peer_{};
    std::vector<mmsghdr> msgs_;
    std::vector<iovec>   iov_;
    uint64_t sys_ = 0;
};

} // namespace

std::unique_ptr<Transport> make_transport_mmsg(const TransportConfig& cfg, std::string* err) {
    uint16_t port = 0;
    int fd = lr_open_udp_socket(cfg, &port, err);
    if (fd < 0) return nullptr;
    return std::make_unique<MmsgTransport>(fd, cfg);
}

bool batched_io_available() { return true; }

} // namespace lr

#else   // not Linux

#include <string>
namespace lr {
std::unique_ptr<Transport> make_transport_mmsg(const TransportConfig&, std::string* err) {
    if (err) *err = "batched syscalls (recvmmsg/sendmmsg) are Linux-only";
    return nullptr;
}
bool batched_io_available() { return false; }
} // namespace lr

#endif
