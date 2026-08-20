// bench.cpp — the scaffolding every experiment shares.
#include "bench.hpp"

#include "lr/clock.hpp"
#include "lr/platform.hpp"
#include "lr/proto.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace lr::bench {

// Payload grid. Denser at the small end, because that is where the fixed cost a
// dominates and where the crossover s* falls; the large end only needs enough
// points to pin the slope b. 64 and 1432 are the anchors quoted in the report:
// the smallest payload a real tunnel carries, and the largest that fits an
// Ethernet MTU once the header and the tag are accounted for.
const std::vector<size_t> kPayloads = {
    16, 32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1200, 1432
};

const std::vector<std::string> kCiphers = {
    "aes-256-gcm", "chacha20-poly1305", "lr-aes-256-gcm", "lr-chacha20-poly1305"
};

const std::vector<int> kBatchSizes = {1, 2, 4, 8, 16, 32, 64};

const uint8_t* bench_key() {
    // Fixed, published, and not a secret: a benchmark whose key changes between
    // runs is a benchmark whose runs are not comparable.
    static const uint8_t k[32] = {
        0x9f, 0x2c, 0x41, 0x08, 0xd3, 0x77, 0x5a, 0xbe,
        0x14, 0x60, 0xe9, 0x2b, 0x8c, 0x35, 0xf1, 0x07,
        0x53, 0xaa, 0x19, 0xc6, 0x7e, 0x02, 0xb8, 0x4d,
        0x91, 0x38, 0xdf, 0x65, 0x2a, 0xf4, 0x0b, 0x76
    };
    return k;
}

int window_for(size_t wire_len) {
    // Stage at most a receive buffer's worth. The kernel charges more than the
    // datagram length per skb; a factor of two is the usual rule of thumb and
    // errs on the side of staging fewer packets than the buffer can hold.
    const size_t rcvbuf = 16u << 20;
    int n = (int)(rcvbuf / (wire_len * 2 + 256));
    if (n > 8192) n = 8192;
    if (n < 32)   n = 32;
    return n;
}

bool measurement_permitted(std::string* why) {
    const auto& c = plat::caps();
    if (c.klass() == plat::HostClass::Development) {
        std::string r = "host class is 'development'";
        auto cav = c.caveats();
        if (!cav.empty()) r += " (" + cav.front() + ")";
        if (why) *why = r;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

std::string arg_str(int argc, char** argv, const std::string& name, const std::string& def) {
    const std::string pre = "--" + name + "=";
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], pre.c_str(), pre.size()) == 0) return argv[i] + pre.size();
    return def;
}

long arg_int(int argc, char** argv, const std::string& name, long def) {
    const std::string s = arg_str(argc, argv, name, "");
    return s.empty() ? def : strtol(s.c_str(), nullptr, 10);
}

bool arg_has(int argc, char** argv, const std::string& name) {
    const std::string flag = "--" + name;
    for (int i = 1; i < argc; ++i)
        if (flag == argv[i]) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Subprocesses
// ---------------------------------------------------------------------------

std::string capture(const std::string& cmd, int* rc) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { if (rc) *rc = -1; return out; }
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    int r = pclose(p);
    if (rc) *rc = (r == -1) ? -1 : (r / 256);
    return out;
}

std::string spawn_unit(const std::string& exe, const std::string& args, bool hw_crypto) {
    // The hardware-crypto mask is read by libcrypto when it loads, and by the
    // dispatcher in simd_crypto.c for the implementations written here. Both
    // read it from the environment, so the only way to vary the factor is
    // across an exec boundary. That constraint turns out to be a benefit: each
    // replicate is a fresh process, so the sweep can be interleaved freely
    // across the factor without a per-process ordering effect.
    std::string cmd;
    if (!hw_crypto) {
        cmd += std::string(plat::hw_crypto_mask_var()) + "=" +
               plat::hw_crypto_mask_value() + " LR_MASK_STATE=off ";
    } else {
        cmd += "LR_MASK_STATE=on ";
    }
    cmd += exe + " " + args + " 2>/dev/null";
    return capture(cmd, nullptr);
}

