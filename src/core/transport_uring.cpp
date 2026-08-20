// transport_uring.cpp — io_uring, and io_uring with a kernel-side submission
// poller.
//
// Why this backend exists. The batching result in E3 bounds how much of the
// fixed per-packet cost a is syscall entry, but only from one side: recvmmsg
// still enters the kernel once per batch. IORING_SETUP_SQPOLL removes even
// that. A kernel thread polls the submission queue, so a submission costs a
// store and a memory barrier and no syscall at all, and completions are read
// out of a shared ring the same way. That is as close to a kernel-bypass data
// path as an unprivileged process reaches without a userspace driver, and it
// turns "batching bounds what a bypass path could recover" from an inference
// into a measurement.
//
// What it is not: it is not DPDK. The packet still traverses the kernel network
// stack, the socket layer and the skb allocator. What disappears is the mode
// switch, which is exactly the component the batching sweep isolates.
#include "lr/transport.hpp"

#if defined(LR_HAVE_URING)

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace lr {

int  lr_open_udp_socket(const TransportConfig& cfg, uint16_t* bound_port, std::string* err);
void lr_fill_peer(const TransportConfig& cfg, sockaddr_in* out);

namespace {

// One submission slot: its own msghdr and iovec, because the kernel reads them
// asynchronously and a shared scratch buffer would be rewritten underneath an
// in-flight operation.
struct Slot {
    msghdr mh{};
    iovec  iov{};
    int    idx = 0;
};

class UringTransport final : public Transport {
public:
    UringTransport(int fd, const TransportConfig& cfg, bool sqpoll)
        : fd_(fd), batch_(cfg.batch < 1 ? 1 : cfg.batch), sqpoll_(sqpoll) {
        lr_fill_peer(cfg, &peer_);
        slots_.resize(batch_);
        for (int i = 0; i < batch_; ++i) slots_[i].idx = i;
    }
    ~UringTransport() override {
        if (ring_ok_) io_uring_queue_exit(&ring_);
        if (fd_ >= 0) ::close(fd_);
    }

    bool init(std::string* err) {
        io_uring_params p{};
        // The ring is sized for two batches so that a receive round and the
        // transmit round it feeds never contend for submission slots.
        unsigned entries = (unsigned)(batch_ * 2);
        if (entries < 8) entries = 8;
        if (sqpoll_) {
            p.flags |= IORING_SETUP_SQPOLL;
            p.sq_thread_idle = 2000;   // ms before the poller sleeps
        }
        int r = io_uring_queue_init_params(entries, &ring_, &p);
        if (r < 0) { if (err) *err = std::string("io_uring_queue_init: ") + strerror(-r); return false; }
        ring_ok_ = true;
        // Registering the descriptor lets the kernel skip the file-table lookup
        // per operation, which is part of what this backend is measuring.
        int fds[1] = {fd_};
        if (io_uring_register_files(&ring_, fds, 1) == 0) reg_ok_ = true;
        return true;
    }

    int recv_batch(PacketBuf* bufs, int max_n) override {
        int n = max_n > batch_ ? batch_ : max_n;
        if (n <= 0) return 0;
        for (int i = 0; i < n; ++i) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (!sqe) { n = i; break; }
            Slot& s = slots_[i];
            s.iov.iov_base = bufs[i].base;
            s.iov.iov_len  = bufs[i].cap;
            std::memset(&s.mh, 0, sizeof(s.mh));
            s.mh.msg_iov    = &s.iov;
            s.mh.msg_iovlen = 1;
            io_uring_prep_recvmsg(sqe, reg_ok_ ? 0 : fd_, &s.mh, MSG_DONTWAIT);
            if (reg_ok_) sqe->flags |= IOSQE_FIXED_FILE;
            io_uring_sqe_set_data(sqe, &s);
        }
        if (n <= 0) return 0;
        // With SQPOLL the kernel thread has usually already consumed the tail,
        // and io_uring_submit returns without entering the kernel at all. That
        // is the whole point of the backend, so it is counted separately.
        int sub = io_uring_submit(&ring_);
        if (!sqpoll_) sys_++;
        if (sub < 0) return 0;

        int got = 0;
        for (int i = 0; i < n; ++i) {
            io_uring_cqe* cqe = nullptr;
            int r = io_uring_peek_cqe(&ring_, &cqe);
            if (r == -EAGAIN || !cqe) {
                // Nothing ready yet. Wait only when nothing at all has arrived,
                // so that an empty socket costs one wait rather than a spin.
                if (got > 0) break;
                r = io_uring_wait_cqe_timeout(&ring_, &cqe, nullptr);
                if (!sqpoll_) sys_++;
                if (r < 0 || !cqe) break;
            }
            Slot* s = (Slot*)io_uring_cqe_get_data(cqe);
            int res = cqe->res;
            io_uring_cqe_seen(&ring_, cqe);
            if (res > 0 && s) { bufs[s->idx].len = (size_t)res; got++; }
            else if (res < 0) { /* EAGAIN on an empty socket */ }
        }
        // Any submissions that produced no completion are still in flight; drain
        // them so the ring does not accumulate stale entries across calls.
        drain_pending();
        return got;
    }

    int send_batch(PacketBuf* bufs, int n) override {
        int total = 0;
        while (total < n) {
            int chunk = n - total;
            if (chunk > batch_) chunk = batch_;
            int queued = 0;
            for (int i = 0; i < chunk; ++i) {
                io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
                if (!sqe) break;
                Slot& s = slots_[i];
                s.iov.iov_base = bufs[total + i].base;
                s.iov.iov_len  = bufs[total + i].len;
                std::memset(&s.mh, 0, sizeof(s.mh));
                s.mh.msg_iov     = &s.iov;
                s.mh.msg_iovlen  = 1;
                s.mh.msg_name    = &peer_;
                s.mh.msg_namelen = sizeof(peer_);
                io_uring_prep_sendmsg(sqe, reg_ok_ ? 0 : fd_, &s.mh, 0);
                if (reg_ok_) sqe->flags |= IOSQE_FIXED_FILE;
                io_uring_sqe_set_data(sqe, &s);
                queued++;
            }
            if (queued == 0) break;
            io_uring_submit(&ring_);
            if (!sqpoll_) sys_++;
            for (int i = 0; i < queued; ++i) {
                io_uring_cqe* cqe = nullptr;
                int r = io_uring_wait_cqe(&ring_, &cqe);
                if (!sqpoll_) sys_++;
                if (r < 0 || !cqe) break;
                io_uring_cqe_seen(&ring_, cqe);
            }
            total += queued;
        }
        return total;
    }

    int         max_batch() const override { return batch_; }
    const char* name()      const override { return sqpoll_ ? "uring-sqpoll" : "uring"; }
    int         fd()        const override { return fd_; }
    uint64_t    syscalls()  const override { return sys_; }

private:
    void drain_pending() {
        io_uring_cqe* cqe = nullptr;
        while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe)
            io_uring_cqe_seen(&ring_, cqe);
    }

    int  fd_;
    int  batch_;
    bool sqpoll_;
    bool ring_ok_ = false;
    bool reg_ok_  = false;
    io_uring ring_{};
    sockaddr_in peer_{};
    std::vector<Slot> slots_;
    uint64_t sys_ = 0;
};

