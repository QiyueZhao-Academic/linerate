// exp_e2_scaling.cpp — how the cost model moves when the data plane is widened.
//
// The shard model shares nothing mutable across threads: each shard owns its
// socket, its key schedule, its replay window and its arena. The scaling
// question is therefore not "does the lock contend" but "what does the machine
// do when N copies of an identical, independent pipeline run at once" — memory
// bandwidth, last-level cache, and on a hyper-threaded host the sharing of one
// physical core's execution ports.
//
// Two scaling laws are reported, because they answer different questions and
// quoting one for the other is a standard way to overstate a result.
//
//   strong  the total packet count is fixed and divided among N shards. Speedup
//           is T(1)/T(N). This is the throughput question: given this much
//           traffic, does adding a core help?
//   weak    each shard processes the same count whatever N is. Efficiency is
//           T(1)/T(N) at N times the work. This is the capacity question: can a
//           machine absorb N times the traffic with N times the cores?
//
// The pool is expressed with OpenMP where the compiler provides it, and with
// std::thread otherwise, and which one ran is recorded. Both spawn one worker
// per shard and neither shares state, so the two are interchangeable here; what
// OpenMP buys is that the same source also expresses the parallel region for a
// reader who expects the standard idiom.
#include "bench.hpp"

#include "lr/clock.hpp"
#include "lr/platform.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <sys/stat.h>
#include <thread>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace lr::bench {

namespace {

struct ShardOutcome {
    double   cycles_per_packet = 0;
    uint64_t packets = 0;
    bool     ok = false;
    std::string error;
};

// Run `n` shards concurrently, each processing `per_shard` packets, and return
// the wall time of the parallel region plus each shard's own per-packet cost.
//
// Wall time is what a scaling law is about: the sum of per-shard costs would
// hide exactly the effect being looked for, since a shard slowed by contention
// still reports its own honest per-packet cost.
bool run_pool(const RunCtx& ctx, const std::string& cipher, size_t payload,
              int n, int per_shard, bool pin,
              double* wall_ns, std::vector<ShardOutcome>* out) {
    std::vector<ShardConfig> cfgs((size_t)n);
    for (int i = 0; i < n; ++i) {
        cfgs[i].cipher  = cipher;
        cfgs[i].payload = payload;
        cfgs[i].backend = "posix";
        cfgs[i].batch   = 1;
        cfgs[i].cpu     = pin ? i : -1;
    }
    out->assign((size_t)n, ShardOutcome{});

    // Construct every shard, and warm every shard, before the timed region
    // opens. Otherwise the first shard to be built would be timing the
    // construction of the last.
    std::vector<std::unique_ptr<Shard>> shards((size_t)n);
    for (int i = 0; i < n; ++i) {
        std::string err;
        shards[i] = std::make_unique<Shard>(cfgs[i], bench_key(), &err);
        if (!err.empty()) { (*out)[i].error = err; return false; }
        const int w = window_for(shards[i]->wire_len());
        for (int k = 0; k < ctx.warmup_cycles; ++k) {
            const int staged = shards[i]->preload(std::min(w, per_shard));
            if (staged > 0) shards[i]->drain(staged);
        }
        shards[i]->reset_stats();
    }

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    auto body = [&](int i) {
        if (pin) plat::pin_to_cpu(i);
        Shard& sh = *shards[(size_t)i];
        const int window = window_for(sh.wire_len());
        // A barrier before the timed region: without it, a shard that starts
        // early measures an uncontended machine and the scaling curve is
        // optimistic by however long thread creation took.
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();

        int done = 0, guard = 0;
        while (done < per_shard) {
            const int want = std::min(window, per_shard - done);
            const int staged = sh.preload(want);
            if (staged <= 0) { if (++guard > 8) break; std::this_thread::yield(); continue; }
            guard = 0;
            done += sh.drain(staged);
        }
        const ShardStats& st = sh.stats();
        ShardOutcome& o = (*out)[(size_t)i];
        o.packets = st.packets;
        o.cycles_per_packet = st.packets ? (double)st.ticks_busy / (double)st.packets : 0;
        o.ok = st.packets > 0 && st.auth_fail == 0;
    };

    uint64_t t0 = 0, t1 = 0;
#ifdef _OPENMP
    // The implicit barrier at the end of the `single` region is what holds the
    // other threads until the clock has been read, so the timed region begins
    // at the same instant for all of them. Thread creation happens before that
    // point and is therefore not charged to the measurement.
    #pragma omp parallel num_threads(n)
    {
        const int i = omp_get_thread_num();
        #pragma omp single
        { t0 = clk::ticks(); go.store(true, std::memory_order_release); }
        body(i);
    }
    t1 = clk::ticks();
#else
    std::vector<std::thread> th;
    th.reserve((size_t)n);
    for (int i = 0; i < n; ++i) th.emplace_back(body, i);
    while (ready.load() < n) std::this_thread::yield();
    t0 = clk::ticks();
    go.store(true, std::memory_order_release);
    for (auto& t : th) t.join();
    t1 = clk::ticks();
#endif
    *wall_ns = clk::ticks_to_ns(t1 - t0);
    return true;
}

double median_of(std::vector<double> v) { return stats::median(std::move(v)); }

} // namespace