bool unit_field(const std::string& js, const std::string& key, double* out) {
    const std::string k = "\"" + key + "\"";
    size_t p = js.find(k);
    if (p == std::string::npos) return false;
    p = js.find(':', p + k.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < js.size() && (js[p] == ' ' || js[p] == '\t' || js[p] == '\n')) ++p;
    if (p < js.size() && js[p] == '"') return false;
    // Booleans read as 1 and 0. Several unit fields are flags, and a parser that
    // silently failed on them would make a failed unit indistinguishable from a
    // successful one.
    if (js.compare(p, 4, "true") == 0)  { *out = 1; return true; }
    if (js.compare(p, 5, "false") == 0) { *out = 0; return true; }
    char* end = nullptr;
    const double v = strtod(js.c_str() + p, &end);
    if (end == js.c_str() + p) return false;
    *out = v;
    return true;
}

bool unit_string(const std::string& js, const std::string& key, std::string* out) {
    const std::string k = "\"" + key + "\"";
    size_t p = js.find(k);
    if (p == std::string::npos) return false;
    p = js.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p = js.find('"', p);
    if (p == std::string::npos) return false;
    const size_t e = js.find('"', p + 1);
    if (e == std::string::npos) return false;
    *out = js.substr(p + 1, e - p - 1);
    return true;
}

// ---------------------------------------------------------------------------
// The hot path, staged locally
// ---------------------------------------------------------------------------

HotPathResult run_hotpath(const ShardConfig& cfg, int target, int warmup) {
    HotPathResult r;
    std::string err;
    Shard sh(cfg, bench_key(), &err);
    if (!err.empty()) { r.error = err; return r; }
    r.aead_backend = sh.aead_backend();

    const int window = window_for(sh.wire_len());

    // Warm-up cycles are discarded entirely: they populate the instruction
    // cache, the branch predictors, the page tables behind the arena and the
    // socket's own allocation, none of which is the steady-state cost of a
    // packet. Their cost is real but it is amortised over a run of any useful
    // length, so charging it to the per-packet term would misattribute it.
    for (int i = 0; i < warmup; ++i) {
        const int staged = sh.preload(window);
        if (staged <= 0) { r.error = "loader could not stage packets"; return r; }
        sh.drain(staged);
    }
    sh.reset_stats();

    int done = 0;
    int guard = 0;
    while (done < target) {
        const int want = std::min(window, target - done);
        const int staged = sh.preload(want);
        if (staged <= 0) {
            if (++guard > 8) break;
            std::this_thread::yield();
            continue;
        }
        guard = 0;
        // Everything above this line is untimed. Only drain() accumulates into
        // ticks_busy, so the loader never appears in the measurement.
        done += sh.drain(staged);
    }

    const ShardStats& st = sh.stats();
    if (st.packets == 0) { r.error = "no packets processed"; return r; }
    r.packets      = st.packets;
    r.auth_fail    = st.auth_fail;
    r.replay_drop  = st.replay_drop;
    r.cycles_per_packet = (double)st.ticks_busy / (double)st.packets;
    const double ns_per_pkt = clk::ticks_to_ns(st.ticks_busy) / (double)st.packets;
    r.pps               = ns_per_pkt > 0 ? 1e9 / ns_per_pkt : 0;
    r.syscalls_per_pkt  = (double)st.syscalls / (double)st.packets;
    r.source            = "loopback";
    r.ok                = true;
    return r;
}

// ---------------------------------------------------------------------------
// The hot path, fed across a physical interface
// ---------------------------------------------------------------------------

Peer::Peer(const RunCtx& ctx) {
    if (ctx.peer_host.empty()) { why_ = "no peer host configured"; return; }
    std::string err;
    if (!c_.connect(ctx.peer_host, ctx.peer_port, &err)) { why_ = err; return; }
    std::map<std::string, std::string> info;
    if (!c_.ping(&info, &err)) { why_ = err; return; }
    nic_    = info.count("nic")     ? info["nic"]     : "unknown";
    driver_ = info.count("driver")  ? info["driver"]  : "unknown";
    addr_   = info.count("addr")    ? info["addr"]    : "";
    echo_port_ = info.count("echo_port") ? (uint16_t)atoi(info["echo_port"].c_str()) : 0;
    ok_ = true;
}

