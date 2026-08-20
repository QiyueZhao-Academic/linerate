// exp_e7_virtualization.cpp — what the isolation boundary costs.
//
// The same data plane is measured inside four increasingly heavy isolation
// tiers, and then chained with copies of itself to measure what composing
// virtual network functions costs per hop.
//
//   host       the VM's own kernel and the gVNIC interface. The reference.
//   netns      a network namespace joined by a veth pair. Namespaces are free
//              in principle — same kernel, same scheduler — so whatever this
//              costs is the veth path and the extra forwarding decision.
//   container  runc under Docker, with the default bridge and its NAT. Same
//              kernel again, plus a bridge, plus netfilter.
//   gvisor     runsc: system calls are serviced by a user-space kernel. This is
//              where the fixed per-packet cost a should move most, because
//              every syscall in the hot path now crosses into another process
//              rather than into the kernel.
//
// True bare metal is not obtainable on a public cloud, and this is stated as a
// limitation rather than papered over: the 'host' tier is itself a guest of the
// hypervisor, so what is measured is the *incremental* cost of each additional
// isolation layer above the VM, not the absolute cost of virtualisation.
//
// The chaining half builds a service function chain of length 1 to 4 out of veth
// pairs, with a copy of the data plane in each namespace, and measures the
// per-hop cost. Each hop terminates one security association and originates
// another, which is what a chain of encrypting VNFs actually does.
//
// Neither half runs from inside this process: entering a namespace or starting a
// container needs privileges the harness must not assume, and the tiers must be
// entered before the process starts, not during it. cloud/virtualization.sh and
// cloud/sfc.sh place a copy of this binary inside each tier and collect the
// per-tier unit results; this experiment reads them back, checks that they came
// from the same environment, and assembles the fragment. Run standalone, it
// measures the tier it is currently in, which is what those scripts invoke.
#include "bench.hpp"

#include "lr/clock.hpp"
#include "lr/platform.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace lr::bench {

namespace {

struct TierResult {
    std::string tier;
    std::string nic_driver;
    double cycles_median = 0;
    double cycles_cv = 0;
    double pps = 0;
    double syscalls_per_packet = 0;
    int    chain_length = 1;
    bool   ok = false;
    std::string error;
};

// Read a unit file written by a copy of this binary running inside a tier.
bool read_tier_file(const std::string& path, TierResult* t) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string js;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) js.append(buf, n);
    fclose(f);

    double v = 0;
    if (!unit_string(js, "tier", &t->tier)) return false;
    unit_string(js, "nic_driver", &t->nic_driver);
    unit_string(js, "error", &t->error);
    if (unit_field(js, "cycles_median", &v)) t->cycles_median = v;
    if (unit_field(js, "cycles_cv", &v))     t->cycles_cv = v;
    if (unit_field(js, "pps", &v))           t->pps = v;
    if (unit_field(js, "syscalls_per_packet", &v)) t->syscalls_per_packet = v;
    if (unit_field(js, "chain_length", &v))  t->chain_length = (int)v;
    if (unit_field(js, "ok", &v))            t->ok = v != 0;
    return true;
}

} // namespace

// The unit: measure the tier this process is currently inside, and print one
// flat object. cloud/virtualization.sh runs this once per tier, redirecting
// stdout into results/raw/tiers/<tier>.json.
int run_virt_unit(const RunCtx& ctx, int argc, char** argv) {
    const std::string cipher = arg_str(argc, argv, "cipher", "aes-256-gcm");
    const size_t payload = (size_t)arg_int(argc, argv, "payload", 512);
    const int reps = (int)arg_int(argc, argv, "reps", 5);
    const int packets = (int)arg_int(argc, argv, "packets", 20000);
    const int chain = (int)arg_int(argc, argv, "chain", 1);

    std::vector<double> cyc, pps, sc;
    std::string err;
    for (int i = 0; i < reps; ++i) {
        ShardConfig cfg;
        cfg.cipher  = cipher;
        cfg.payload = payload;
        HotPathResult h = run_hotpath(cfg, packets, ctx.warmup_cycles);
        if (!h.ok) { err = h.error; break; }
        cyc.push_back(h.cycles_per_packet);
        pps.push_back(h.pps);
        sc.push_back(h.syscalls_per_pkt);
    }

    stats::Summary s;
    if (!cyc.empty()) s = stats::summarise(cyc, ctx.cv_threshold, ctx.seed);

    json::Writer w;
    w.obj_open();
      w.kv("ok", !cyc.empty());
      w.kv("tier", plat::caps().isolation_tier);
      w.kv("nic_driver", plat::caps().nic_driver);
      w.kv("chain_length", chain);
      w.kv("cipher", cipher);
      w.kv("payload_bytes", (long long)payload);
      w.kv("cycles_median", s.median, 3);
      w.kv("cycles_cv", s.cv, 5);
      w.kv("pps", pps.empty() ? 0.0 : stats::median(pps), 1);
      w.kv("syscalls_per_packet", sc.empty() ? 0.0 : stats::median(sc), 4);
      w.kv("error", err);
    w.obj_close();
    printf("%s\n", w.str().c_str());
    return cyc.empty() ? 1 : 0;
}

