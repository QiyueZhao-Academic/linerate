// bench.hpp — scaffolding shared by the experiments.
//
// Everything here exists to make the statistical discipline in
// docs/methodology.md a property of the code rather than a habit of the
// operator: replicate counts, warm-up discards, interleaved sweep order, the
// stability gate and the anchor check are implemented once and used by all
// experiments.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lr/ctrl.hpp"
#include "lr/json.hpp"
#include "lr/stats.hpp"
#include "lr/worker.hpp"

namespace lr::bench {

struct RunCtx {
    std::string outdir      = "results/raw";
    std::string exe;                 // argv[0], used to re-spawn masked units
    int      reps           = 11;    // odd, so the median is an observed value
    double   cv_threshold   = 0.10;  // above this a point is recorded as unstable
    uint64_t seed           = 20260819;
    double   anchor_tol     = 0.10;  // sweep is void if the anchor moves more
    int      target_packets = 30000; // per replicate, after warm-up
    int      warmup_cycles  = 3;     // preload/drain cycles discarded first
    bool     verbose        = false;

    // Wire mode. Empty peer host means no second node is available and the
    // experiments fall back to staging packets over loopback, which is recorded
    // in the dataset rather than assumed away.
    std::string peer_host;
    uint16_t    peer_port   = ctrl::kDefaultPort;
    std::string local_addr;          // this node's address on the measured link
    double      offer_factor = 3.0;  // generator rate as a multiple of capacity
    double      unit_seconds = 1.2;  // wall time of one wire replicate
};

// The fixed factor levels of the study. Declared once so that the sweep, the
// schema and the report cannot disagree about what was measured.
extern const std::vector<size_t>      kPayloads;   // plaintext bytes
extern const std::vector<std::string> kCiphers;
extern const std::vector<int>         kBatchSizes;

// One replicate of the hot path at a fixed configuration.
struct HotPathResult {
    double   cycles_per_packet = 0;
    double   pps               = 0;   // implied single-core capacity
    double   syscalls_per_pkt  = 0;
    uint64_t packets           = 0;
    uint64_t auth_fail         = 0;
    uint64_t replay_drop       = 0;
    std::string aead_backend;
    std::string source = "loopback";  // "loopback" | "wire"
    bool     ok                = false;
    std::string error;
};

// The connection to the load generator on the other node, held open for a whole
// sweep. Constructing one when ctx.peer_host is empty leaves it disabled, and
// every call reports that rather than failing.
class Peer {
public:
    explicit Peer(const RunCtx& ctx);
    ~Peer();
    bool enabled() const { return ok_; }
    const std::string& why() const { return why_; }
    const std::string& remote_nic() const { return nic_; }
    const std::string& remote_driver() const { return driver_; }
    const std::string& remote_addr() const { return addr_; }
    uint16_t echo_port() const { return echo_port_; }

    bool start(const ctrl::StartRequest& q, std::string* err);
    bool stop(ctrl::StopReply* rep, std::string* err);

private:
    ctrl::Client c_;
    bool ok_ = false;
    std::string why_, nic_, driver_, addr_;
    uint16_t echo_port_ = 0;
};

// Preload/drain a shard until `target` packets have been processed, discarding
// `warmup` cycles first. The timed region contains only the hot path: staging
// packets into the receive queue happens between timed windows, so the loader's
// cost is never charged to the data plane.
HotPathResult run_hotpath(const ShardConfig& cfg, int target, int warmup);

// The same measurement, but with the packets arriving from the other node over
// a real interface. The generator is asked for enough offered load that the
// receive queue never empties, so what is timed is the service of a packet and
// not an idle poll. Loss at the far end is expected and is not an error: this
// measures cost per packet served, not delivery.
HotPathResult run_hotpath_wire(const RunCtx& ctx, Peer& peer, const ShardConfig& cfg,
                               int target, double seconds, double offered_pps);

// Number of packets to stage per cycle, chosen so that a full window fits in the
// socket receive buffer. The value self-corrects after the first cycle.
int window_for(size_t wire_len);

// Environment block, written once per run and carried with every dataset.
void write_environment(json::Writer& w, const RunCtx& ctx);

// True when the host may write a measurement at all.
bool measurement_permitted(std::string* why);

// Deterministic 256-bit secret. Fixed so two runs are comparable and a result is
// reproducible; this is a benchmark, not a deployment.
const uint8_t* bench_key();

// Spawn `exe subcmd args...` with the hardware-crypto mask applied or not, and
// return the child's stdout. The mask is an environment variable read by
// libcrypto at load time, so it can only be applied across an exec boundary,
// which is also what makes true interleaving across the hardware-crypto factor
// possible: each replicate is a fresh process and the sweep order is free.
std::string spawn_unit(const std::string& exe, const std::string& args, bool hw_crypto);

// Run a shell command and return its standard output. Used by the experiments
// that drive an external tool rather than a socket.
std::string capture(const std::string& cmd, int* rc = nullptr);

// Parse "key": value out of a flat JSON object emitted by a unit process.
bool unit_field(const std::string& js, const std::string& key, double* out);
bool unit_string(const std::string& js, const std::string& key, std::string* out);

// Argument helpers.
std::string arg_str(int argc, char** argv, const std::string& name, const std::string& def);
long        arg_int(int argc, char** argv, const std::string& name, long def);
bool        arg_has(int argc, char** argv, const std::string& name);

// Write a fragment that records an experiment could not run, so that a dataset
// is always complete and the reason is in the data rather than in a log.
int write_unavailable(const RunCtx& ctx, const std::string& file,
                      const std::string& key, const std::string& reason);

// Entry points, one per experiment.
int run_e1(const RunCtx&, int argc, char** argv);
int run_e1_unit(int argc, char** argv);
int run_e2(const RunCtx&, int argc, char** argv);
int run_e3(const RunCtx&, int argc, char** argv);
int run_e4(const RunCtx&, int argc, char** argv);
int run_e5(const RunCtx&, int argc, char** argv);
int run_e6(const RunCtx&, int argc, char** argv);
int run_e7(const RunCtx&, int argc, char** argv);
int run_controls(const RunCtx&, int argc, char** argv);
int run_controls_unit(int argc, char** argv);
int run_virt_unit(const RunCtx&, int argc, char** argv);
int run_env(const RunCtx&, int argc, char** argv);

} // namespace lr::bench
