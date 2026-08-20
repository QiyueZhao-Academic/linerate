// exp_e3_iopath.cpp — how much of the fixed cost a is the kernel boundary.
//
// The same hot path is run over four transports at a range of batch depths. The
// cipher is held constant, so the difference between two rows is the cost of
// moving a packet across the system-call boundary and nothing else.
//
//   posix         one recvfrom and one sendto per packet: two crossings
//   mmsg          recvmmsg/sendmmsg: two crossings per batch of B
//   uring         io_uring: submissions and completions share a mapped ring,
//                 one io_uring_enter per batch
//   uring-sqpoll  io_uring with a kernel-side submission thread: submission
//                 costs no crossing at all
//
// The last row is the point of the experiment. Without it, the syscall share of
// a can only be extrapolated from the batching curve — the earlier version of
// this study inferred a bound that way and said so. With it, the bound is
// measured: sqpoll is what an unprivileged process can reach without a
// userspace driver, and the gap between posix and sqpoll at B=1 is the cost of
// the boundary itself rather than of amortising it.
//
// Measured syscalls per packet are reported alongside, taken from a counter
// inside each backend, because a batch that comes back short does not issue the
// number of syscalls the design intends.
#include "bench.hpp"

#include "lr/clock.hpp"
#include "lr/platform.hpp"
#include "lr/transport.hpp"

#include <algorithm>
#include <cstdio>
#include <sys/stat.h>

