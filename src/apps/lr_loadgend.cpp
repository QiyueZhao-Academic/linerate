// lr_loadgend.cpp — the load generator, as a daemon on the far node.
//
// The measuring node drives this over a single TCP control connection for a
// whole sweep. Driving each of the three hundred-odd units over ssh instead
// would spend more time on ssh than on measurement, and would put the operator's
// laptop inside the measurement loop.
//
// Two threads while a unit is running:
//
//   sender    seals packets and transmits them to the data-plane node, either
//             paced against the invariant counter or unpaced. Unpaced is what
//             the cost-model sweep wants: the receive queue on the far side
//             must never empty, or the measurement times an idle poll rather
//             than the service of a packet.
//
//   returner  receives the packets the data plane forwards back and reads the
//             origin tick out of the header. Both timestamps are taken on this
//             machine's own counter, so the round trip is measured without any
//             clock synchronisation between the two hosts. The one-way sojourn
//             is not directly observable across two machines and the report
//             does not pretend otherwise; what is reported is the round trip,
//             against a no-crypto echo baseline measured the same way.
#include "lr/clock.hpp"
#include "lr/ctrl.hpp"
#include "lr/platform.hpp"
#include "lr/proto.hpp"
#include "lr/stats.hpp"
#include "lr/worker.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace lr;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// signal(2) installs a handler with SA_RESTART on Linux, so a SIGTERM arriving
// while this process sits in accept() sets the flag and then the kernel
// restarts the call: the flag is never read again and the daemon does not stop.
// Everything that stops it — systemctl, pkill, the driver's own teardown —
// then had to escalate to SIGKILL, and a daemon killed rather than stopped
// leaves its sockets in the kernel for the next one to fail to bind. Installing
// without SA_RESTART makes accept() return EINTR, which is what the loop below
// is written to expect.
bool install_handler(int sig) {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                  // deliberately not SA_RESTART
    return ::sigaction(sig, &sa, nullptr) == 0;
}

std::string opt(int argc, char** argv, const char* name, const char* def) {
    std::string pre = std::string("--") + name + "=";
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], pre.c_str(), pre.size()) == 0) return argv[i] + pre.size();
    return def;
}

// One running unit.
//
// `fault` is what the sender thread writes when it cannot start. Without it a
// cipher name the factory does not know, or a destination that cannot be
// reached, produced a unit that ran to its end and reported zero packets sent —
// a number the sweep would have gone on to treat as a measurement. A reason
// carried back over the control channel turns that into an error the driver can
// print. It is a single whitespace-free token because parse_kv splits on
// whitespace; both ends share that parser precisely so they cannot disagree.
struct Unit {
    std::atomic<bool> stop{false};
    std::thread send_th, ret_th;
    std::atomic<uint64_t> sent{0}, refused{0}, returned{0};
    stats::LatencyHist rtt;
    int return_fd = -1;
    bool active = false;
    std::mutex fault_mu;
    std::string fault;
};

// Spaces and newlines would split one field into several on the far side.
std::string one_token(const std::string& s) {
    std::string out;
    for (char c : s) out += (c == ' ' || c == '\t' || c == '\n' || c == '\r') ? '_' : c;
    if (out.empty()) out = "unknown";
    if (out.size() > 160) out.resize(160);
    return out;
}

int open_return_socket(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1, rb = 16 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(fd, (sockaddr*)&a, sizeof(a)) != 0) { ::close(fd); return -1; }
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

