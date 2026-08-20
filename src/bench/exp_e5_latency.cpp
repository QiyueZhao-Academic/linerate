// exp_e5_latency.cpp — what the cost model implies for delay, and where it stops
// implying anything.
//
// E1 measures service cost. A service cost implies a capacity, and a capacity
// implies a knee: below it, delay is the service time; above it, delay is the
// queue and grows without bound until something drops. This experiment sweeps
// the offered rate across that knee and reports the tail.
//
// The load must be open-loop. A closed-loop generator waits for the previous
// packet to be served before sending the next, so it cannot offer more than the
// receiver's capacity and the queueing it is supposed to expose never forms.
// Reporting a closed-loop latency curve as a latency-under-load result is a
// standard error and it always produces a flatteringly flat tail.
//
// Two topologies, and which one produced a number is recorded with it.
//
//   wire       the generator runs on the other VM and sends across the physical
//              interface. The data plane forwards each packet back, and the
//              generator reads the origin tick out of the header — which is
//              carried in the clear as associated data, so no decryption is
//              needed to read it. Both timestamps come from the generator's own
//              counter, so no clock synchronisation between hosts is assumed.
//              What this yields is a round trip, not a one-way delay, and the
//              report says so; the no-crypto echo baseline measured the same
//              way is what makes the difference interpretable.
//
//   loopback   no peer available. The generator is a thread in this process and
//              the packets never leave the host. Latency is then a lower bound
//              that omits the driver, the wire and the far-side stack entirely.
//
// Network conditions are injected outside this program, by cloud/netem.sh, which
// attaches a tc qdisc to the egress interface. The label of the active condition
// arrives in LR_NETEM_LABEL and is recorded with every point, so a curve
// measured under 5 ms of added delay cannot be confused with a clean one.
#include "bench.hpp"

#include "lr/clock.hpp"
#include "lr/platform.hpp"
#include "lr/proto.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <thread>

namespace lr::bench {

namespace {

struct Row {
    double offered_pps = 0;
    double achieved_pps = 0;
    double p50_ns = 0, p99_ns = 0, p999_ns = 0;
    double loss_ratio = 0;
    uint64_t packets = 0;
    std::string source;
    std::string netem;
    bool ok = false;
    std::string error;
};

// Loopback topology: a generator thread on this host feeds the shard.
Row measure_loopback(const RunCtx& ctx, const std::string& cipher, size_t payload,
                     double offered_pps, double seconds) {
    Row r;
    r.offered_pps = offered_pps;
    r.source = "loopback";

    ShardConfig cfg;
    cfg.cipher  = cipher;
    cfg.payload = payload;
    cfg.latency = true;
    cfg.cpu     = plat::caps().hard_affinity ? 0 : -1;
    std::string err;
    Shard sh(cfg, bench_key(), &err);
    if (!err.empty()) { r.error = err; return r; }

    Generator gen(sh, "127.0.0.1", sh.local_port(), &err);
    if (!err.empty()) { r.error = err; return r; }

    std::atomic<bool> stop{false};
    std::thread tx([&] {
        if (plat::caps().hard_affinity && plat::caps().physical_cores > 1)
            plat::pin_to_cpu(1);
        const uint64_t interval = (uint64_t)((double)clk::tick_hz() / offered_pps);
        const uint64_t gap = clk::tick_hz() / 100000;
        uint64_t next = clk::ticks();
        uint64_t seq = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            next += interval;
            for (;;) {
                const uint64_t now = clk::ticks();
                if (now >= next) break;
                if (next - now > gap) std::this_thread::yield();
            }
            gen.emit(seq++);
        }
    });

    // Settle, then reset: the first packets pay for cold caches and a cold
    // socket, and charging that to the tail would put a spike at every rate.
    const uint64_t settle = clk::ticks() + clk::ns_to_ticks(200e6);
    while (clk::ticks() < settle) sh.drain(1024);
    sh.reset_stats();

    const uint64_t deadline = clk::ticks() + clk::ns_to_ticks(seconds * 1e9);
    while (clk::ticks() < deadline) sh.drain(1024);
    stop.store(true);
    tx.join();

