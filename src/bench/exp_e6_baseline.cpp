// exp_e6_baseline.cpp — the comparisons that make a number mean something.
//
// A cost model fitted to one program's own measurements is self-consistent by
// construction. This experiment puts three independent references beside it.
//
//   openssl speed -evp   the same cipher, timed by a program this project did
//                        not write, on the same machine, at the same lengths.
//                        It measures bulk throughput with no packet framing at
//                        all, so it estimates the per-byte term b directly and
//                        independently. If our fitted b disagrees with it by
//                        much, the fit is wrong.
//
//   iperf3 over UDP      the same interface with no cryptography and no
//                        user-space forwarding. The upper bound on what any
//                        encrypted data plane on this link could achieve.
//
//   WireGuard            a production encrypted tunnel, in the kernel, carrying
//                        the same traffic over the same link. This is the
//                        number a reader actually wants: not "is our data plane
//                        fast" but "how does its per-packet cost compare with a
//                        deployed implementation of the same idea".
//
// The last two need a peer, root, and tools that are not present on every host,
// so they are produced by cloud/baselines.sh and merged from a separate
// fragment. What runs here is the part that needs neither: openssl speed, and
// our own data plane at the same lengths.
#include "bench.hpp"

#include "lr/aead.hpp"
#include "lr/clock.hpp"
#include "lr/platform.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace lr::bench {