bool start_unit(Unit& u, const ctrl::StartRequest& q, std::string* err) {
    if (u.active) { *err = "already running"; return false; }
    u.stop.store(false);
    u.sent.store(0); u.refused.store(0); u.returned.store(0);
    u.rtt = stats::LatencyHist();
    { std::lock_guard<std::mutex> lk(u.fault_mu); u.fault.clear(); }

    if (q.return_port) {
        u.return_fd = open_return_socket(q.return_port);
        if (u.return_fd < 0) { *err = "cannot bind return port"; return false; }
    }

    const std::string cipher = q.cipher;
    const size_t payload = q.payload;
    const double pps = q.pps;
    const std::string dst = q.dst_addr;
    const uint16_t dport = q.dst_port;
    const uint64_t epoch = q.epoch_packets;
    const int cpu = q.cpu;

    u.send_th = std::thread([&u, cipher, payload, pps, dst, dport, epoch, cpu] {
        if (cpu >= 0) plat::pin_to_cpu(cpu);
        ShardConfig cfg;
        cfg.cipher  = cipher;
        cfg.payload = payload;
        cfg.epoch_packets = epoch;
        cfg.source  = Source::Loopback;   // used only for its sealing
        std::string e;
        auto fail = [&u](const std::string& why) {
            std::lock_guard<std::mutex> lk(u.fault_mu);
            if (u.fault.empty()) u.fault = one_token(why);
        };
        Shard proto(cfg, proto::bench_psk(), &e);
        if (!e.empty()) { fail("shard:" + e); return; }
        Generator gen(proto, dst, dport, &e);
        if (!e.empty()) { fail("generator:" + e); return; }

        uint64_t seq = 0;
        if (pps > 0) {
            const uint64_t interval = (uint64_t)((double)clk::tick_hz() / pps);
            const uint64_t gap = clk::tick_hz() / 100000;   // 10 us
            uint64_t next = clk::ticks();
            while (!u.stop.load(std::memory_order_relaxed)) {
                next += interval;
                for (;;) {
                    const uint64_t now = clk::ticks();
                    if (now >= next) break;
                    // Pace on the counter, not on a sleep: a sleep would
                    // quantise the schedule to the timer tick and turn a smooth
                    // offered rate into a burst train.
                    if (next - now > gap) std::this_thread::yield();
                }
                if (gen.emit(seq++)) u.sent.fetch_add(1, std::memory_order_relaxed);
                else                 u.refused.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            while (!u.stop.load(std::memory_order_relaxed)) {
                if (gen.emit(seq++)) u.sent.fetch_add(1, std::memory_order_relaxed);
                else {
                    u.refused.fetch_add(1, std::memory_order_relaxed);
                    // A full transmit buffer is normal when unpaced; back off
                    // for a moment rather than spinning on EAGAIN.
                    std::this_thread::yield();
                }
            }
        }
    });

    if (u.return_fd >= 0) {
        u.ret_th = std::thread([&u] {
            std::vector<uint8_t> buf(2048);
            while (!u.stop.load(std::memory_order_relaxed)) {
                ssize_t r = ::recv(u.return_fd, buf.data(), buf.size(), 0);
                if (r < (ssize_t)LR_HDR) { std::this_thread::yield(); continue; }
                WireHdr h{};
                std::memcpy(&h, buf.data(), LR_HDR);
                if (h.magic != LR_MAGIC) continue;
                // The header travels in the clear as associated data, so the
                // origin tick is readable without a decryption. The data plane
                // preserves it across the re-encryption.
                const uint64_t now = clk::ticks();
                if (now > h.tsc) u.rtt.add(clk::ticks_to_ns(now - h.tsc));
                u.returned.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    u.active = true;
    return true;
}

void stop_unit(Unit& u, ctrl::StopReply* rep) {
    if (!u.active) { rep->ok = true; return; }
    u.stop.store(true);
    if (u.send_th.joinable()) u.send_th.join();
    if (u.ret_th.joinable())  u.ret_th.join();
    if (u.return_fd >= 0) { ::close(u.return_fd); u.return_fd = -1; }
    rep->sent        = u.sent.load();
    rep->refused     = u.refused.load();
    rep->returned    = u.returned.load();
    rep->rtt_p50_ns  = u.rtt.quantile(0.50);
    rep->rtt_p99_ns  = u.rtt.quantile(0.99);
    rep->rtt_p999_ns = u.rtt.quantile(0.999);
    { std::lock_guard<std::mutex> lk(u.fault_mu); rep->fault = u.fault; }
    rep->ok = true;
    u.active = false;
}

// A plain UDP echo, no crypto, for the baseline the round-trip numbers are
// measured against. It runs for the daemon's whole lifetime on its own port.
//
// A failure here used to be silent: the thread returned, the daemon carried on,
// and E5's no-crypto reference came back empty for a reason nothing recorded.
// It is reported and flagged instead. It is still not fatal — the cost model
// does not need the echo port, and losing one baseline is a smaller loss than
// losing the sweep.
// `state` is tri-state rather than a flag because the readiness line has to
// distinguish "not bound yet" from "will never bind". A plain bool read by the
// main thread reports every echo server as unavailable whenever it is read
// before the thread has got as far as bind(), which is most of the time.
enum EchoState { kEchoPending = 0, kEchoReady = 1, kEchoFailed = 2 };

void echo_server(uint16_t port, std::atomic<bool>* stop, std::atomic<int>* state) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "lr_loadgend: echo socket: %s\n", strerror(errno));
        state->store(kEchoFailed);
        return;
    }
    int one = 1, rb = 8 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(fd, (sockaddr*)&a, sizeof(a)) != 0) {
        fprintf(stderr, "lr_loadgend: echo bind %u: %s (the no-crypto round-trip "
                        "baseline will be unavailable)\n", port, strerror(errno));
        ::close(fd);
        state->store(kEchoFailed);
        return;
    }
    state->store(kEchoReady);
    timeval tv{0, 200000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::vector<uint8_t> buf(2048);
    while (!stop->load(std::memory_order_relaxed)) {
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        ssize_t r = ::recvfrom(fd, buf.data(), buf.size(), 0, (sockaddr*)&from, &fl);
        if (r <= 0) continue;
        ::sendto(fd, buf.data(), (size_t)r, 0, (sockaddr*)&from, fl);
    }
    ::close(fd);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--help") == 0) {
        printf("lr_loadgend --port=N --echo-port=N\n"
               "  Runs on the load-generating node. The measuring node connects\n"
               "  once and drives the whole sweep over the control channel.\n");
        return 0;
    }
    install_handler(SIGINT);
    install_handler(SIGTERM);
    signal(SIGPIPE, SIG_IGN);

    const uint16_t port = (uint16_t)atoi(opt(argc, argv, "port",
                                             std::to_string(ctrl::kDefaultPort).c_str()).c_str());
    const uint16_t echo_port = (uint16_t)atoi(opt(argc, argv, "echo-port", "9098").c_str());

    std::atomic<bool> echo_stop{false};
    std::atomic<int>  echo_state{kEchoPending};
    std::thread echo(echo_server, echo_port, &echo_stop, &echo_state);

    int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        fprintf(stderr, "lr_loadgend: socket: %s\n", strerror(errno));
        echo_stop.store(true);
        if (echo.joinable()) echo.join();
        return 1;
    }
    int one = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(ls, (sockaddr*)&a, sizeof(a)) != 0) {
        fprintf(stderr, "lr_loadgend: bind %u: %s\n", port, strerror(errno));
        echo_stop.store(true);
        if (echo.joinable()) echo.join();
        ::close(ls);
        return 1;
    }
    // A signal is delivered to whichever thread of the process has it unblocked,
    // which is not necessarily this one. Interrupting accept() is therefore not
    // something a handler can be relied on to do, and a daemon whose shutdown
    // depends on that is one that stops promptly on a machine where the main
    // thread happened to be chosen and hangs on a machine where it was not. A
    // receive timeout on the listening socket makes accept() return by itself
    // several times a second, so g_stop is read regardless of who was signalled.
    timeval atv{0, 250000};
    ::setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &atv, sizeof(atv));

    // An unchecked listen() is how a daemon comes to hold a bound socket that
    // accepts nothing: the process exists, so everything that looks for a
    // process is satisfied, and every connection to it is refused.
    if (::listen(ls, 4) != 0) {
        fprintf(stderr, "lr_loadgend: listen %u: %s\n", port, strerror(errno));
        echo_stop.store(true);
        if (echo.joinable()) echo.join();
        ::close(ls);
        return 1;
    }

    std::string nic, addr, drv;
    if (!plat::primary_interface(&nic, &addr, &drv)) {
        nic = "unknown"; addr = "unknown"; drv = "unknown";
    }
    // Give the echo thread a bounded moment to settle, so that the line below
    // reports what happened rather than what had happened by the time the main
    // thread got here. Two seconds is far longer than a bind takes and far
    // shorter than anything waiting on this daemon will notice.
    for (int i = 0; i < 200 && echo_state.load() == kEchoPending; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // One line, on stderr, which is unbuffered: whatever starts this daemon
    // redirects both its streams into a log, and a readiness line that is still
    // sitting in a buffer when the process is killed is a log that says nothing
    // happened. The word `ready` is what an operator greps for.
    fprintf(stderr, "lr_loadgend: ready pid=%ld control=:%u echo=:%u%s "
                    "interface=%s driver=%s addr=%s\n",
            (long)::getpid(), port, echo_port,
            echo_state.load() == kEchoReady ? "" : " (echo unavailable)",
            nic.c_str(), drv.c_str(), addr.c_str());

    Unit unit;
    while (!g_stop) {
        sockaddr_in ca{};
        socklen_t cl = sizeof(ca);
        int c = ::accept(ls, (sockaddr*)&ca, &cl);
        if (c < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // timeout
            if (errno == EINTR) continue;       // g_stop decides, not the signal
            if (errno == ECONNABORTED || errno == EMFILE || errno == ENFILE) {
                // Transient. Sleeping rather than retrying at once, because a
                // descriptor limit reached in a tight loop is a spin that keeps
                // the machine too busy to release the descriptors.
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            fprintf(stderr, "lr_loadgend: accept: %s\n", strerror(errno));
            break;
        }
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        // Same reasoning as the listening socket: the driver holds this
        // connection open for a whole sweep and is silent between units, so a
        // blocking read here would outlive a stop request by however long the
        // next unit takes to arrive.
        timeval ctv{0, 250000};
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
        fprintf(stderr, "lr_loadgend: control connection from %s\n", inet_ntoa(ca.sin_addr));

        std::string line;
        char ch;
        bool alive = true;
        while (alive && !g_stop) {
            ssize_t r = ::recv(c, &ch, 1, 0);
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            if (ch != '\n') { line += ch; if (line.size() < 8192) continue; }
            std::string reply;
            if (line.rfind("PING", 0) == 0) {
                char b[512];
                std::snprintf(b, sizeof(b),
                    "PONG version=2 arch=%s nic=%s driver=%s addr=%s echo_port=%u tick_hz=%llu",
                    plat::caps().arch.c_str(), nic.c_str(), drv.c_str(), addr.c_str(),
                    (unsigned)echo_port, (unsigned long long)clk::tick_hz());
                reply = b;
            } else if (line.rfind("START", 0) == 0) {
                auto kv = ctrl::parse_kv(line.substr(5));
                ctrl::StartRequest q;
                auto get = [&](const char* k, const char* d) {
                    auto it = kv.find(k); return it == kv.end() ? std::string(d) : it->second;
                };
                q.cipher  = get("cipher", "aes-256-gcm");
                q.payload = (size_t)atol(get("payload", "512").c_str());
                q.pps     = atof(get("pps", "0").c_str());
                q.seconds = atof(get("seconds", "2").c_str());
                q.return_port   = (uint16_t)atoi(get("return_port", "0").c_str());
                q.epoch_packets = strtoull(get("epoch_packets", "0").c_str(), nullptr, 10);
                q.cpu     = atoi(get("cpu", "-1").c_str());
                std::string d = get("dst", "");
                auto colon = d.rfind(':');
                if (colon != std::string::npos) {
                    q.dst_addr = d.substr(0, colon);
                    q.dst_port = (uint16_t)atoi(d.c_str() + colon + 1);
                }
                std::string err;
                reply = start_unit(unit, q, &err) ? "OK" : ("ERR " + err);
            } else if (line.rfind("STOP", 0) == 0) {
                ctrl::StopReply rep;
                stop_unit(unit, &rep);
                char b[512];
                std::snprintf(b, sizeof(b),
                    "DONE sent=%llu refused=%llu returned=%llu "
                    "rtt_p50_ns=%.0f rtt_p99_ns=%.0f rtt_p999_ns=%.0f fault=%s",
                    (unsigned long long)rep.sent, (unsigned long long)rep.refused,
                    (unsigned long long)rep.returned,
                    rep.rtt_p50_ns, rep.rtt_p99_ns, rep.rtt_p999_ns,
                    rep.fault.c_str());
                reply = b;
            } else if (line.rfind("QUIT", 0) == 0) {
                reply = "BYE";
                alive = false;
            } else {
                reply = "ERR unknown command";
            }
            reply += "\n";
            ::send(c, reply.data(), reply.size(), 0);
            line.clear();
        }
        // A dropped control connection must not leave a generator blasting at
        // the far node for the rest of the day.
        if (unit.active) { ctrl::StopReply rep; stop_unit(unit, &rep); }
        ::close(c);
    }
    echo_stop.store(true);
    if (echo.joinable()) echo.join();
    ::close(ls);
    return 0;
}
