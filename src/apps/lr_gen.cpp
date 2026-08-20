// lr_gen.cpp — the load generator, as a one-shot process.
//
// Two modes, and using one for the other's question is the classic error.
//
//   closed  the generator waits for the sink to consume what it sent before
//           sending more. Measures capacity. Says nothing about latency,
//           because its own back-pressure suppresses the queueing that latency
//           under load is about.
//   open    the generator sends at a fixed rate whatever the sink does.
//           Measures latency under load. Says nothing about capacity, because
//           past the knee it is measuring the queue, not the service rate.
//
// The sweep uses lr_loadgend rather than this, because a sweep wants one
// long-lived generator it can reconfigure per unit. This is for driving a sink
// by hand while bringing a two-node lab up.
#include "lr/clock.hpp"
#include "lr/platform.hpp"
#include "lr/proto.hpp"
#include "lr/worker.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
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
        printf("lr_gen --host=IP --port=N --cipher=NAME --payload=N "
               "--mode=open|closed --pps=N --seconds=N --cpu=N\n");
        return 0;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    const std::string host = opt(argc, argv, "host", "127.0.0.1");
    const uint16_t    port = (uint16_t)atoi(opt(argc, argv, "port", "0").c_str());
    const std::string mode = opt(argc, argv, "mode", "open");
    const double      pps  = atof(opt(argc, argv, "pps", "100000").c_str());
    const double  seconds  = atof(opt(argc, argv, "seconds", "5").c_str());
    const int         cpu  = atoi(opt(argc, argv, "cpu", "-1").c_str());

    if (port == 0) { fprintf(stderr, "lr_gen: --port is required\n"); return 64; }
    if (cpu >= 0) plat::pin_to_cpu(cpu);

    ShardConfig cfg;
    cfg.cipher  = opt(argc, argv, "cipher", "aes-256-gcm");
    cfg.payload = (size_t)atol(opt(argc, argv, "payload", "512").c_str());
    cfg.source  = Source::Loopback;   // used only for its sealing

    std::string err;
    Shard proto_shard(cfg, proto::bench_psk(), &err);
    if (!err.empty()) { fprintf(stderr, "lr_gen: %s\n", err.c_str()); return 1; }

    Generator gen(proto_shard, host, port, &err);
    if (!err.empty()) { fprintf(stderr, "lr_gen: %s\n", err.c_str()); return 1; }

    const uint64_t interval = (uint64_t)((double)clk::tick_hz() / (pps > 0 ? pps : 1));
    const uint64_t gap      = clk::tick_hz() / 100000;
    const uint64_t deadline = clk::ticks() + clk::ns_to_ticks(seconds * 1e9);
    uint64_t next = clk::ticks();
    uint64_t seq = 0;

    while (!g_stop && clk::ticks() < deadline) {
        if (mode == "open") {
            next += interval;
            for (;;) {
                const uint64_t now = clk::ticks();
                if (now >= next) break;
                if (next - now > gap) std::this_thread::yield();
            }
        }
        gen.emit(seq++);
    }
    printf("lr_gen sent %llu, refused %llu, in %s mode at an offered %.0f pps\n",
           (unsigned long long)gen.sent(), (unsigned long long)gen.refused(),
           mode.c_str(), pps);
    return 0;
}