namespace {

// Parse the block-size table `openssl speed -evp NAME` prints. The layout has
// changed across OpenSSL releases, so this reads the numeric columns of the
// data row rather than trusting fixed offsets.
struct SpeedRow { long bytes; double kbytes_per_sec; };

std::vector<SpeedRow> parse_openssl_speed(const std::string& out,
                                          const std::vector<long>& sizes) {
    std::vector<SpeedRow> rows;
    // The layout of this table has changed across OpenSSL releases and the
    // header text differs again between the FIPS and default providers, so
    // rather than keying on any particular phrase this scans every line for the
    // one carrying the most `<number>k` tokens. On every version seen that is
    // the data row, and on none of them is it a header.
    std::vector<double> best;
    size_t pos = 0;
    while (pos <= out.size()) {
        const size_t nl = out.find('\n', pos);
        const std::string line = out.substr(pos, nl == std::string::npos
                                                 ? std::string::npos : nl - pos);
        std::vector<double> rates;
        const char* s = line.c_str();
        while (*s) {
            char* end = nullptr;
            const double v = strtod(s, &end);
            if (end == s) { ++s; continue; }
            if (*end == 'k') { rates.push_back(v); s = end + 1; }
            else s = end;
        }
        if (rates.size() > best.size()) best = rates;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    // openssl states its own unit: the numbers are thousands of bytes per
    // second, so the suffix is a factor of 1000 and not 1024. Getting this wrong
    // would bias every derived cycles-per-byte figure by 2.4%.
    for (size_t i = 0; i < best.size() && i < sizes.size(); ++i)
        rows.push_back(SpeedRow{sizes[i], best[i]});
    return rows;
}

// Least squares on cost per packet against payload size, used here only to put
// our own numbers on the same axis as the reference. The authoritative fit,
// with its confidence intervals and diagnostics, is in python/lr/model.py.
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

} // namespace

int run_e6(const RunCtx& ctx, int argc, char** argv) {
    printf("E6 baselines\n");
    std::string why;
    if (!measurement_permitted(&why))
        return write_unavailable(ctx, "e6_baseline.json", "e6", why);

    const bool quick = arg_has(argc, argv, "quick");
    const std::vector<long> sizes = {16, 64, 256, 1024, 8192, 16384};
    const std::vector<std::string> ciphers = quick
        ? std::vector<std::string>{"aes-256-gcm"}
        : std::vector<std::string>{"aes-256-gcm", "chacha20-poly1305"};

    struct Ref {
        std::string cipher;
        std::vector<SpeedRow> rows;
        double cycles_per_byte = 0;   // at the largest block, where framing is negligible
        bool ok = false;
        std::string error;
    };
    std::vector<Ref> refs;

    int rc = 0;
    const std::string probe = capture("openssl version 2>/dev/null", &rc);
    const bool have_openssl_cli = rc == 0 && !probe.empty();

    for (const std::string& c : ciphers) {
        Ref r;
        r.cipher = c;
        if (!have_openssl_cli) {
            r.error = "openssl command-line tool not found";
            refs.push_back(r);
            continue;
        }
        // -seconds 1 keeps the whole experiment short; the numbers are stable
        // at that duration because the kernel being timed is a tight loop.
        const std::string cmd = "openssl speed -evp " + c +
                                (quick ? " -seconds 1" : " -seconds 2") + " 2>/dev/null";
        const std::string out = capture(cmd, &rc);
        r.rows = parse_openssl_speed(out, sizes);
        if (r.rows.empty()) { r.error = "could not parse openssl speed output"; }
        else {
            const double kbps = r.rows.back().kbytes_per_sec;
            const double bytes_per_sec = kbps * 1000.0;
            const double hz = (double)(plat::caps().nominal_cpu_hz
                                       ? plat::caps().nominal_cpu_hz : clk::tick_hz());
            r.cycles_per_byte = bytes_per_sec > 0 ? hz / bytes_per_sec : 0;
            r.ok = true;
        }
        refs.push_back(r);
        printf("    openssl speed %-22s %s\n", c.c_str(),
               r.ok ? "ok" : r.error.c_str());
    }

    // Our own data plane at the same lengths, so the two sit on one axis. This
    // is the full decrypt-and-re-encrypt path, which passes over each payload
    // byte twice, so its per-byte slope is expected to be about twice the
    // reference's and the report states that rather than treating the factor of
    // two as a discrepancy.
    struct Ours { std::string cipher; std::vector<double> x, y; double a = 0, b = 0; };
    std::vector<Ours> ours;
    for (const std::string& c : ciphers) {
        Ours o;
        o.cipher = c;
        for (size_t s : (quick ? std::vector<size_t>{64, 512, 1432} : kPayloads)) {
            ShardConfig cfg;
            cfg.cipher = c;
            cfg.payload = s;
            cfg.cpu = plat::caps().hard_affinity ? 0 : -1;
            HotPathResult h = run_hotpath(cfg, quick ? 4000 : ctx.target_packets,
                                          ctx.warmup_cycles);
            if (!h.ok) continue;
            o.x.push_back((double)s);
            o.y.push_back(h.cycles_per_packet);
        }
        fit_line(o.x, o.y, &o.a, &o.b);
        ours.push_back(o);
        printf("    linerate      %-22s a=%.1f cycles  b=%.4f cycles/byte\n",
               c.c_str(), o.a, o.b);
    }

    // Was the network half produced? cloud/baselines.sh writes it separately.
    struct stat sb{};
    const std::string netfrag = ctx.outdir + "/e6_network_baselines.json";
    const bool have_net = ::stat(netfrag.c_str(), &sb) == 0;

    ::mkdir(ctx.outdir.c_str(), 0755);
    json::Writer w;
    w.obj_open();
      write_environment(w, ctx);
      w.key("e6").obj_open();
        w.kv("available", true);
        w.kv("quick", quick);
        w.kv("openssl_cli_available", have_openssl_cli);
        w.kv("network_fragment_present", have_net);
        w.kv("network_fragment", "e6_network_baselines.json");
        w.kv("passes_per_payload_byte", 2);
        w.kv("passes_note",
             "the data plane decrypts and re-encrypts, so each payload byte "
             "crosses the cipher twice; openssl speed crosses it once");
        w.key("openssl_speed").arr_open();
        for (const Ref& r : refs) {
            w.obj_open();
              w.kv("cipher", r.cipher);
              w.kv("ok", r.ok);
              w.kv("error", r.error);
              w.kv("cycles_per_byte", r.cycles_per_byte, 5);
              w.key("blocks").arr_open();
              for (const SpeedRow& s : r.rows) {
                  w.obj_open();
                    w.kv("bytes", (long long)s.bytes);
                    w.kv("thousand_bytes_per_sec", s.kbytes_per_sec, 1);
                  w.obj_close();
              }
              w.arr_close();
            w.obj_close();
        }
        w.arr_close();
        w.key("linerate").arr_open();
        for (const Ours& o : ours) {
            w.obj_open();
              w.kv("cipher", o.cipher);
              w.kv("fixed_cycles", o.a, 2);
              w.kv("cycles_per_byte", o.b, 5);
              w.kv("cycles_per_byte_per_pass", o.b / 2.0, 5);
              w.kv("crossover_bytes", o.b > 0 ? o.a / o.b : 0.0, 2);
              w.kv("n_points", (long long)o.x.size());
              w.kv_nums("payload_bytes", o.x, 0);
              w.kv_nums("cycles_per_packet", o.y, 3);
            w.obj_close();
        }
        w.arr_close();
      w.obj_close();
    w.obj_close();

    const std::string path = ctx.outdir + "/e6_baseline.json";
    if (!w.write_file(path)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    printf("  wrote %s%s\n", path.c_str(),
           have_net ? "" : " (network baselines not present; run cloud/baselines.sh)");
    return 0;
}

} // namespace lr::bench