    const ShardStats& st = sh.stats();
    if (st.packets == 0) { r.error = "no packets processed"; return r; }
    r.packets      = st.packets;
    r.achieved_pps = (double)st.packets / seconds;
    r.p50_ns       = st.lat.quantile(0.50);
    r.p99_ns       = st.lat.quantile(0.99);
    r.p999_ns      = st.lat.quantile(0.999);
    const double sent = gen.sent() > 0 ? (double)gen.sent() : 1.0;
    r.loss_ratio   = std::max(0.0, 1.0 - (double)st.packets / sent);
    r.ok = true;
    return r;
}

// Wire topology: the generator is on the other VM and measures the round trip
// itself, because only it holds both ends of the interval on one clock.
Row measure_wire(const RunCtx& ctx, Peer& peer, const std::string& cipher,
                 size_t payload, double offered_pps, double seconds,
                 uint16_t return_port) {
    Row r;
    r.offered_pps = offered_pps;
    r.source = "wire";

    ShardConfig cfg;
    cfg.cipher    = cipher;
    cfg.payload   = payload;
    cfg.latency   = false;      // the far side owns the latency measurement here
    cfg.source    = Source::Wire;
    cfg.bind_addr = ctx.local_addr.empty() ? "0.0.0.0" : ctx.local_addr;
    cfg.peer_addr = peer.remote_addr();
    cfg.peer_port = return_port;
    cfg.cpu       = plat::caps().hard_affinity ? 0 : -1;

    std::string err;
    Shard sh(cfg, bench_key(), &err);
    if (!err.empty()) { r.error = err; return r; }

    ctrl::StartRequest q;
    q.cipher      = cipher;
    q.payload     = payload;
    q.pps         = offered_pps;
    q.seconds     = seconds;
    q.dst_addr    = ctx.local_addr;
    q.dst_port    = sh.local_port();
    q.return_port = return_port;
    if (!peer.start(q, &err)) { r.error = err; return r; }

    const uint64_t settle = clk::ticks() + clk::ns_to_ticks(200e6);
    while (clk::ticks() < settle) sh.drain(2048);
    sh.reset_stats();

    const uint64_t deadline = clk::ticks() + clk::ns_to_ticks(seconds * 1e9);
    while (clk::ticks() < deadline) sh.drain(2048);

    ctrl::StopReply rep;
    if (!peer.stop(&rep, &err)) { r.error = err; return r; }

    const ShardStats& st = sh.stats();
    r.packets      = st.packets;
    r.achieved_pps = (double)st.packets / seconds;
    r.p50_ns       = rep.rtt_p50_ns;
    r.p99_ns       = rep.rtt_p99_ns;
    r.p999_ns      = rep.rtt_p999_ns;
    r.loss_ratio   = rep.sent > 0
        ? std::max(0.0, 1.0 - (double)rep.returned / (double)rep.sent) : 0.0;
    r.ok = st.packets > 0;
    if (!r.ok) {
        r.error = rep.fault.empty()
            ? "no packets arrived over the wire"
            : "the load generator could not run this unit: " + rep.fault;
    }
    return r;
}

} // namespace