Peer::~Peer() = default;

bool Peer::start(const ctrl::StartRequest& q, std::string* err) {
    if (!ok_) { if (err) *err = why_; return false; }
    return c_.start(q, err);
}

bool Peer::stop(ctrl::StopReply* rep, std::string* err) {
    if (!ok_) { if (err) *err = why_; return false; }
    return c_.stop(rep, err);
}

HotPathResult run_hotpath_wire(const RunCtx& ctx, Peer& peer, const ShardConfig& cfg_in,
                               int target, double seconds, double offered_pps) {
    HotPathResult r;
    if (!peer.enabled()) { r.error = "peer unavailable: " + peer.why(); return r; }

    ShardConfig cfg = cfg_in;
    cfg.source    = Source::Wire;
    cfg.bind_addr = ctx.local_addr.empty() ? "0.0.0.0" : ctx.local_addr;
    cfg.bind_port = 0;
    cfg.peer_addr = peer.remote_addr();

    std::string err;
    Shard sh(cfg, bench_key(), &err);
    if (!err.empty()) { r.error = err; return r; }
    r.aead_backend = sh.aead_backend();

    ctrl::StartRequest q;
    q.cipher   = cfg.cipher;
    q.payload  = cfg.payload;
    q.pps      = offered_pps;         // 0 means "as fast as you can"
    q.seconds  = seconds;
    q.dst_addr = ctx.local_addr;
    q.dst_port = sh.local_port();
    q.return_port   = cfg.peer_port ? cfg.peer_port : 0;
    q.epoch_packets = cfg.epoch_packets;
    if (!peer.start(q, &err)) { r.error = err; return r; }

    // Let the pipe fill before anything is charged to the measurement. Until
    // the first packets arrive, a drain would be timing an empty socket.
    const uint64_t settle = clk::ticks() + clk::ns_to_ticks(150e6);
    while (clk::ticks() < settle) sh.drain(4096);
    sh.reset_stats();

    const uint64_t deadline = clk::ticks() + clk::ns_to_ticks(seconds * 1e9);
    int done = 0;
    while (clk::ticks() < deadline && done < target) done += sh.drain(4096);

    ctrl::StopReply rep;
    peer.stop(&rep, &err);

    const ShardStats& st = sh.stats();
    if (st.packets == 0) {
        // The far node knows why it sent nothing; asking it is cheaper than
        // guessing here, and a unit that reports "no packets arrived" when the
        // real answer is "that cipher does not exist on the generator" sends
        // the next reader to look at the network.
        r.error = rep.fault.empty()
            ? "no packets arrived over the wire (generator sent " +
              std::to_string(rep.sent) + ")"
            : "the load generator could not run this unit: " + rep.fault;
        return r;
    }
    r.packets     = st.packets;
    r.auth_fail   = st.auth_fail;
    r.replay_drop = st.replay_drop;
    // Cost per packet *served*. The generator is deliberately overrunning the
    // receiver, so packets are lost at the far end and in the receive queue;
    // that is intended. What is being measured is the cost of serving a packet
    // once it has arrived, not the delivery ratio, and the ratio is recorded
    // separately so the report can state how hard the receiver was driven.
    r.cycles_per_packet = (double)st.ticks_busy / (double)st.packets;
    const double ns_per_pkt = clk::ticks_to_ns(st.ticks_busy) / (double)st.packets;
    r.pps              = ns_per_pkt > 0 ? 1e9 / ns_per_pkt : 0;
    r.syscalls_per_pkt = (double)st.syscalls / (double)st.packets;
    r.source           = "wire";
    r.ok               = true;
    return r;
}

} // namespace lr::bench
