// exp_e1_cost_model.cpp — the central experiment.
//
// Sweeps payload size against per-packet cost for four ciphers, with the
// hardware crypto instructions present and masked, and fits
//
//     C(s) = a + b*s          a: cycles that do not depend on payload size
//                             b: cycles per payload byte
//     s*   = a / b            the payload at which the two halves are equal
//
// Two implementations of each of the two ciphers are swept as four levels of one
// factor: OpenSSL's EVP, and the AES-NI/PCLMULQDQ and AVX2/NEON implementations
// written in src/core/simd_crypto.c. Having a second implementation of the same
// algorithm is what separates "this cipher costs X" from "this library's version
// of this cipher costs X", and the gap between the two is itself a result.
//
// Three properties of the design matter more than the sweep itself.
//
//   Each unit runs in a fresh process. The hardware-crypto mask is read at load
//   time by libcrypto and by our own dispatcher, so it cannot be varied inside a
//   process. Re-spawning makes the mask a first-class factor and lets the sweep
//   order be interleaved across it.
//
//   The sweep order is interleaved and seeded, not nested. A nested loop
//   confounds the factor with time: if the machine warms up, or another tenant
//   arrives halfway through, the effect lands entirely on whichever level was
//   running then. Interleaving spreads any drift across all levels.
//
//   An anchor point is measured at the start and at the end of every sweep. If
//   the two disagree by more than the tolerance, the machine changed underneath
//   the measurement and the whole sweep is marked void rather than reported.
#include "bench.hpp"

#include "lr/aead.hpp"
#include "lr/clock.hpp"
#include "lr/platform.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <sys/stat.h>