bool probe(bool sqpoll, std::string* why) {
    io_uring ring{};
    io_uring_params p{};
    if (sqpoll) { p.flags |= IORING_SETUP_SQPOLL; p.sq_thread_idle = 100; }
    int r = io_uring_queue_init_params(8, &ring, &p);
    if (r < 0) {
        if (why) *why = std::string(sqpoll ? "IORING_SETUP_SQPOLL" : "io_uring")
                        + " refused: " + strerror(-r);
        return false;
    }
    io_uring_queue_exit(&ring);
    return true;
}

} // namespace

std::unique_ptr<Transport> make_transport_uring(const TransportConfig& cfg,
                                                bool sqpoll, std::string* err) {
    uint16_t port = 0;
    int fd = lr_open_udp_socket(cfg, &port, err);
    if (fd < 0) return nullptr;
    auto t = std::make_unique<UringTransport>(fd, cfg, sqpoll);
    if (!t->init(err)) return nullptr;
    return t;
}

bool uring_available(std::string* why) {
    static std::string cached;
    static bool ok = probe(false, &cached);
    if (!ok && why) *why = cached;
    return ok;
}

bool uring_sqpoll_available(std::string* why) {
    static std::string cached;
    static bool ok = uring_available(nullptr) && probe(true, &cached);
    if (!ok && why) *why = cached.empty() ? "io_uring unavailable" : cached;
    return ok;
}

} // namespace lr

#else   // built without liburing

#include <string>
namespace lr {
std::unique_ptr<Transport> make_transport_uring(const TransportConfig&, bool,
                                                std::string* err) {
    if (err) *err = "built without liburing; install liburing-dev and rebuild";
    return nullptr;
}
bool uring_available(std::string* why) {
    if (why) *why = "built without liburing (liburing-dev absent at configure time)";
    return false;
}
bool uring_sqpoll_available(std::string* why) {
    if (why) *why = "built without liburing (liburing-dev absent at configure time)";
    return false;
}
} // namespace lr

#endif
