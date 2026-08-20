// lr_mpi_scale.cpp — the cross-node half of E2.
//
// E2 measures scaling inside one machine, where adding a worker means sharing a
// memory system. This measures scaling across two, where adding a worker means
// crossing a network, and the two curves answer different questions:
//
//   inside a node   what does contention for cache and memory bandwidth cost?
//   across nodes    what does coordination cost, and does the per-packet cost
//                   of the data plane itself stay put when the ranks are on
//                   different hosts?
//
// Every rank runs the same shard the rest of the study measures, so a per-packet
// cost from here is directly comparable with one from E1. MPI is used for what
// it is good at and nothing else: starting the ranks, holding them at a barrier
// so the timed region opens at the same instant everywhere, and reducing the
// results. It carries no packets. Putting the data plane's traffic through MPI
// would measure MPI's transport rather than the one under study.
//
// The coordination cost is measured separately, as the time a barrier and an
// allreduce take with no work between them, so the report can state how much of
// any scaling shortfall is coordination rather than the data plane.
#include <mpi.h>

#include "lr/clock.hpp"
#include "lr/platform.hpp"
#include "lr/proto.hpp"
#include "lr/stats.hpp"
#include "lr/json.hpp"
#include "lr/worker.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace lr;

namespace {

std::string opt(int argc, char** argv, const char* name, const char* def) {
    const std::string pre = std::string("--") + name + "=";
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], pre.c_str(), pre.size()) == 0) return argv[i] + pre.size();
    return def;
}

struct Local {
    double cycles_per_packet = 0;
    double wall_ns = 0;
    long long packets = 0;
};

// One rank's share: build a shard, warm it, wait at the barrier, then process
// `packets` and report. The barrier is what makes the wall time comparable
// across ranks; without it the first rank to arrive measures an idle network.
Local rank_work(const std::string& cipher, size_t payload, int packets,
                int warmup, int cpu, std::string* err) {
    Local L;
    ShardConfig cfg;
    cfg.cipher  = cipher;
    cfg.payload = payload;
    cfg.cpu     = cpu;
    Shard sh(cfg, proto::bench_psk(), err);
    if (!err->empty()) return L;

    const int window = std::min(2048, std::max(64, packets / 4));
    for (int i = 0; i < warmup; ++i) {
        const int staged = sh.preload(window);
        if (staged > 0) sh.drain(staged);
    }
    sh.reset_stats();

    MPI_Barrier(MPI_COMM_WORLD);
    const uint64_t t0 = clk::ticks();
    int done = 0, guard = 0;
    while (done < packets) {
        const int staged = sh.preload(std::min(window, packets - done));
        if (staged <= 0) { if (++guard > 8) break; continue; }
        guard = 0;
        done += sh.drain(staged);
    }
    const uint64_t t1 = clk::ticks();

    const ShardStats& st = sh.stats();
    L.packets = (long long)st.packets;
    L.cycles_per_packet = st.packets ? (double)st.ticks_busy / (double)st.packets : 0;
    L.wall_ns = clk::ticks_to_ns(t1 - t0);
    return L;
}

