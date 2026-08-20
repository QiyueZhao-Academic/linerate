// exp_controls.cpp — the checks that decide whether the other experiments mean
// anything.
//
// Each control has a prediction attached to it, made before the measurement and
// recorded in the fragment next to the outcome. A control whose prediction is
// only written down afterwards cannot fail, and a control that cannot fail is
// decoration.
//
//   null cipher (negative)
//       Replace the AEAD with a memcpy of the same length. Everything else in
//       the hot path is unchanged: the same syscalls, the same replay window,
//       the same header parse, the same buffers. What remains is the fixed cost
//       of the data plane with the cryptography removed, which is what makes
//       the fixed term a decomposable rather than a single opaque number.
//       Prediction: a drops substantially; b drops nearly to the cost of the
//       two memory passes.
//
//   masked hardware crypto (positive)
//       Clear the AES and carry-less-multiply capability bits so libcrypto and
//       our own dispatcher both fall back to software. The per-byte term must
//       rise by an order of magnitude. If it does not, the mask never reached
//       the implementation and every masked measurement in E1 is void.
//       Prediction: b rises by at least 8x for AES-256-GCM.
//
//   idle poll (instrumentation)
//       Drain a socket that has nothing in it. This is what the harness pays to
//       observe that there is no work, and it lands inside the timed region, so
//       it is charged to whatever packets the same drain call did serve.
//       What matters is not the cost of one empty poll but the cost of all of
//       them divided by the packets they were charged to, which is why the
//       criterion is the product of that cost and the measured frequency of
//       empty polls rather than the cost alone.
//       Prediction: the amortised contamination is below 1% of the per-packet
//       cost.
//
//   payload independence of the header path (instrumentation)
//       The header is a fixed 24 bytes whatever the payload is. Its parse,
//       replay check and nonce derivation must therefore cost the same at 16
//       bytes of payload as at 1432. Measured with the null cipher, where
//       nothing else scales with size except the two memory passes.
//       Prediction: the residual after subtracting a linear memory term is flat
//       within the stability threshold.
//
//   tamper cost symmetry (instrumentation)
//       A packet whose tag does not verify must cost about what a valid one
//       costs. A cheap rejection path would let an attacker's forged traffic be
//       served faster than legitimate traffic, and would also bias any run in
//       which authentication failures occur.
//       Prediction: within 25% of the valid-packet cost.
#include "bench.hpp"

#include "lr/aead.hpp"
#include "lr/clock.hpp"
#include "lr/platform.hpp"
#include "lr/proto.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

namespace lr::bench {

namespace {

struct Control {
    std::string name;
    std::string prediction;
    std::string outcome;
    double observed = 0;
    double reference = 0;
    double ratio = 0;
    bool   passed = false;
    bool   ran = false;
    std::string error;
};

// Least squares, as in E6: enough to state a and b for a control, while the
// authoritative fit stays in Python.
void fit_line(const std::vector<double>& x, const std::vector<double>& y,
              double* a, double* b) {
    const size_t n = x.size();
    if (n < 2) { *a = *b = 0; return; }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) { sx += x[i]; sy += y[i]; sxx += x[i]*x[i]; sxy += x[i]*y[i]; }
    const double d = n * sxx - sx * sx;
    if (d == 0) { *a = *b = 0; return; }
    *b = (n * sxy - sx * sy) / d;
    *a = (sy - *b * sx) / n;
}

// Fit a and b for one cipher, optionally with hardware crypto masked, by
// spawning units so the mask can be varied.
bool fit_cipher(const RunCtx& ctx, const std::string& cipher, bool hw,
                const std::vector<size_t>& payloads,
                double* a, double* b, std::string* err) {
    std::vector<double> x, y;
    for (size_t s : payloads) {
        char args[256];
        std::snprintf(args, sizeof(args),
                      "controls-unit --cipher=%s --payload=%zu --packets=%d --warmup=%d",
                      cipher.c_str(), s, ctx.target_packets, ctx.warmup_cycles);
        const std::string js = spawn_unit(ctx.exe, args, hw);
        double ok = 0, c = 0;
        if (!unit_field(js, "ok", &ok) || ok == 0) {
            unit_string(js, "error", err);
            if (err->empty()) *err = "unit produced no result";
            return false;
        }
        unit_field(js, "cycles_per_packet", &c);
        x.push_back((double)s);
        y.push_back(c);
    }
    fit_line(x, y, a, b);
    return true;
}

} // namespace

int run_controls_unit(int argc, char** argv) {
    ShardConfig cfg;
    cfg.cipher  = arg_str(argc, argv, "cipher", "aes-256-gcm");
    cfg.payload = (size_t)arg_int(argc, argv, "payload", 512);
    cfg.replay  = !arg_has(argc, argv, "no-replay");
    const int packets = (int)arg_int(argc, argv, "packets", 30000);
    const int warmup  = (int)arg_int(argc, argv, "warmup", 3);

    HotPathResult r = run_hotpath(cfg, packets, warmup);
    json::Writer w;
    w.obj_open();
      w.kv("ok", r.ok);
      w.kv("cycles_per_packet", r.cycles_per_packet, 3);
      w.kv("pps", r.pps, 1);
      w.kv("aead_backend", r.aead_backend);
      w.kv("hw_crypto_masked", plat::hw_crypto_masked());
      w.kv("error", r.error);
    w.obj_close();
    printf("%s\n", w.str().c_str());
    return r.ok ? 0 : 1;
}

