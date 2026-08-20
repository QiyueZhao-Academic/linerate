// bench_main.cpp — the dispatcher.
//
// One binary, one subcommand per experiment, plus the hidden unit subcommands
// the sweep re-spawns itself with to vary the hardware-crypto factor across an
// exec boundary.
#include "bench.hpp"

#include "lr/platform.hpp"
#include "build_info.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace lr;
using namespace lr::bench;

namespace {

void usage() {
    printf(
"lr_bench <experiment> [options]\n"
"\n"
"experiments\n"
"  env         write the environment block alone\n"
"  e1          cost model: cycles per packet against payload size, four ciphers,\n"
"              hardware crypto present and masked. Fits C(s) = a + b*s and s*.\n"
"  e2          scaling: 1..N shards, OpenMP worker pool, strong and weak scaling\n"
"  e3          I/O path: posix / mmsg / uring / uring-sqpoll against batch depth\n"
"  e4          memory roofline: working set against per-packet cost\n"
"  e5          latency under load: offered rate against p50/p99/p99.9, with netem\n"
"  e6          baselines: iperf3, WireGuard, openssl speed, this data plane\n"
"  e7          virtualisation: host / netns / container / gVisor, and a chain\n"
"  controls    negative and positive controls, and the anchor\n"
"  all         e1 e2 e3 e4 e5 controls, in that order\n"
"\n"
"options\n"
"  --out=DIR             fragment directory            (results/raw)\n"
"  --reps=N              replicates per point          (11)\n"
"  --packets=N           packets per replicate         (30000)\n"
"  --warmup=N            discarded cycles per point    (3)\n"
"  --cv=F                stability threshold           (0.10)\n"
"  --seed=N              bootstrap seed                (20260819)\n"
"  --peer=HOST           load generator host; enables wire mode\n"
"  --peer-port=N         control port                  (9099)\n"
"  --local=ADDR          this node's address on the measured link\n"
"  --seconds=F           wall time of one wire replicate (1.2)\n"
"  --offer=F             offered load as a multiple of capacity (3.0)\n"
"  --quick               a reduced grid, for a smoke test\n"
"  --verbose             per-point progress\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        usage();
        return argc < 2 ? 64 : 0;
    }
    const std::string cmd = argv[1];

    // Hidden: a single unit of work, run in a fresh process so that the
    // hardware-crypto mask can be varied. Prints one flat JSON object on stdout
    // and is never called by hand.
    if (cmd == "e1-unit")       return run_e1_unit(argc, argv);
    if (cmd == "controls-unit") return run_controls_unit(argc, argv);

    RunCtx ctx;
    ctx.exe            = argv[0];
    ctx.outdir         = arg_str(argc, argv, "out", ctx.outdir);
    ctx.reps           = (int)arg_int(argc, argv, "reps", ctx.reps);
    ctx.target_packets = (int)arg_int(argc, argv, "packets", ctx.target_packets);
    ctx.warmup_cycles  = (int)arg_int(argc, argv, "warmup", ctx.warmup_cycles);
    ctx.seed           = (uint64_t)arg_int(argc, argv, "seed", (long)ctx.seed);
    ctx.peer_host      = arg_str(argc, argv, "peer", "");
    ctx.peer_port      = (uint16_t)arg_int(argc, argv, "peer-port", ctx.peer_port);
    ctx.local_addr     = arg_str(argc, argv, "local", "");
    ctx.verbose        = arg_has(argc, argv, "verbose");
    {
        const std::string cv = arg_str(argc, argv, "cv", "");
        if (!cv.empty()) ctx.cv_threshold = atof(cv.c_str());
        const std::string se = arg_str(argc, argv, "seconds", "");
        if (!se.empty()) ctx.unit_seconds = atof(se.c_str());
        const std::string of = arg_str(argc, argv, "offer", "");
        if (!of.empty()) ctx.offer_factor = atof(of.c_str());
    }
    if (arg_has(argc, argv, "quick")) {
        // Enough to exercise every code path end to end and produce a dataset
        // the analysis accepts, in about a minute. Not enough for a result, and
        // the fragment records that so no figure can be built from it by
        // accident.
        ctx.reps = 3;
        ctx.target_packets = 4000;
        ctx.warmup_cycles = 1;
        ctx.unit_seconds = 0.4;
    }
    // If no local address was given but the host has one interface, use it:
    // an operator should not have to type an address the machine already knows.
    if (ctx.local_addr.empty()) {
        std::string n, a, d;
        if (plat::primary_interface(&n, &a, &d)) ctx.local_addr = a;
    }

    if (cmd == "env")      return run_env(ctx, argc, argv);
    if (cmd == "e1")       return run_e1(ctx, argc, argv);
    if (cmd == "e2")       return run_e2(ctx, argc, argv);
    if (cmd == "e3")       return run_e3(ctx, argc, argv);
    if (cmd == "e4")       return run_e4(ctx, argc, argv);
    if (cmd == "e5")       return run_e5(ctx, argc, argv);
    if (cmd == "e6")       return run_e6(ctx, argc, argv);
    if (cmd == "e7")       return run_e7(ctx, argc, argv);
    if (cmd == "controls") return run_controls(ctx, argc, argv);
    // Hidden: measure the isolation tier this process is inside. cloud scripts
    // place a copy of this binary in each tier and run exactly this.
    if (cmd == "virt-unit") return run_virt_unit(ctx, argc, argv);
    if (cmd == "all") {
        int rc = 0;
        rc |= run_env(ctx, argc, argv);
        rc |= run_e1(ctx, argc, argv);
        rc |= run_e2(ctx, argc, argv);
        rc |= run_e3(ctx, argc, argv);
        rc |= run_e4(ctx, argc, argv);
        rc |= run_e5(ctx, argc, argv);
        rc |= run_controls(ctx, argc, argv);
        return rc;
    }

    fprintf(stderr, "unknown experiment '%s'\n\n", cmd.c_str());
    usage();
    return 64;
}