int run_e5(const RunCtx& ctx, int argc, char** argv) {
    printf("E5 latency under load\n");
    std::string why;
    if (!measurement_permitted(&why))
        return write_unavailable(ctx, "e5_latency.json", "e5", why);

    const bool quick = arg_has(argc, argv, "quick");
    const std::string cipher = arg_str(argc, argv, "e5-cipher", "aes-256-gcm");
    const size_t payload = (size_t)arg_int(argc, argv, "e5-payload", 512);
    const double seconds = quick ? 0.4 : std::max(1.0, ctx.unit_seconds);
    const char* netem_env = getenv("LR_NETEM_LABEL");
    const std::string netem = netem_env ? netem_env : "none";

    // Capacity comes from a short measurement of the same configuration, not
    // from a constant: the rate grid has to straddle the knee on whatever
    // machine this is, and a hard-coded grid would miss it on a faster or
    // slower host.
    ShardConfig probe;
    probe.cipher = cipher;
    probe.payload = payload;
    HotPathResult cap = run_hotpath(probe, quick ? 4000 : 20000, ctx.warmup_cycles);
    if (!cap.ok)
        return write_unavailable(ctx, "e5_latency.json", "e5",
                                 "capacity probe failed: " + cap.error);
    const double capacity = cap.pps;
    printf("  capacity probe: %.0f pps at %s / %zu B\n", capacity, cipher.c_str(), payload);

    std::vector<double> fractions = quick
        ? std::vector<double>{0.3, 0.7, 0.95, 1.2}
        : std::vector<double>{0.1, 0.25, 0.4, 0.55, 0.7, 0.8, 0.9, 0.95, 1.0, 1.1, 1.3, 1.6};

    Peer peer(ctx);
    const bool wire = peer.enabled();
    printf("  topology: %s%s, netem condition '%s'\n",
           wire ? "wire" : "loopback",
           wire ? "" : (" (" + peer.why() + ")").c_str(), netem.c_str());

    std::vector<Row> rows;
    const uint16_t return_port = 9097;
    for (double f : fractions) {
        const double offered = capacity * f;
        Row r = wire ? measure_wire(ctx, peer, cipher, payload, offered, seconds, return_port)
                     : measure_loopback(ctx, cipher, payload, offered, seconds);
        r.netem = netem;
        rows.push_back(r);
        printf("    %6.0f%% of capacity  offered %9.0f  achieved %9.0f  "
               "p50 %8.0f  p99 %9.0f  p99.9 %10.0f ns\n",
               f * 100, r.offered_pps, r.achieved_pps, r.p50_ns, r.p99_ns, r.p999_ns);
    }

    // The knee: the highest offered rate whose p99 is still within twice the
    // lowest p99 observed. Defined from the data rather than asserted, so a
    // machine with a different queue discipline gets its own answer.
    double knee_pps = 0, base_p99 = 0;
    for (const Row& r : rows)
        if (r.ok && (base_p99 == 0 || r.p99_ns < base_p99)) base_p99 = r.p99_ns;
    for (const Row& r : rows)
        if (r.ok && base_p99 > 0 && r.p99_ns <= 2.0 * base_p99)
            knee_pps = std::max(knee_pps, r.offered_pps);

    ::mkdir(ctx.outdir.c_str(), 0755);
    json::Writer w;
    w.obj_open();
      write_environment(w, ctx);
      w.key("e5").obj_open();
        w.kv("available", true);
        w.kv("quick", quick);
        w.kv("cipher", cipher);
        w.kv("payload_bytes", (long long)payload);
        w.kv("seconds_per_point", seconds, 3);
        w.kv("capacity_pps", capacity, 1);
        w.kv("knee_pps", knee_pps, 1);
        w.kv("knee_fraction", capacity > 0 ? knee_pps / capacity : 0.0, 4);
        w.kv("baseline_p99_ns", base_p99, 1);
        w.kv("topology", wire ? "wire" : "loopback");
        w.kv("topology_reason", wire ? std::string("peer generator on ") + peer.remote_addr()
                                     : peer.why());
        w.kv("netem_label", netem);
        // An open-loop generator on the same core as the data plane is not a
        // load source, it is a competitor for the CPU. The queue that forms is
        // then a scheduling artefact and the tail says nothing about the
        // service discipline. The condition is recorded so the analysis can
        // refuse to draw a knee from such a run rather than drawing a wrong one.
        w.kv("generator_shares_cpu", !wire && plat::caps().physical_cores < 2);
        w.kv("interpretable",
             wire || plat::caps().physical_cores >= 2);
        w.kv("interpretation_note",
             (!wire && plat::caps().physical_cores < 2)
               ? "loopback fallback on a single-core host: the generator and the "
                 "data plane share one core, so the delays reported here are "
                 "scheduling latency, not queueing delay, and no knee should be "
                 "read from them"
               : (wire ? "generator on a separate host across the measured link"
                       : "generator on a separate core of this host"));
        w.kv("latency_metric", wire ? "round trip, generator clock, both ends"
                                    : "one way, local clock, header timestamp");
        w.key("points").arr_open();
        for (const Row& r : rows) {
            w.obj_open();
              w.kv("offered_pps", r.offered_pps, 1);
              w.kv("offered_fraction", capacity > 0 ? r.offered_pps / capacity : 0.0, 4);
              w.kv("achieved_pps", r.achieved_pps, 1);
              w.kv("p50_ns", r.p50_ns, 1);
              w.kv("p99_ns", r.p99_ns, 1);
              w.kv("p999_ns", r.p999_ns, 1);
              w.kv("loss_ratio", r.loss_ratio, 5);
              w.kv("packets", (long long)r.packets);
              w.kv("source", r.source);
              w.kv("netem_label", r.netem);
              w.kv("ok", r.ok);
              w.kv("error", r.error);
            w.obj_close();
        }
        w.arr_close();
      w.obj_close();
    w.obj_close();

    const std::string path = ctx.outdir + "/e5_latency.json";
    if (!w.write_file(path)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    printf("  wrote %s\n", path.c_str());
    return 0;
}

} // namespace lr::bench
