// transport.hpp — the I/O backend interface, and the only place where platform
// differences are allowed to live.
//
// The harness calls recv_batch() and send_batch() and nothing else. That is what
// turns E3 from a rewrite into a configuration change: each backend issues a
// different number of syscalls for the same number of packets, and the
// difference between two backends is a direct measurement of the syscall share
// of the fixed per-packet cost a.
//
//   posix        one recvfrom and one sendto per packet. The honest baseline,
//                and the only backend that exists on macOS.
//   mmsg         recvmmsg/sendmmsg: one syscall per B packets, kernel path
//                otherwise unchanged.
//   uring        io_uring: submissions batched in a shared ring, one
//                io_uring_enter per batch.
//   uring-sqpoll io_uring with a kernel-side submission poller. Submission
//                costs no syscall at all, which is as close to a kernel-bypass
//                data path as an unprivileged process gets without a userspace
//                driver. This is the backend that turns the bound in the
//                analysis from an inference into a measurement.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lr {

// A packet slot. `base` points into a caller-owned arena; the transport never
// allocates in the hot path.
struct PacketBuf {
    uint8_t* base = nullptr;
    size_t   cap  = 0;   // capacity of the slot
    size_t   len  = 0;   // bytes present (recv) or to send (send)
};

class Transport {
public:
    virtual ~Transport() = default;

    // Drain up to max_n datagrams. Returns the count received; 0 when the socket
    // is empty. Never blocks: every backend is opened non-blocking, so an empty
    // socket costs one failed syscall rather than a scheduler round trip.
    virtual int recv_batch(PacketBuf* bufs, int max_n) = 0;

    // Transmit n datagrams to the configured peer. Returns the count accepted.
    virtual int send_batch(PacketBuf* bufs, int n) = 0;

    virtual int         max_batch() const = 0;
    virtual const char* name()      const = 0;
    virtual int         fd()        const = 0;

    // Syscalls issued since construction, counted by the backend itself. E3
    // quotes measured syscalls per packet rather than the value the design
    // intends, because the two differ whenever a batch comes back short.
    virtual uint64_t    syscalls()  const = 0;
};

struct TransportConfig {
    std::string backend   = "posix";   // see the list above
    std::string bind_addr = "127.0.0.1";
    uint16_t    bind_port = 0;         // 0 = ephemeral, read back with local_port()
    std::string peer_addr = "127.0.0.1";
    uint16_t    peer_port = 9;         // discard service; no listener is required
    int         batch     = 1;         // requested depth; posix clamps to 1
    int         rcvbuf    = 16 << 20;  // large enough to stage a full drain window
    int         sndbuf    = 16 << 20;
    bool        reuseport = false;     // one socket per shard in E2
};

std::unique_ptr<Transport> make_transport(const TransportConfig& cfg, std::string* err);

// Port actually bound, after an ephemeral bind.
uint16_t transport_local_port(const Transport& t);

// Which backends this build and this kernel can actually provide, in sweep
// order. Always contains "posix".
const std::vector<std::string>& transport_backends();

// True when batched syscalls exist on this build. False on macOS.
bool batched_io_available();
// True when io_uring is compiled in and the kernel accepts a ring.
bool uring_available(std::string* why);
// True when io_uring accepts IORING_SETUP_SQPOLL for this process.
bool uring_sqpoll_available(std::string* why);

} // namespace lr
