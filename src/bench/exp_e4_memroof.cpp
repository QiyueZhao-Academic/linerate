// exp_e4_memroof.cpp — E4, the memory roof.
//
// A data plane cannot move bytes faster than the memory system can supply them,
// divided by the number of times each byte is touched. This experiment measures
// the numerator directly, so that the scaling plot in E2 can be drawn against a
// roof rather than against nothing.
//
// Two measurements are made.
//
// Sustained bandwidth. A STREAM-style triad, a[i] = b[i] + q*c[i], over arrays
// far larger than the last-level cache. Two reads and one write per element, and
// on a write-allocate machine the write costs a read as well; the conventional
// STREAM accounting of three streams is used here and stated in the report so
// the roof can be recomputed under a different convention.
//
// Working-set sweep. The same kernel over a geometric range of sizes, from well
// inside L1 to well beyond the last-level cache. The plateaus in this curve are
// the cache hierarchy, and they say which level a packet buffer of a given size
// is actually being served from -- which is the mechanism behind the cross-core
// interference term the scaling fit reports.
#include "bench.hpp"

#include "lr/clock.hpp"
#include "lr/platform.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <vector>

namespace lr::bench {
namespace {

// Kept out of line and fed a runtime scalar so the compiler cannot fold the
// loop away or replace it with a memset.
double triad_once(double* __restrict a, const double* __restrict b,
                  const double* __restrict c, size_t n, double q) {
    const uint64_t t0 = clk::ticks();
    for (size_t i = 0; i < n; ++i) a[i] = b[i] + q * c[i];
    const uint64_t dt = clk::ticks() - t0;
    return (double)dt / (double)clk::tick_hz();   // seconds
}

// Bandwidth in GB/s for a triad over n elements, taking the median of `reps`.
double triad_gbps(size_t n, int reps, double* sink) {
    std::vector<double> a(n), b(n), c(n);
    for (size_t i = 0; i < n; ++i) { b[i] = 1.0 + (double)i; c[i] = 2.0; a[i] = 0.0; }
    std::vector<double> gbps;
    gbps.reserve(reps);
    for (int r = 0; r < reps; ++r) {
        // A different scalar each pass, so no result can be reused.
        const double q = 3.0 + 0.001 * r;
        double s = triad_once(a.data(), b.data(), c.data(), n, q);
        if (s > 0) gbps.push_back(3.0 * (double)n * sizeof(double) / s / 1e9);
        *sink += a[n / 2];
    }
    return stats::median(gbps);
}

} // namespace

int run_e4(const RunCtx& ctx, int argc, char** argv) {
    printf("E4 memory roof\n");
    std::string why;
    if (!measurement_permitted(&why))
        return write_unavailable(ctx, "e4_memroof.json", "e4", why);

    const bool quick = arg_has(argc, argv, "quick");
    double sink = 0.0;

    // Working-set sweep: 8 KiB up to well past the last-level cache, at three
    // points per octave. The quick grid stops at 24 MiB, which is enough to
    // exercise the code path but not to see a sustained-bandwidth plateau, and
    // the fragment records that it was a quick run.
    const size_t top_kb = quick ? 24u * 1024u : 192u * 1024u;
    std::vector<size_t> bytes;
    for (size_t kb = 8; kb <= top_kb; kb = (kb * 3) / 2)
        bytes.push_back(kb * 1024);
    // The triad holds three arrays, so the resident set is three times the
    // per-array size quoted below.

    struct Row { size_t per_array_bytes; double gbps; };
    std::vector<Row> rows;
    for (size_t B : bytes) {
        size_t n = B / sizeof(double);
        if (n < 1024) continue;
        // Smaller working sets are re-run more often so every point takes a
        // comparable amount of wall time and carries comparable noise.
        int reps = (B < (1u << 20)) ? (quick ? 51 : 201)
                 : (B < (32u << 20) ? (quick ? 7 : 21) : (quick ? 3 : 7));
        double g = triad_gbps(n, reps, &sink);
        rows.push_back({B, g});
        if (ctx.verbose) printf("    %8zu KiB  %6.2f GB/s\n", B / 1024, g);
    }

    // Sustained bandwidth: the largest working set measured, which is well past
    // the last-level cache on any machine this study targets.
    const double sustained = rows.empty() ? 0.0 : rows.back().gbps;

    // Bytes of memory traffic per packet in the hot path, enumerated rather than
    // assumed. Per payload byte: the kernel copies in on recv (read + write),
    // the AEAD reads ciphertext and writes plaintext on open, reads plaintext
    // and writes ciphertext on seal, and the kernel copies out on send
    // (read + write). Eight passes, of which six are unavoidable at the
    // application level and two belong to the socket API.
    const int passes_total = 8;
    const int passes_app   = 6;

    json::Writer w;
    w.obj_open();
    write_environment(w, ctx);
    w.key("e4").obj_open();
    w.kv("available", true);
    w.kv("quick", quick);
    w.kv("kernel", std::string("STREAM triad a[i] = b[i] + q*c[i], 3 streams of 8 bytes"));
    w.kv("stream_bandwidth_gbps", sustained, 6);
    w.kv("largest_working_set_bytes", (long long)(rows.empty() ? 0 : rows.back().per_array_bytes));
    w.kv("payload_passes_total", passes_total);
    w.kv("payload_passes_application", passes_app);
    w.kv("passes_note", std::string("recv copy (2), aead.open (2), aead.seal (2), send copy (2)"));
    // The roof is a byte rate the data plane cannot exceed: sustained bandwidth
    // divided by the number of passes each payload byte makes through memory.
    w.kv("roof_payload_gbps", sustained / passes_total, 6);
    w.kv("roof_payload_gbps_application_only", sustained / passes_app, 6);
    w.key("points").arr_open();
    for (const auto& r : rows) {
        w.obj_open();
        w.kv("per_array_bytes", (long long)r.per_array_bytes);
        w.kv("resident_bytes", (long long)(3 * r.per_array_bytes));
        w.kv("gbps", r.gbps, 6);
        w.obj_close();
    }
    w.arr_close();
    w.obj_close();
    w.obj_close();

    // Keeps the accumulated result observable so the optimiser cannot delete
    // the triad loops. The branch is never taken.
    if (sink == 1e300) fprintf(stderr, "unreachable\n");

    ::mkdir(ctx.outdir.c_str(), 0755);
    const std::string path = ctx.outdir + "/e4_memroof.json";
    if (!w.write_file(path)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    printf("  wrote %s (%zu points, sustained %.1f GB/s)\n",
           path.c_str(), rows.size(), sustained);
    return 0;
}

} // namespace lr::bench
