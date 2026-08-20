// emit_json.cpp — the environment block and the "this could not run" fragment.
//
// Two rules are enforced here, and both exist so that a dataset is
// self-describing rather than dependent on an operator's notes.
//
//   1. Every fragment carries the environment that produced it. Merging two
//      fragments from different machines is then detectable rather than silent.
//   2. Every experiment writes a fragment even when it cannot run, with
//      available:false and a reason. The report then renders "not available on
//      this host, because X" instead of a missing figure, and check_wiring.py
//      can assert that the set of fragments is complete.
#include "bench.hpp"

#include "lr/aead.hpp"
#include "lr/clock.hpp"
#include "lr/platform.hpp"
#include "lr/transport.hpp"
#include "lr/worker.hpp"
#include "build_info.hpp"

#include <ctime>
#include <sys/stat.h>

namespace lr::bench {

namespace {
std::string iso8601_now() {
    std::time_t t = std::time(nullptr);
    std::tm gm{};
    gmtime_r(&t, &gm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gm);
    return buf;
}
} // namespace

void write_environment(json::Writer& w, const RunCtx& ctx) {
    const auto& c = plat::caps();
    w.key("environment").obj_open();
      w.kv("schema_version", 2);
      w.kv("timestamp_utc", iso8601_now());
      w.kv("linerate_version", LR_VERSION);
      w.kv("git_commit", LR_GIT_COMMIT);
      w.kv("compiler", LR_COMPILER);
      w.kv("cxx_flags", LR_CXX_FLAGS);
      w.kv("build_type", LR_BUILD_TYPE);
      w.kv("os", c.os);
      w.kv("arch", c.arch);
      w.kv("cpu_model", c.cpu_model);
      w.kv("logical_cpus", c.logical_cpus);
      w.kv("physical_cores", c.physical_cores);
      w.kv("smt_enabled", c.smt_enabled);
      w.kv("homogeneous_cores", c.homogeneous);
      w.kv("invariant_tsc", c.invariant_tsc);
      w.kv("hard_affinity", c.hard_affinity);
      w.kv("tsc_hz", (long long)c.tsc_hz);
      w.kv("nominal_cpu_hz", (long long)c.nominal_cpu_hz);
      w.kv("clock_source", c.clock_source);
      w.kv("governor", c.governor);
      w.kv("turbo_control", c.turbo_control);
      w.kv("irqbalance", c.irqbalance);
      w.kv("kernel_cmdline", c.kernel_cmdline);
      w.kv("isolcpus", c.isolcpus);
      w.kv("nohz_full", c.nohz_full);
      w.kv("rcu_nocbs", c.rcu_nocbs);
      w.kv("hw_aes", c.hw_aes);
      w.kv("hw_clmul", c.hw_clmul);
      w.kv("hw_avx2", c.hw_avx2);
      w.kv("openmp", c.openmp);
      w.kv("batched_io", c.batched_io);
      w.kv("uring_io", c.uring_io);
      w.kv("uring_sqpoll", c.uring_sqpoll);
      w.kv("openssl_version", c.openssl_version);
      w.kv("openssl_hw_aes_active", plat::openssl_hw_aes_active());
      w.kv("hw_crypto_masked", plat::hw_crypto_masked());
      w.kv("host_class", plat::host_class_name(c.klass()));
      w.kv("isolation_tier", c.isolation_tier);
      w.kv("nic_name", c.nic_name);
      w.kv("nic_driver", c.nic_driver);
      w.kv("nic_addr", c.nic_addr);
      w.kv_strs("cpu_flags", c.cpu_flags);
      w.kv_strs("caveats", c.caveats());
      w.kv_strs("transports", transport_backends());
      w.kv_strs("ciphers", aead_names());
      w.key("protocol").obj_open();
        w.kv("header_bytes", (long long)LR_HDR);
        w.kv("tag_bytes", (long long)LR_TAG);
        w.kv("max_payload_bytes", (long long)LR_MAX_PAYLOAD);
        w.kv("mtu_bytes", 1500);
        w.kv("ip_udp_overhead_bytes", 28);
      w.obj_close();
      w.key("run").obj_open();
        w.kv("replicates", ctx.reps);
        w.kv("cv_threshold", ctx.cv_threshold, 4);
        w.kv("seed", (long long)ctx.seed);
        w.kv("target_packets", ctx.target_packets);
        w.kv("warmup_cycles", ctx.warmup_cycles);
        w.kv("anchor_tolerance", ctx.anchor_tol, 4);
        w.kv("peer_host", ctx.peer_host);
        w.kv("local_addr", ctx.local_addr);
        w.kv("offer_factor", ctx.offer_factor, 3);
        w.kv("unit_seconds", ctx.unit_seconds, 3);
      w.obj_close();
    w.obj_close();
}

int write_unavailable(const RunCtx& ctx, const std::string& file,
                      const std::string& key, const std::string& reason) {
    ::mkdir(ctx.outdir.c_str(), 0755);
    json::Writer w;
    w.obj_open();
      write_environment(w, ctx);
      w.key(key).obj_open();
        w.kv("available", false);
        w.kv("reason", reason);
      w.obj_close();
    w.obj_close();
    const std::string path = ctx.outdir + "/" + file;
    if (!w.write_file(path)) {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }
    printf("  not available: %s\n  wrote %s\n", reason.c_str(), path.c_str());
    return 0;
}

int run_env(const RunCtx& ctx, int, char**) {
    ::mkdir(ctx.outdir.c_str(), 0755);
    json::Writer w;
    w.obj_open();
      write_environment(w, ctx);
    w.obj_close();
    const std::string path = ctx.outdir + "/environment.json";
    if (!w.write_file(path)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    printf("wrote %s\n", path.c_str());
    return 0;
}

} // namespace lr::bench
