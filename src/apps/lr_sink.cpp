// lr_sink.cpp — the data plane as a standalone process.
//
// The bench binary embeds the same shard, so this exists for the cases where an
// experiment needs the data plane to be a *process* rather than a function
// call: chained through a service function chain, isolated in a namespace, or
// wrapped in a container whose overhead is the thing being measured.
#include "lr/clock.hpp"
#include "lr/platform.hpp"
#include "lr/proto.hpp"
#include "lr/worker.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

using namespace lr;

namespace {
volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::string opt(int argc, char** argv, const char* name, const char* def) {
    std::string pre = std::string("--") + name + "=";
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], pre.c_str(), pre.size()) == 0) return argv[i] + pre.size();
    return def;
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--help") == 0) {
        printf("lr_sink --bind=IP --port=N --peer=IP --peer-port=N --cipher=NAME\n"
               "        --payload=N --backend=posix|mmsg|uring|uring-sqpoll --batch=N\n"
               "        --cpu=N --seconds=N --epoch-packets=N --no-replay\n");
        return 0;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    ShardConfig cfg;
    cfg.cipher   = opt(argc, argv, "cipher", "aes-256-gcm");
    cfg.backend  = opt(argc, argv, "backend", "posix");
    cfg.batch    = atoi(opt(argc, argv, "batch", "1").c_str());
    cfg.payload  = (size_t)atol(opt(argc, argv, "payload", "512").c_str());
    cfg.cpu      = atoi(opt(argc, argv, "cpu", "-1").c_str());
    cfg.epoch_packets = strtoull(opt(argc, argv, "epoch-packets", "0").c_str(), nullptr, 10);
    cfg.source   = Source::Wire;
    cfg.bind_addr= opt(argc, argv, "bind", "0.0.0.0");
    cfg.bind_port= (uint16_t)atoi(opt(argc, argv, "port", "0").c_str());
    cfg.peer_addr= opt(argc, argv, "peer", "127.0.0.1");
    cfg.peer_port= (uint16_t)atoi(opt(argc, argv, "peer-port", "9").c_str());
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--no-replay") == 0) cfg.replay = false;
    const double seconds = atof(opt(argc, argv, "seconds", "0").c_str());

    std::string err;
    Shard sh(cfg, proto::bench_psk(), &err);
    if (!err.empty()) { fprintf(stderr, "lr_sink: %s\n", err.c_str()); return 1; }

    printf("lr_sink listening on %s:%u -> %s:%u  cipher=%s(%s) backend=%s batch=%d payload=%zu\n",
           cfg.bind_addr.c_str(), (unsigned)sh.local_port(),
           cfg.peer_addr.c_str(), (unsigned)cfg.peer_port,
           cfg.cipher.c_str(), sh.aead_backend(), cfg.backend.c_str(),
           cfg.batch, cfg.payload);
    fflush(stdout);

    const uint64_t deadline = seconds > 0
        ? clk::ticks() + clk::ns_to_ticks(seconds * 1e9) : 0;
    uint64_t last_report = clk::ticks();
    uint64_t last_pkts = 0;

    while (!g_stop) {
        sh.drain(4096);
        const uint64_t now = clk::ticks();
        if (deadline && now >= deadline) break;
        if (now - last_report > clk::tick_hz()) {
            const ShardStats& s = sh.stats();
            const double secs = (double)(now - last_report) / (double)clk::tick_hz();
            printf("  %8.0f pps   packets=%llu auth_fail=%llu replay_drop=%llu rekeys=%llu\n",
                   (double)(s.packets - last_pkts) / secs,
                   (unsigned long long)s.packets, (unsigned long long)s.auth_fail,
                   (unsigned long long)s.replay_drop, (unsigned long long)s.rekeys);
            fflush(stdout);
            last_report = now;
            last_pkts = s.packets;
        }
    }
    const ShardStats& s = sh.stats();
    printf("lr_sink total packets=%llu bytes=%llu auth_fail=%llu replay_drop=%llu\n",
           (unsigned long long)s.packets, (unsigned long long)s.bytes,
           (unsigned long long)s.auth_fail, (unsigned long long)s.replay_drop);
    return 0;
}