int run_e2(const RunCtx& ctx, int argc, char** argv) {
    printf("E2 scaling\n");
    std::string why;
    if (!measurement_permitted(&why))
        return write_unavailable(ctx, "e2_scaling.json", "e2", why);

    const auto& caps = plat::caps();
    const int max_shards = std::max(1, caps.physical_cores);
    const size_t payload = (size_t)arg_int(argc, argv, "e2-payload", 512);
    const std::string cipher = arg_str(argc, argv, "e2-cipher", "aes-256-gcm");
    const bool quick = arg_has(argc, argv, "quick");
    const int reps = quick ? 3 : std::max(3, ctx.reps / 2);
    const int total_packets = quick ? 8000 : ctx.target_packets * 2;
    const bool pin = caps.hard_affinity;

    std::vector<int> widths;
    for (int n = 1; n <= max_shards; ++n) widths.push_back(n);

#ifdef _OPENMP
    const char* pool = "openmp";
#else
    const char* pool = "std::thread";
#endif

    printf("  %d physical cores, pool=%s, pinning=%s\n",
           max_shards, pool, pin ? "hard" : "none");

    struct Row {
        int n = 0;
        double strong_wall_ns = 0, weak_wall_ns = 0;
        double strong_cycles = 0, weak_cycles = 0;
        uint64_t strong_packets = 0, weak_packets = 0;
        bool ok = false;
        std::string error;
    };
    std::vector<Row> rows;

    for (int n : widths) {
        Row r;
        r.n = n;
        std::vector<double> sw, ww, sc, wc;

        for (int rep = 0; rep < reps; ++rep) {
            std::vector<ShardOutcome> out;
            double wall = 0;

            // Strong: the same total work, divided.
            const int per_strong = std::max(500, total_packets / n);
            if (!run_pool(ctx, cipher, payload, n, per_strong, pin, &wall, &out)) {
                r.error = out.empty() ? "pool failed" : out[0].error;
                break;
            }
            sw.push_back(wall);
            {
                double s = 0; uint64_t pk = 0;
                for (const auto& o : out) { s += o.cycles_per_packet; pk += o.packets; }
                sc.push_back(s / (double)n);
                r.strong_packets = pk;
            }

            // Weak: the same work each, so the total grows with n.
            const int per_weak = std::max(500, total_packets / std::max(1, max_shards));
            if (!run_pool(ctx, cipher, payload, n, per_weak, pin, &wall, &out)) {
                r.error = out.empty() ? "pool failed" : out[0].error;
                break;
            }
            ww.push_back(wall);
            {
                double s = 0; uint64_t pk = 0;
                for (const auto& o : out) { s += o.cycles_per_packet; pk += o.packets; }
                wc.push_back(s / (double)n);
                r.weak_packets = pk;
            }
        }
        if (!sw.empty()) {
            r.strong_wall_ns = median_of(sw);
            r.weak_wall_ns   = median_of(ww);
            r.strong_cycles  = median_of(sc);
            r.weak_cycles    = median_of(wc);
            r.ok = true;
        }
        rows.push_back(r);
        printf("    n=%d  strong %.2f ms  weak %.2f ms  %.1f cycles/pkt\n",
               n, r.strong_wall_ns / 1e6, r.weak_wall_ns / 1e6, r.strong_cycles);
    }

    // Speedup and efficiency against the single-shard row. Amdahl's serial
    // fraction is derived from the widest point that ran, and is reported as a
    // fitted quantity rather than an assumption.
    const double t1_strong = rows.empty() ? 0 : rows[0].strong_wall_ns;
    const double t1_weak   = rows.empty() ? 0 : rows[0].weak_wall_ns;
    double serial_fraction = 0;
    if (rows.size() > 1 && rows.back().ok && t1_strong > 0) {
        const double n = rows.back().n;
        const double sp = t1_strong / rows.back().strong_wall_ns;
        if (n > 1 && sp > 0) serial_fraction = (n / sp - 1.0) / (n - 1.0);
    }

    ::mkdir(ctx.outdir.c_str(), 0755);
    json::Writer w;
    w.obj_open();
      write_environment(w, ctx);
      w.key("e2").obj_open();
        w.kv("available", true);
        w.kv("quick", quick);
        w.kv("pool", pool);
        w.kv("pinned", pin);
        w.kv("cipher", cipher);
        w.kv("payload_bytes", (long long)payload);
        w.kv("max_shards", max_shards);
        w.kv("replicates", reps);
        w.kv("total_packets_strong", (long long)total_packets);
        w.kv("serial_fraction", serial_fraction, 5);
        w.key("points").arr_open();
        for (const Row& r : rows) {
            w.obj_open();
              w.kv("shards", r.n);
              w.kv("strong_wall_ns", r.strong_wall_ns, 1);
              w.kv("weak_wall_ns", r.weak_wall_ns, 1);
              w.kv("strong_speedup", t1_strong > 0 && r.strong_wall_ns > 0
                                     ? t1_strong / r.strong_wall_ns : 0.0, 4);
              w.kv("strong_efficiency", t1_strong > 0 && r.strong_wall_ns > 0
                                     ? (t1_strong / r.strong_wall_ns) / r.n : 0.0, 4);
              w.kv("weak_efficiency", t1_weak > 0 && r.weak_wall_ns > 0
                                     ? t1_weak / r.weak_wall_ns : 0.0, 4);
              w.kv("cycles_per_packet_strong", r.strong_cycles, 2);
              w.kv("cycles_per_packet_weak", r.weak_cycles, 2);
              w.kv("packets_strong", (long long)r.strong_packets);
              w.kv("packets_weak", (long long)r.weak_packets);
              w.kv("ok", r.ok);
              w.kv("error", r.error);
            w.obj_close();
        }
        w.arr_close();
        // The cross-node half of this experiment is an MPI program that runs on
        // both VMs; it writes its own fragment. Recorded here so the report can
        // state whether the two halves came from the same session.
        w.kv("mpi_fragment", "e2_mpi_scaling.json");
      w.obj_close();
    w.obj_close();

    const std::string path = ctx.outdir + "/e2_scaling.json";
    if (!w.write_file(path)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    printf("  wrote %s\n", path.c_str());
    return 0;
}

} // namespace lr::bench