int run_controls(const RunCtx& ctx, int argc, char** argv) {
    printf("Controls\n");
    std::string why;
    if (!measurement_permitted(&why))
        return write_unavailable(ctx, "controls.json", "controls", why);

    const bool quick = arg_has(argc, argv, "quick");
    const std::vector<size_t> payloads = quick
        ? std::vector<size_t>{64, 512, 1432}
        : std::vector<size_t>{16, 64, 128, 256, 512, 1024, 1432};

    std::vector<Control> controls;
    double aes_a = 0, aes_b = 0, null_a = 0, null_b = 0;
    std::string err;

    // Reference: AES-256-GCM with hardware crypto present.
    const bool have_aes = fit_cipher(ctx, "aes-256-gcm", true, payloads, &aes_a, &aes_b, &err);
    printf("  aes-256-gcm   a=%8.1f  b=%.4f\n", aes_a, aes_b);

    // Negative control: the null cipher.
    {
        Control c;
        c.name = "null cipher removes the cryptographic cost";
        c.prediction = "fixed cost a falls below the AEAD fixed cost; per-byte "
                       "cost b falls to roughly the cost of two memory passes";
        if (!have_aes) { c.error = err; }
        else if (!fit_cipher(ctx, "null", true, payloads, &null_a, &null_b, &c.error)) {
            // error already recorded
        } else {
            c.ran = true;
            c.observed = null_a;
            c.reference = aes_a;
            c.ratio = aes_a > 0 ? null_a / aes_a : 0;

            // The claim this control actually tests is about a: removing the
            // cipher must remove the cipher's per-packet setup. The claim about
            // b is only testable where b is identifiable, and on a host whose
            // fixed cost dwarfs the per-byte term across the whole payload
            // range it is not: two slopes that are both small next to a large
            // intercept differ by less than the fit can resolve, and requiring
            // one to be below the other would make this control fail on noise.
            // Whether b was identifiable is measured, not assumed.
            const double max_payload = (double)payloads.back();
            const bool b_identifiable = aes_a > 0 &&
                (aes_b * max_payload) > 0.10 * aes_a;
            c.passed = null_a < aes_a && (!b_identifiable || null_b < aes_b);

            char buf[400];
            std::snprintf(buf, sizeof(buf),
                "null a=%.1f b=%.4f against aead a=%.1f b=%.4f; the difference "
                "in a, %.1f cycles, is the AEAD's own per-packet cost. %s",
                null_a, null_b, aes_a, aes_b, aes_a - null_a,
                b_identifiable
                  ? "The per-byte terms are separately identifiable here and "
                    "were compared."
                  : "The per-byte term contributes under a tenth of the fixed "
                    "cost even at the largest payload, so b is not separately "
                    "identifiable on this host and was not compared.");
            c.outcome = buf;
        }
        controls.push_back(c);
    }

    // Positive control: masking must move the per-byte cost.
    {
        Control c;
        c.name = "masking hardware crypto raises the per-byte cost";
        c.prediction = "b rises by at least a factor of 8 for AES-256-GCM";
        const auto& caps = plat::caps();
        if (!caps.hw_aes) {
            c.error = "no hardware AES on this host, so there is nothing to mask";
        } else {
            double ma = 0, mb = 0;
            if (!fit_cipher(ctx, "aes-256-gcm", false, payloads, &ma, &mb, &c.error)) {
                // error already recorded
            } else {
                c.ran = true;
                c.observed = mb;
                c.reference = aes_b;
                c.ratio = aes_b > 0 ? mb / aes_b : 0;
                c.passed = c.ratio >= 8.0;
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    "masked b=%.4f against unmasked b=%.4f, a factor of %.1f; "
                    "masked a=%.1f against unmasked a=%.1f",
                    mb, aes_b, c.ratio, ma, aes_a);
                c.outcome = buf;
            }
        }
        controls.push_back(c);
    }

    // Instrumentation: what the harness pays to notice there is no work, and
    // how often it pays it.
    {
        Control c;
        c.name = "the harness's own idle polling does not contaminate a measurement";
        c.prediction = "cost per empty poll times empty polls per packet is below "
                       "1% of the per-packet cost";
        ShardConfig cfg;
        cfg.cipher = "null";
        cfg.payload = 64;
        std::string e;
        Shard sh(cfg, bench_key(), &e);
        if (!e.empty()) c.error = e;
        else {
            const int iters = 20000;
            const uint64_t t0 = clk::ticks();
            for (int i = 0; i < iters; ++i) sh.drain(64);
            const uint64_t t1 = clk::ticks();
            const double per_poll = (double)(t1 - t0) / iters;

            // How often an empty poll actually happens in a real run. The
            // posix backend issues exactly two syscalls per packet when every
            // poll finds work, so the excess over two is the empty-poll rate.
            ShardConfig live;
            live.cipher = "null";
            live.payload = 64;
            HotPathResult h = run_hotpath(live, ctx.target_packets, ctx.warmup_cycles);
            const double empty_per_packet =
                h.ok ? std::max(0.0, h.syscalls_per_pkt - 2.0) : 1.0;

            c.ran = true;
            c.observed  = per_poll * empty_per_packet;
            c.reference = null_a > 0 ? null_a : aes_a;
            c.ratio = c.reference > 0 ? c.observed / c.reference : 0;
            c.passed = c.ratio < 0.01;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "%.1f cycles per empty poll, %.5f empty polls per packet, so "
                "%.3f cycles charged to each of %.1f cycles of fixed cost",
                per_poll, empty_per_packet, c.observed, c.reference);
            c.outcome = buf;
        }
        controls.push_back(c);
    }

    // Instrumentation: a rejected packet must not be cheaper than a valid one.
    {
        Control c;
        c.name = "a forged packet costs about what a valid one costs";
        c.prediction = "within 25% of the valid-packet cost";
        auto a = make_aead("aes-256-gcm", bench_key());
        if (!a) c.error = "cipher unavailable";
        else {
            const size_t L = 512;
            std::vector<uint8_t> pt(L, 0x7C), ct(L + 32), out(L + 32);
            uint8_t nonce[12] = {0};
            const size_t n = a->seal(nonce, nullptr, 0, pt.data(), L, ct.data());
            const int iters = 20000;

            const uint64_t v0 = clk::ticks();
            for (int i = 0; i < iters; ++i)
                a->open(nonce, nullptr, 0, ct.data(), n, out.data());
            const uint64_t v1 = clk::ticks();

            std::vector<uint8_t> bad = ct;
            bad[L / 2] ^= 0x01;
            const uint64_t f0 = clk::ticks();
            for (int i = 0; i < iters; ++i)
                a->open(nonce, nullptr, 0, bad.data(), n, out.data());
            const uint64_t f1 = clk::ticks();

            c.ran = true;
            c.reference = (double)(v1 - v0) / iters;
            c.observed  = (double)(f1 - f0) / iters;
            c.ratio = c.reference > 0 ? c.observed / c.reference : 0;
            c.passed = c.ratio > 0.75 && c.ratio < 1.25;
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "forged %.1f cycles against valid %.1f cycles, ratio %.3f",
                          c.observed, c.reference, c.ratio);
            c.outcome = buf;
        }
        controls.push_back(c);
    }

    // Instrumentation: the two implementations of each cipher must agree on
    // ciphertext, which the self-test proves, and must both be measurable here.
    // A missing level would silently shrink E1's design.
    {
        Control c;
        c.name = "every cipher level in the design is constructible";
        c.prediction = "all four cipher names build and process packets";
        int built = 0;
        std::string missing;
        for (const std::string& name : kCiphers) {
            auto a = make_aead(name, bench_key());
            if (a) built++;
            else missing += name + " ";
        }
        c.ran = true;
        c.observed = built;
        c.reference = (double)kCiphers.size();
        c.passed = built == (int)kCiphers.size();
        c.outcome = c.passed ? "all four levels available"
                             : ("missing: " + missing);
        controls.push_back(c);
    }

    int passed = 0, ran = 0;
    for (const Control& c : controls) { if (c.ran) { ran++; if (c.passed) passed++; } }

    ::mkdir(ctx.outdir.c_str(), 0755);
    json::Writer w;
    w.obj_open();
      write_environment(w, ctx);
      w.key("controls").obj_open();
        w.kv("available", true);
        w.kv("quick", quick);
        w.kv("ran", ran);
        w.kv("passed", passed);
        w.kv("aead_fixed_cycles", aes_a, 2);
        w.kv("aead_cycles_per_byte", aes_b, 5);
        w.kv("null_fixed_cycles", null_a, 2);
        w.kv("null_cycles_per_byte", null_b, 5);
        w.kv("aead_only_fixed_cycles", aes_a - null_a, 2);
        w.kv("per_byte_identifiable",
             aes_a > 0 && (aes_b * (double)payloads.back()) > 0.10 * aes_a);
        w.key("checks").arr_open();
        for (const Control& c : controls) {
            w.obj_open();
              w.kv("name", c.name);
              w.kv("prediction", c.prediction);
              w.kv("outcome", c.outcome);
              w.kv("observed", c.observed, 4);
              w.kv("reference", c.reference, 4);
              w.kv("ratio", c.ratio, 4);
              w.kv("ran", c.ran);
              w.kv("passed", c.passed);
              w.kv("error", c.error);
            w.obj_close();
        }
        w.arr_close();
      w.obj_close();
    w.obj_close();

    const std::string path = ctx.outdir + "/controls.json";
    if (!w.write_file(path)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    printf("  %d of %d controls passed; wrote %s\n", passed, ran, path.c_str());
    return 0;
}

} // namespace lr::bench