namespace lr::bench {

namespace {

struct Unit {
    size_t      payload;
    std::string cipher;
    bool        hw_crypto;
};

struct Point {
    Unit unit;
    std::vector<double> cycles;      // one per replicate
    std::vector<double> pps;
    stats::Summary summary;
    std::string aead_backend;
    std::string source = "loopback";
    uint64_t auth_fail = 0;
    std::string error;
};

// One replicate, in this process. Called by the e1-unit subcommand.
int unit_body(int argc, char** argv) {
    ShardConfig cfg;
    cfg.cipher  = arg_str(argc, argv, "cipher", "aes-256-gcm");
    cfg.payload = (size_t)arg_int(argc, argv, "payload", 512);
    cfg.backend = arg_str(argc, argv, "backend", "posix");
    cfg.batch   = (int)arg_int(argc, argv, "batch", 1);
    cfg.cpu     = (int)arg_int(argc, argv, "cpu", -1);
    cfg.replay  = !arg_has(argc, argv, "no-replay");
    const int target = (int)arg_int(argc, argv, "packets", 30000);
    const int warmup = (int)arg_int(argc, argv, "warmup", 3);

    HotPathResult r = run_hotpath(cfg, target, warmup);

    json::Writer w;
    w.obj_open();
      w.kv("ok", r.ok);
      w.kv("cycles_per_packet", r.cycles_per_packet, 3);
      w.kv("pps", r.pps, 1);
      w.kv("syscalls_per_packet", r.syscalls_per_pkt, 4);
      w.kv("packets", (long long)r.packets);
      w.kv("auth_fail", (long long)r.auth_fail);
      w.kv("replay_drop", (long long)r.replay_drop);
      w.kv("aead_backend", r.aead_backend);
      w.kv("hw_crypto_masked", plat::hw_crypto_masked());
      w.kv("openssl_hw_aes_active", plat::openssl_hw_aes_active());
      w.kv("error", r.error);
    w.obj_close();
    printf("%s\n", w.str().c_str());
    return r.ok ? 0 : 1;
}

// Measure one unit by spawning a child, so the hardware-crypto factor can vary.
bool measure_unit(const RunCtx& ctx, const Unit& u, double* cycles, double* pps,
                  std::string* backend, uint64_t* auth_fail, std::string* err) {
    char args[512];
    std::snprintf(args, sizeof(args),
                  "e1-unit --cipher=%s --payload=%zu --packets=%d --warmup=%d",
                  u.cipher.c_str(), u.payload, ctx.target_packets, ctx.warmup_cycles);
    const std::string js = spawn_unit(ctx.exe, args, u.hw_crypto);
    double ok = 0;
    if (!unit_field(js, "ok", &ok) || ok == 0) {
        std::string e;
        unit_string(js, "error", &e);
        *err = e.empty() ? "unit produced no result" : e;
        return false;
    }
    double c = 0, p = 0, af = 0;
    unit_field(js, "cycles_per_packet", &c);
    unit_field(js, "pps", &p);
    unit_field(js, "auth_fail", &af);
    unit_string(js, "aead_backend", backend);

    // The mask is only meaningful if it reached the library. A child that
    // reports hardware AES still active while the mask was set means the lever
    // did not work on this build, and reporting the pair as a contrast would be
    // reporting noise as an effect.
    double masked = 0;
    if (unit_field(js, "hw_crypto_masked", &masked)) {
        const bool child_masked = masked != 0;
        if (child_masked == u.hw_crypto) {
            *err = "hardware-crypto mask did not take effect in the child process";
            return false;
        }
    }
    *cycles = c;
    *pps = p;
    *auth_fail = (uint64_t)af;
    return true;
}

} // namespace

int run_e1_unit(int argc, char** argv) { return unit_body(argc, argv); }

int run_e1(const RunCtx& ctx, int argc, char** argv) {
    printf("E1 cost model\n");
    std::string why;
    if (!measurement_permitted(&why))
        return write_unavailable(ctx, "e1_cost_model.json", "e1", why);

    const bool quick = arg_has(argc, argv, "quick");
    std::vector<size_t> payloads = kPayloads;
    std::vector<std::string> ciphers = kCiphers;
    if (quick) {
        payloads = {64, 256, 512, 1024, 1432};
        ciphers  = {"aes-256-gcm", "lr-aes-256-gcm"};
    }

    // Whether the hardware-crypto factor can be varied at all. On a machine
    // without AES-NI there is nothing to mask, and pretending otherwise would
    // put two identical levels in the design.
    const auto& caps = plat::caps();
    std::vector<bool> hw_levels = {true};
    if (caps.hw_aes && std::strlen(plat::hw_crypto_mask_var()) > 0) hw_levels.push_back(false);

    std::vector<Unit> units;
    for (bool hw : hw_levels)
        for (const auto& c : ciphers)
            for (size_t p : payloads)
                units.push_back(Unit{p, c, hw});

    // Interleave: every replicate of every unit goes into one list, which is
    // then shuffled with a fixed seed. Fixed, so that the order is part of the
    // recorded design and a rerun reproduces it.
    struct Job { size_t unit_index; int rep; };
    std::vector<Job> jobs;
    for (size_t i = 0; i < units.size(); ++i)
        for (int r = 0; r < ctx.reps; ++r) jobs.push_back(Job{i, r});
    std::mt19937_64 rng(ctx.seed);
    std::shuffle(jobs.begin(), jobs.end(), rng);

    std::vector<Point> points(units.size());
    for (size_t i = 0; i < units.size(); ++i) points[i].unit = units[i];

    // The anchor: one fixed configuration, measured before and after the sweep.
    //
    // The median of several units at each end rather than one. A single unit
    // carries the same replicate-to-replicate variance as any other point, so
    // an anchor built from two single observations tests the anchor's own noise
    // as much as the machine's drift, and voids sweeps that were fine.
    const Unit anchor{512, "aes-256-gcm", true};
    const int anchor_n = std::max(3, ctx.reps / 2);
    auto measure_anchor = [&](double* out, std::string* err) -> bool {
        std::vector<double> v;
        for (int i = 0; i < anchor_n; ++i) {
            double c = 0, p = 0;
            uint64_t af = 0;
            std::string backend;
            if (!measure_unit(ctx, anchor, &c, &p, &backend, &af, err)) return false;
            v.push_back(c);
        }
        *out = stats::median(v);
        return true;
    };
    double anchor_before = 0, anchor_after = 0;
    std::string aerr;
    if (!measure_anchor(&anchor_before, &aerr))
        return write_unavailable(ctx, "e1_cost_model.json", "e1",
                                 "anchor measurement failed: " + aerr);

    printf("  %zu units x %d replicates = %zu measurements, interleaved\n",
           units.size(), ctx.reps, jobs.size());

    size_t done = 0, failed = 0;
    for (const Job& j : jobs) {
        Point& pt = points[j.unit_index];
        double c = 0, p = 0;
        uint64_t af = 0;
        std::string backend, err;
        if (measure_unit(ctx, pt.unit, &c, &p, &backend, &af, &err)) {
            pt.cycles.push_back(c);
            pt.pps.push_back(p);
            pt.aead_backend = backend;
            pt.auth_fail += af;
        } else {
            pt.error = err;
            failed++;
        }
        if (ctx.verbose || (++done % 50 == 0))
            printf("    %zu/%zu\n", done, jobs.size());
    }

    if (!measure_anchor(&anchor_after, &aerr))
        return write_unavailable(ctx, "e1_cost_model.json", "e1",
                                 "closing anchor measurement failed: " + aerr);

    const double drift = anchor_before > 0
        ? std::abs(anchor_after - anchor_before) / anchor_before : 1.0;
    const bool anchor_ok = drift <= ctx.anchor_tol;
    printf("  anchor %.1f -> %.1f cycles (%.2f%% drift, tolerance %.0f%%): %s\n",
           anchor_before, anchor_after, drift * 100, ctx.anchor_tol * 100,
           anchor_ok ? "sweep is valid" : "SWEEP VOID");

    for (Point& pt : points)
        if (!pt.cycles.empty())
            pt.summary = stats::summarise(pt.cycles, ctx.cv_threshold, ctx.seed);

    ::mkdir(ctx.outdir.c_str(), 0755);
    json::Writer w;
    w.obj_open();
      write_environment(w, ctx);
      w.key("e1").obj_open();
        w.kv("available", true);
        w.kv("quick", quick);
        w.kv("anchor_cycles_before", anchor_before, 2);
        w.kv("anchor_cycles_after", anchor_after, 2);
        w.kv("anchor_drift", drift, 5);
        w.kv("anchor_ok", anchor_ok);
        w.kv("anchor_replicates", anchor_n);
        w.kv("failed_units", (long long)failed);
        w.kv("interleaved", true);
        w.kv("source", "loopback");
        w.kv_strs("ciphers_swept", ciphers);
        {
            std::vector<double> ps;
            for (size_t p : payloads) ps.push_back((double)p);
            w.kv_nums("payloads_swept", ps, 0);
        }
        w.key("points").arr_open();
        for (const Point& pt : points) {
            w.obj_open();
              w.kv("payload_bytes", (long long)pt.unit.payload);
              w.kv("cipher", pt.unit.cipher);
              w.kv("cipher_family", aead_is_local(pt.unit.cipher) ? "linerate" : "openssl");
              w.kv("hw_crypto", pt.unit.hw_crypto);
              w.kv("aead_backend", pt.aead_backend);
              w.kv("n", (long long)pt.cycles.size());
              w.kv("cycles_median", pt.summary.median, 3);
              w.kv("cycles_ci_lo", pt.summary.ci_lo, 3);
              w.kv("cycles_ci_hi", pt.summary.ci_hi, 3);
              w.kv("cycles_cv", pt.summary.cv, 5);
              w.kv("stable", pt.summary.stable);
              w.kv("pps_median", pt.pps.empty() ? 0.0 : stats::median(pt.pps), 1);
              w.kv("auth_fail", (long long)pt.auth_fail);
              w.kv_nums("cycles_replicates", pt.cycles, 3);
              w.kv("error", pt.error);
            w.obj_close();
        }
        w.arr_close();
      w.obj_close();
    w.obj_close();

    const std::string path = ctx.outdir + "/e1_cost_model.json";
    if (!w.write_file(path)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    printf("  wrote %s (%zu points, %zu failed)\n", path.c_str(), points.size(), failed);
    return 0;
}

} // namespace lr::bench