namespace lr::bench {

int run_e3(const RunCtx& ctx, int argc, char** argv) {
    printf("E3 I/O path\n");
    std::string why;
    if (!measurement_permitted(&why))
        return write_unavailable(ctx, "e3_iopath.json", "e3", why);

    const bool quick = arg_has(argc, argv, "quick");
    const std::string cipher = arg_str(argc, argv, "e3-cipher", "aes-256-gcm");
    std::vector<size_t> payloads = quick ? std::vector<size_t>{64, 1432}
                                         : std::vector<size_t>{64, 512, 1432};
    std::vector<int> batches = quick ? std::vector<int>{1, 8, 32} : kBatchSizes;
    const std::vector<std::string>& backends = transport_backends();
    const int reps = quick ? 3 : std::max(3, ctx.reps / 2);
    const int packets = quick ? 4000 : ctx.target_packets;

    struct Row {
        std::string backend;
        int    batch = 0;
        size_t payload = 0;
        stats::Summary cycles;
        double syscalls_per_packet = 0;
        double pps = 0;
        bool   ok = false;
        std::string error;
    };
    std::vector<Row> rows;

    for (const std::string& b : backends) {
        for (size_t payload : payloads) {
            for (int batch : batches) {
                // A batch depth is meaningless for a backend that issues one
                // syscall per packet by construction; recording B>1 for posix
                // would put a level in the design that does not exist.
                if (b == "posix" && batch != 1) continue;

                Row r;
                r.backend = b;
                r.batch = batch;
                r.payload = payload;

                std::vector<double> cyc, sc, pps;
                for (int rep = 0; rep < reps; ++rep) {
                    ShardConfig cfg;
                    cfg.cipher  = cipher;
                    cfg.backend = b;
                    cfg.batch   = batch;
                    cfg.payload = payload;
                    cfg.cpu     = plat::caps().hard_affinity ? 0 : -1;
                    HotPathResult h = run_hotpath(cfg, packets, ctx.warmup_cycles);
                    if (!h.ok) { r.error = h.error; break; }
                    cyc.push_back(h.cycles_per_packet);
                    sc.push_back(h.syscalls_per_pkt);
                    pps.push_back(h.pps);
                }
                if (!cyc.empty()) {
                    r.cycles = stats::summarise(cyc, ctx.cv_threshold, ctx.seed);
                    r.syscalls_per_packet = stats::median(sc);
                    r.pps = stats::median(pps);
                    r.ok = true;
                }
                rows.push_back(r);
                if (ctx.verbose)
                    printf("    %-12s B=%-3d s=%-5zu %8.1f cycles  %.3f syscalls/pkt\n",
                           b.c_str(), batch, payload, r.cycles.median,
                           r.syscalls_per_packet);
            }
        }
    }

    // The headline contrast, at the largest payload and B=1: what a system call
    // costs when there is nothing to amortise it over.
    double posix_b1 = 0, sqpoll_b1 = 0, best_batched = 0;
    std::string best_batched_label;
    for (const Row& r : rows) {
        if (!r.ok || r.payload != payloads.back()) continue;
        if (r.backend == "posix" && r.batch == 1) posix_b1 = r.cycles.median;
        if (r.backend == "uring-sqpoll" && r.batch == 1) sqpoll_b1 = r.cycles.median;
        if (r.batch == batches.back() &&
            (best_batched == 0 || r.cycles.median < best_batched)) {
            best_batched = r.cycles.median;
            best_batched_label = r.backend;
        }
    }

    ::mkdir(ctx.outdir.c_str(), 0755);
    json::Writer w;
    w.obj_open();
      write_environment(w, ctx);
      w.key("e3").obj_open();
        w.kv("available", true);
        w.kv("quick", quick);
        w.kv("cipher", cipher);
        w.kv("replicates", reps);
        w.kv_strs("backends", backends);
        {
            std::string why_uring, why_sqpoll;
            const bool u = uring_available(&why_uring);
            const bool s = uring_sqpoll_available(&why_sqpoll);
            w.kv("uring_available", u);
            w.kv("uring_reason", why_uring);
            w.kv("uring_sqpoll_available", s);
            w.kv("uring_sqpoll_reason", why_sqpoll);
        }
        w.kv("posix_b1_cycles", posix_b1, 2);
        w.kv("sqpoll_b1_cycles", sqpoll_b1, 2);
        w.kv("posix_minus_sqpoll_b1_cycles",
             (posix_b1 > 0 && sqpoll_b1 > 0) ? posix_b1 - sqpoll_b1 : 0.0, 2);
        // A kernel-side submission poller is a second runnable thread. On a host
        // with one usable core it competes with the data plane for that core, so
        // the syscalls it removes cost more in scheduling than they saved. The
        // condition is recorded rather than inferred, because a negative
        // "saving" is a property of the host, not of the mechanism, and the
        // report must not present it as either a bug or a benefit.
        w.kv("sqpoll_contends_for_cpu", plat::caps().physical_cores < 2);
        w.kv("sqpoll_interpretation",
             plat::caps().physical_cores < 2
               ? "fewer than two usable cores: the submission poller and the "
                 "data plane share one core, so the syscall-free path is slower "
                 "than the syscall path and the difference bounds nothing"
               : "the poller has its own core, so the difference between posix "
                 "and sqpoll at B=1 bounds the cost of the system-call boundary");
        w.kv("best_batched_backend", best_batched_label);
        w.kv("best_batched_cycles", best_batched, 2);
        w.key("points").arr_open();
        for (const Row& r : rows) {
            w.obj_open();
              w.kv("backend", r.backend);
              w.kv("batch", r.batch);
              w.kv("payload_bytes", (long long)r.payload);
              w.kv("cycles_median", r.cycles.median, 3);
              w.kv("cycles_ci_lo", r.cycles.ci_lo, 3);
              w.kv("cycles_ci_hi", r.cycles.ci_hi, 3);
              w.kv("cycles_cv", r.cycles.cv, 5);
              w.kv("stable", r.cycles.stable);
              w.kv("syscalls_per_packet", r.syscalls_per_packet, 4);
              w.kv("pps_median", r.pps, 1);
              w.kv("ok", r.ok);
              w.kv("error", r.error);
            w.obj_close();
        }
        w.arr_close();
      w.obj_close();
    w.obj_close();

    const std::string path = ctx.outdir + "/e3_iopath.json";
    if (!w.write_file(path)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    printf("  wrote %s (%zu rows)\n", path.c_str(), rows.size());
    return 0;
}

} // namespace lr::bench