// Cost of the coordination alone, with no data-plane work between the calls.
double coordination_ns(int iters) {
    double dummy = 1.0, sum = 0;
    MPI_Barrier(MPI_COMM_WORLD);
    const uint64_t t0 = clk::ticks();
    for (int i = 0; i < iters; ++i) {
        MPI_Barrier(MPI_COMM_WORLD);
        MPI_Allreduce(&dummy, &sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    }
    const uint64_t t1 = clk::ticks();
    return clk::ticks_to_ns(t1 - t0) / iters;
}

} // namespace

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    const std::string cipher = opt(argc, argv, "cipher", "aes-256-gcm");
    const size_t payload = (size_t)atol(opt(argc, argv, "payload", "512").c_str());
    const int packets = atoi(opt(argc, argv, "packets", "40000").c_str());
    const int warmup  = atoi(opt(argc, argv, "warmup", "3").c_str());
    const int reps    = atoi(opt(argc, argv, "reps", "5").c_str());
    const std::string outdir = opt(argc, argv, "out", "results/raw");

    char host[256] = {0};
    int hlen = 0;
    MPI_Get_processor_name(host, &hlen);

    // How many distinct hosts the ranks are spread over. A "two-node" scaling
    // result measured with both ranks on one node is a common and invisible
    // error, so the count is measured and recorded rather than assumed.
    std::vector<char> allhosts((size_t)size * MPI_MAX_PROCESSOR_NAME, 0);
    MPI_Allgather(host, MPI_MAX_PROCESSOR_NAME, MPI_CHAR,
                  allhosts.data(), MPI_MAX_PROCESSOR_NAME, MPI_CHAR, MPI_COMM_WORLD);
    std::vector<std::string> hostnames;
    for (int i = 0; i < size; ++i) {
        std::string h(&allhosts[(size_t)i * MPI_MAX_PROCESSOR_NAME]);
        if (std::find(hostnames.begin(), hostnames.end(), h) == hostnames.end())
            hostnames.push_back(h);
    }
    const int distinct_hosts = (int)hostnames.size();

    const int cpu = plat::caps().hard_affinity
        ? rank % std::max(1, plat::caps().physical_cores) : -1;

    // Strong: the total is fixed, so each rank does 1/size of it.
    // Weak: each rank does the same amount whatever size is.
    std::vector<double> strong_wall, weak_wall, strong_cyc, weak_cyc;
    std::string err;
    for (int r = 0; r < reps; ++r) {
        Local s = rank_work(cipher, payload, std::max(1000, packets / size),
                            warmup, cpu, &err);
        if (!err.empty()) break;
        Local wk = rank_work(cipher, payload, packets, warmup, cpu, &err);
        if (!err.empty()) break;

        // The slowest rank sets the wall time of the parallel region; averaging
        // would understate exactly the effect a scaling study is looking for.
        double sw = 0, ww = 0, sc = 0, wc = 0;
        MPI_Allreduce(&s.wall_ns,  &sw, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
        MPI_Allreduce(&wk.wall_ns, &ww, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
        MPI_Allreduce(&s.cycles_per_packet,  &sc, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        MPI_Allreduce(&wk.cycles_per_packet, &wc, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        strong_wall.push_back(sw);
        weak_wall.push_back(ww);
        strong_cyc.push_back(sc / size);
        weak_cyc.push_back(wc / size);
    }

    int any_error = err.empty() ? 0 : 1;
    int all_error = 0;
    MPI_Allreduce(&any_error, &all_error, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

    const double coord_ns = coordination_ns(200);

    if (rank == 0) {
        ::mkdir(outdir.c_str(), 0755);
        json::Writer w;
        w.obj_open();
          w.key("e2_mpi").obj_open();
            w.kv("available", all_error == 0 && !strong_wall.empty());
            w.kv("reason", all_error ? ("a rank failed: " + err) : std::string(""));
            w.kv("ranks", size);
            w.kv("distinct_hosts", distinct_hosts);
            w.kv("cross_node", distinct_hosts > 1);
            w.kv_strs("hosts", hostnames);
            w.kv("cipher", cipher);
            w.kv("payload_bytes", (long long)payload);
            w.kv("packets_total_strong", (long long)packets);
            w.kv("packets_per_rank_weak", (long long)packets);
            w.kv("replicates", reps);
            w.kv("strong_wall_ns", strong_wall.empty() ? 0.0 : stats::median(strong_wall), 1);
            w.kv("weak_wall_ns",   weak_wall.empty()   ? 0.0 : stats::median(weak_wall), 1);
            w.kv("cycles_per_packet_strong",
                 strong_cyc.empty() ? 0.0 : stats::median(strong_cyc), 3);
            w.kv("cycles_per_packet_weak",
                 weak_cyc.empty() ? 0.0 : stats::median(weak_cyc), 3);
            w.kv("coordination_ns_per_iteration", coord_ns, 1);
            w.kv("coordination_note",
                 "one barrier and one allreduce with no work between them; the "
                 "data plane's packets never travel through MPI");
            w.kv("arch", plat::caps().arch);
            w.kv("nic_driver", plat::caps().nic_driver);
          w.obj_close();
        w.obj_close();
        const std::string path = outdir + "/e2_mpi_scaling.json";
        if (w.write_file(path))
            printf("lr_mpi_scale: %d ranks over %d host(s), wrote %s\n",
                   size, distinct_hosts, path.c_str());
        else
            fprintf(stderr, "lr_mpi_scale: cannot write %s\n", path.c_str());
    }

    MPI_Finalize();
    return all_error;
}
