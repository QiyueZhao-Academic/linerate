// transport_posix.cpp — the portable backend: one syscall per packet.
//
// This is the honest baseline. It exists on both macOS and Linux, which is what
// lets the same binary be exercised on the development machine, and it is the
// reference against which every other backend's saving is measured in E3.
#include "lr/transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace lr {

// Shared socket setup, used by every backend.
int lr_open_udp_socket(const TransportConfig& cfg, uint16_t* bound_port, std::string* err) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { if (err) *err = std::string("socket: ") + strerror(errno); return -1; }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    if (cfg.reuseport) ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    // Large buffers let an experiment stage a full drain window in the kernel,
    // so the timed region contains the data plane and not the loader.
    int rb = cfg.rcvbuf, sb = cfg.sndbuf;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof(sb));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(cfg.bind_port);
    if (cfg.bind_addr == "0.0.0.0") {
        a.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, cfg.bind_addr.c_str(), &a.sin_addr) != 1) {
        if (err) *err = "bad bind address: " + cfg.bind_addr;
        ::close(fd); return -1;
    }
    if (::bind(fd, (sockaddr*)&a, sizeof(a)) != 0) {
        if (err) *err = std::string("bind ") + cfg.bind_addr + ": " + strerror(errno);
        ::close(fd); return -1;
    }
    socklen_t alen = sizeof(a);
    ::getsockname(fd, (sockaddr*)&a, &alen);
    if (bound_port) *bound_port = ntohs(a.sin_port);

    // Non-blocking: an empty socket must cost one failed syscall, not a
    // scheduler round trip that would appear in the measurement as a stall.
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

void lr_fill_peer(const TransportConfig& cfg, sockaddr_in* out) {
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons(cfg.peer_port);
    ::inet_pton(AF_INET, cfg.peer_addr.c_str(), &out->sin_addr);
}

namespace {

class PosixTransport final : public Transport {
public:
    PosixTransport(int fd, const TransportConfig& cfg) : fd_(fd) {
        lr_fill_peer(cfg, &peer_);
    }
    ~PosixTransport() override { if (fd_ >= 0) ::close(fd_); }

    int recv_batch(PacketBuf* bufs, int max_n) override {
        // max_batch() is 1, but the loop is written for max_n so that the
        // harness code path is identical for every backend. With max_n == 1 the
        // loop runs once and issues exactly one recv.
        int n = 0;
        for (; n < max_n; ++n) {
            ssize_t r = ::recv(fd_, bufs[n].base, bufs[n].cap, 0);
            sys_++;
            if (r <= 0) break;
            bufs[n].len = (size_t)r;
        }
        return n;
    }

    int send_batch(PacketBuf* bufs, int n) override {
        int sent = 0;
        for (; sent < n; ++sent) {
            ssize_t s = ::sendto(fd_, bufs[sent].base, bufs[sent].len, 0,
                                 (sockaddr*)&peer_, sizeof(peer_));
            sys_++;
            if (s < 0) {
                // ECONNREFUSED arrives asynchronously when the destination port
                // has no listener. It is expected and is not a transmit failure.
                if (errno == ECONNREFUSED) continue;
                break;
            }
        }
        return sent;
    }

    int         max_batch() const override { return 1; }
    const char* name()      const override { return "posix"; }
    int         fd()        const override { return fd_; }
    uint64_t    syscalls()  const override { return sys_; }

private:
    int fd_;
    sockaddr_in peer_{};
    uint64_t sys_ = 0;
};

} // namespace

std::unique_ptr<Transport> make_transport_posix(const TransportConfig& cfg, std::string* err) {
    uint16_t port = 0;
    int fd = lr_open_udp_socket(cfg, &port, err);
    if (fd < 0) return nullptr;
    return std::make_unique<PosixTransport>(fd, cfg);
}

} // namespace lr