int run_e7(const RunCtx& ctx, int argc, char** argv) {
    printf("E7 virtualisation and service chaining\n");
    std::string why;
    if (!measurement_permitted(&why))
        return write_unavailable(ctx, "e7_virtualization.json", "e7", why);

    // Tier results are dropped here by cloud/virtualization.sh, one file per
    // tier, and here by cloud/sfc.sh, one per chain length.
    const std::string dir = ctx.outdir + "/tiers";
    std::vector<TierResult> tiers, chain;

    DIR* d = opendir(dir.c_str());
    if (d) {
        dirent* e;
        while ((e = readdir(d)) != nullptr) {
            const std::string name = e->d_name;
            if (name.size() < 6 || name.substr(name.size() - 5) != ".json") continue;
            TierResult t;
            if (!read_tier_file(dir + "/" + name, &t)) continue;
            if (name.rfind("chain", 0) == 0) chain.push_back(t);
            else                             tiers.push_back(t);
        }
        closedir(d);
    }

    // Always include the tier this process is in, measured now. If the scripts
    // were never run, the fragment still carries one honest row rather than
    // being empty.
    {
        TierResult here;
        here.tier = plat::caps().isolation_tier;
        here.nic_driver = plat::caps().nic_driver;
        const bool already = std::any_of(tiers.begin(), tiers.end(),
            [&](const TierResult& t) { return t.tier == here.tier; });
        if (!already) {
            std::vector<double> cyc, pps, sc;
            for (int i = 0; i < std::max(3, ctx.reps / 2); ++i) {
                ShardConfig cfg;
                cfg.cipher = "aes-256-gcm";
                cfg.payload = 512;
                HotPathResult h = run_hotpath(cfg,
                    arg_has(argc, argv, "quick") ? 4000 : ctx.target_packets,
                    ctx.warmup_cycles);
                if (!h.ok) { here.error = h.error; break; }
                cyc.push_back(h.cycles_per_packet);
                pps.push_back(h.pps);
                sc.push_back(h.syscalls_per_pkt);
            }
            if (!cyc.empty()) {
                stats::Summary s = stats::summarise(cyc, ctx.cv_threshold, ctx.seed);
                here.cycles_median = s.median;
                here.cycles_cv = s.cv;
                here.pps = stats::median(pps);
                here.syscalls_per_packet = stats::median(sc);
                here.ok = true;
            }
            tiers.push_back(here);
        }
    }

    std::sort(tiers.begin(), tiers.end(), [](const TierResult& a, const TierResult& b) {
        auto rank = [](const std::string& t) {
            if (t == "host") return 0;
            if (t == "netns") return 1;
            if (t == "container") return 2;
            if (t == "gvisor") return 3;
            return 4;
        };
        return rank(a.tier) < rank(b.tier);
    });
    std::sort(chain.begin(), chain.end(), [](const TierResult& a, const TierResult& b) {
        return a.chain_length < b.chain_length;
    });

    // Relative cost against the host tier, which is the only comparison that
    // means anything: the absolute cycle count depends on the machine.
    double host_cycles = 0;
    for (const TierResult& t : tiers)
        if (t.tier == "host" && t.ok) host_cycles = t.cycles_median;

    // Per-hop cost of the chain, from the slope of cost against chain length.
    double per_hop = 0;
    if (chain.size() >= 2) {
        const TierResult& a = chain.front();
        const TierResult& b = chain.back();
        if (b.chain_length > a.chain_length)
            per_hop = (b.cycles_median - a.cycles_median) /
                      (double)(b.chain_length - a.chain_length);
    }

    ::mkdir(ctx.outdir.c_str(), 0755);
    json::Writer w;
    w.obj_open();
      write_environment(w, ctx);
      w.key("e7").obj_open();
        w.kv("available", true);
        w.kv("tiers_measured", (long long)tiers.size());
        w.kv("chain_points", (long long)chain.size());
        w.kv("host_cycles", host_cycles, 2);
        w.kv("chain_cycles_per_hop", per_hop, 2);
        w.kv("bare_metal_available", false);
        w.kv("bare_metal_note",
             "the 'host' tier is itself a hypervisor guest, so what is reported "
             "is the incremental cost of each isolation layer above the VM, not "
             "the absolute cost of virtualisation");
        w.key("tiers").arr_open();
        for (const TierResult& t : tiers) {
            w.obj_open();
              w.kv("tier", t.tier);
              w.kv("nic_driver", t.nic_driver);
              w.kv("cycles_median", t.cycles_median, 3);
              w.kv("cycles_cv", t.cycles_cv, 5);
              w.kv("pps", t.pps, 1);
              w.kv("syscalls_per_packet", t.syscalls_per_packet, 4);
              w.kv("relative_to_host",
                   host_cycles > 0 ? t.cycles_median / host_cycles : 0.0, 4);
              w.kv("ok", t.ok);
              w.kv("error", t.error);
            w.obj_close();
        }
        w.arr_close();
        w.key("chain").arr_open();
        for (const TierResult& t : chain) {
            w.obj_open();
              w.kv("chain_length", t.chain_length);
              w.kv("cycles_median", t.cycles_median, 3);
              w.kv("cycles_cv", t.cycles_cv, 5);
              w.kv("pps", t.pps, 1);
              w.kv("ok", t.ok);
              w.kv("error", t.error);
            w.obj_close();
        }
        w.arr_close();
      w.obj_close();
    w.obj_close();

    const std::string path = ctx.outdir + "/e7_virtualization.json";
    if (!w.write_file(path)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    printf("  wrote %s (%zu tiers, %zu chain points)\n",
           path.c_str(), tiers.size(), chain.size());
    return 0;
}

} // namespace lr::bench
