// platform.cpp — capability detection and host classification.
#include "lr/platform.hpp"
#include "lr/clock.hpp"
#include "lr/transport.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace lr::plat {

const char* host_class_name(HostClass c) {
    switch (c) {
        case HostClass::Measurement: return "measurement";
        case HostClass::Constrained: return "constrained";
        default:                     return "development";
    }
}

HostClass host_class_from_name(const std::string& s) {
    if (s == "measurement") return HostClass::Measurement;
    if (s == "constrained") return HostClass::Constrained;
    return HostClass::Development;
}

namespace {

std::string slurp(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

bool exists(const char* p) { struct stat st; return ::stat(p, &st) == 0; }

#if defined(__linux__)
// Parse /proc/cpuinfo once for model name, flags, and the physical-core count.
void read_cpuinfo(Caps& c) {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    std::set<std::string> core_ids;   // "physical_id:core_id" pairs
    std::string phys, core;
    int logical = 0;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            if (!phys.empty() && !core.empty()) core_ids.insert(phys + ":" + core);
            phys.clear(); core.clear();
            continue;
        }
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t");
            size_t b = s.find_last_not_of(" \t");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(k); trim(v);
        if (k == "processor")            logical++;
        else if (k == "model name" && c.cpu_model.empty()) c.cpu_model = v;
        else if (k == "Processor"  && c.cpu_model.empty()) c.cpu_model = v;
        else if (k == "physical id")     phys = v;
        else if (k == "core id")         core = v;
        else if ((k == "flags" || k == "Features") && c.cpu_flags.empty()) {
            std::istringstream fs(v);
            std::string flag;
            // Only the flags this study depends on are recorded. A full dump of
            // 200 flags is noise in a results file that has to stay readable.
            static const std::set<std::string> wanted = {
                "aes", "pclmulqdq", "vaes", "vpclmulqdq", "avx", "avx2",
                "avx512f", "constant_tsc", "nonstop_tsc", "tsc_known_freq", "rdtscp",
                "pmull", "sha2", "asimd", "neon"};
            while (fs >> flag) if (wanted.count(flag)) c.cpu_flags.push_back(flag);
        }
    }
    if (!phys.empty() && !core.empty()) core_ids.insert(phys + ":" + core);
    c.logical_cpus   = logical;
    c.physical_cores = core_ids.empty() ? logical : (int)core_ids.size();
    c.smt_enabled    = c.physical_cores > 0 && c.logical_cpus > c.physical_cores;
    auto has = [&](const char* f) {
        return std::find(c.cpu_flags.begin(), c.cpu_flags.end(), f) != c.cpu_flags.end();
    };
    c.invariant_tsc = has("constant_tsc") && has("nonstop_tsc");
    c.hw_aes        = has("aes");
    c.hw_clmul      = has("pclmulqdq") || has("pmull");
    c.hw_avx2       = has("avx2");
#if defined(__aarch64__)
    c.invariant_tsc = true;   // CNTVCT_EL0 is architecturally invariant
    c.hw_aes        = c.hw_aes   || has("aes");
    c.hw_clmul      = c.hw_clmul || has("pmull");
#endif
}

void read_hygiene(Caps& c) {
    c.kernel_cmdline = slurp("/proc/cmdline");
    c.isolcpus  = contains(c.kernel_cmdline, "isolcpus=");
    c.nohz_full = contains(c.kernel_cmdline, "nohz_full=");
    c.rcu_nocbs = contains(c.kernel_cmdline, "rcu_nocbs=");

    std::string gov = slurp("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    c.governor = gov.empty() ? "unavailable-in-guest" : gov;

    std::string turbo = slurp("/sys/devices/system/cpu/intel_pstate/no_turbo");
    if (turbo.empty()) turbo = slurp("/sys/devices/system/cpu/cpufreq/boost");
    c.turbo_control = turbo.empty() ? "unavailable-in-guest" : ("writable:" + turbo);

    // irqbalance state without shelling out: its pidfile is the cheapest signal
    // that survives in a minimal image where systemctl may not exist.
    c.irqbalance = exists("/run/irqbalance.pid") ? "running" : "not-running";
}

// Which sandbox this process is inside. E7 varies exactly this, so a number
// that does not carry it is not comparable with one that does.
std::string read_isolation_tier() {
    if (const char* e = getenv("LR_ISOLATION_TIER")) if (e[0]) return e;
    // gVisor identifies itself in the kernel release string and ships a
    // distinctive /proc/self/status. Checking the release is enough and costs
    // one read.
    std::string rel = slurp("/proc/sys/kernel/osrelease");
    if (contains(rel, "gvisor") || contains(rel, "gVisor")) return "gvisor";
    if (exists("/.dockerenv")) return "container";
    std::string cg = slurp("/proc/self/cgroup");
    if (contains(cg, "docker") || contains(cg, "containerd") || contains(cg, "kubepods"))
        return "container";
    return "host";
}
#endif // __linux__

#if defined(__APPLE__)
std::string sysctl_str(const char* name) {
    size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
    std::string v(len, '\0');
    if (sysctlbyname(name, v.data(), &len, nullptr, 0) != 0) return {};
    while (!v.empty() && v.back() == '\0') v.pop_back();
    return v;
}
int64_t sysctl_i64(const char* name) {
    int64_t v = 0; size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
    return v;
}
#endif

Caps build_caps() {
    Caps c;
    utsname u{};
    if (uname(&u) == 0) {
        c.os   = std::string(u.sysname) + " " + u.release;
        c.arch = u.machine;
    }
    c.openssl_version = OpenSSL_version(OPENSSL_VERSION);

#if defined(__linux__)
    read_cpuinfo(c);
    read_hygiene(c);
    c.batched_io     = true;
    c.isolation_tier = read_isolation_tier();

    // Hard affinity is a property we verify rather than assume: read the mask
    // back after setting it.
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) c.hard_affinity = CPU_COUNT(&set) >= 1;
    // Cores are homogeneous unless the kernel exposes more than one CPU
    // capacity class, which is how ARM big.LITTLE and Intel hybrid parts appear.
    std::set<std::string> caps_seen;
    for (int i = 0; i < c.logical_cpus; ++i) {
        std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpu_capacity";
        std::string v = slurp(p.c_str());
        if (!v.empty()) caps_seen.insert(v);
    }
    c.homogeneous = caps_seen.size() <= 1;

#elif defined(__APPLE__)
    c.cpu_model      = sysctl_str("machdep.cpu.brand_string");
    c.logical_cpus   = (int)sysctl_i64("hw.logicalcpu");
    c.physical_cores = (int)sysctl_i64("hw.physicalcpu");
    c.smt_enabled    = c.logical_cpus > c.physical_cores;
    // Apple silicon exposes more than one performance level: P-cores and
    // E-cores. A scaling curve that spans both measures two different CPUs.
    c.homogeneous    = sysctl_i64("hw.nperflevels") <= 1;
    c.invariant_tsc  = true;      // mach timebase is invariant
    c.hard_affinity  = false;     // thread_policy_set affinity tags are a hint only
    c.batched_io     = false;     // no recvmmsg / sendmmsg
    c.governor       = "unavailable-on-darwin";
    c.turbo_control  = "unavailable-on-darwin";
    c.irqbalance     = "not-applicable";
    c.isolation_tier = "host";
#if defined(__aarch64__)
    c.hw_aes   = true;            // ARMv8 crypto extensions are mandatory on Apple silicon
    c.hw_clmul = true;
    c.cpu_flags = {"aes", "pmull", "neon"};
#endif
#endif

    c.uring_io     = uring_available(nullptr);
    c.uring_sqpoll = uring_sqpoll_available(nullptr);
#if defined(_OPENMP)
    c.openmp = true;
#endif

    std::string n, a, d;
    if (primary_interface(&n, &a, &d)) { c.nic_name = n; c.nic_addr = a; c.nic_driver = d; }

    c.tsc_hz         = clk::tick_hz();
    c.nominal_cpu_hz = clk::nominal_cpu_hz();
    c.clock_source   = clk::source();
    return c;
}

} // namespace

const Caps& caps() {
    static Caps c = build_caps();
    return c;
}

bool primary_interface(std::string* name, std::string* addr, std::string* driver) {
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return false;
    bool found = false;
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        std::string nm = p->ifa_name ? p->ifa_name : "";
        if (nm == "lo" || nm == "lo0") continue;
        if (nm.rfind("docker", 0) == 0 || nm.rfind("veth", 0) == 0 ||
            nm.rfind("br-", 0) == 0) continue;
        char buf[64] = {0};
        auto* sin = (sockaddr_in*)p->ifa_addr;
        ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        if (name)  *name = nm;
        if (addr)  *addr = buf;
        if (driver) {
#if defined(__linux__)
            // The driver name is the target of the sysfs symlink; gVNIC appears
            // as "gve", a paravirtual NIC as "virtio_net".
            std::string link = "/sys/class/net/" + nm + "/device/driver";
            char rp[512] = {0};
            ssize_t k = ::readlink(link.c_str(), rp, sizeof(rp) - 1);
            if (k > 0) {
                std::string s(rp, (size_t)k);
                auto slash = s.rfind('/');
                *driver = (slash == std::string::npos) ? s : s.substr(slash + 1);
            } else {
                *driver = "unknown";
            }
#else
            *driver = "darwin";
#endif
        }
        found = true;
        break;
    }
    freeifaddrs(ifa);
    return found;
}

HostClass Caps::klass() const {
    // A host that cannot time reliably or cannot hold a thread still may build,
    // self-test, analyse and typeset, but must never write a measurement.
    if (!invariant_tsc || !hard_affinity) return HostClass::Development;
#if !defined(__linux__)
    return HostClass::Development;
#else
    if (isolcpus && nohz_full && homogeneous && physical_cores >= 2)
        return HostClass::Measurement;
    return HostClass::Constrained;
#endif
}

std::vector<std::string> Caps::caveats() const {
    std::vector<std::string> v;
    if (!isolcpus)  v.push_back("kernel isolation absent: no isolcpus on the data-plane cores");
    if (!nohz_full) v.push_back("timer tick not suppressed: no nohz_full on the data-plane cores");
    if (!rcu_nocbs) v.push_back("RCU callbacks not offloaded: no rcu_nocbs on the data-plane cores");
    if (physical_cores < 2)
        v.push_back("single core available: generator and data plane share one CPU, "
                    "and core-count scaling cannot be measured");
    if (!homogeneous)
        v.push_back("heterogeneous cores: a scaling curve would span two microarchitectures");
    if (smt_enabled)
        v.push_back("simultaneous multithreading enabled: sibling threads share execution resources");
    if (turbo_control == "unavailable-in-guest" || turbo_control == "unavailable-on-darwin")
        v.push_back("core frequency not controllable: turbo state is set by the platform, not by the harness");
    if (irqbalance == "running")
        v.push_back("irqbalance running: interrupt affinity may move during a sweep");
    if (!batched_io)
        v.push_back("batched syscalls absent: the recvmmsg backend cannot be exercised");
    if (!uring_io)
        v.push_back("io_uring unavailable: the ring-based backends are absent from E3");
    else if (!uring_sqpoll)
        v.push_back("io_uring accepted but SQPOLL refused: the syscall-free submission "
                    "path, which bounds what kernel bypass could recover, was not measured");
    if (nic_driver == "unknown" || nic_addr.empty())
        v.push_back("no non-loopback interface discovered: wire experiments cannot run here");
    if (isolation_tier != "host")
        v.push_back("running inside a " + isolation_tier +
                    ": absolute costs include that layer's overhead");
    return v;
}

bool pin_to_cpu(int cpu) {
#if defined(__linux__)
    if (cpu < 0) return true;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) return false;
    // Read back: a mask that was accepted but not honoured is worse than a
    // refusal, because it looks like success.
    cpu_set_t got;
    CPU_ZERO(&got);
    if (sched_getaffinity(0, sizeof(got), &got) != 0) return false;
    return CPU_ISSET(cpu, &got) && CPU_COUNT(&got) == 1;
#else
    (void)cpu;
    return false;   // Darwin affinity tags are advisory; report the truth
#endif
}

const char* hw_crypto_mask_var() {
#if defined(__x86_64__)
    return "OPENSSL_ia32cap";
#elif defined(__aarch64__)
    return "OPENSSL_armcap";
#else
    return "";
#endif
}

const char* hw_crypto_mask_value() {
#if defined(__x86_64__)
    // Word 1 bit 57 = CPUID(1).ECX[25] AES-NI; bit 33 = CPUID(1).ECX[1] PCLMULQDQ.
    // Word 2 bit 41 = CPUID(7,0).ECX[9] VAES; bit 42 = CPUID(7,0).ECX[10] VPCLMULQDQ.
    // AVX and AVX2 are left intact so that the ChaCha20 vector path is unaffected
    // and the comparison isolates the AES accelerator rather than vectorisation.
    return "~0x200000200000000:~0x60000000000";
#elif defined(__aarch64__)
    // Bit 0 ARMV7_NEON kept; bit 2 ARMV8_AES and bit 5 ARMV8_PMULL cleared, for
    // the same reason: mask the AES accelerator, not the vector unit.
    return "0x1";
#else
    return "";
#endif
}

bool hw_crypto_masked() {
    // LR_MASK_STATE is set across the exec boundary by the sweep, next to the
    // OpenSSL capability variable, so both implementations see one signal.
    static const bool masked = [] {
        const char* s = getenv("LR_MASK_STATE");
        return s && std::strcmp(s, "off") == 0;
    }();
    return masked;
}

// Whether OpenSSL is actually using the hardware AES path.
//
// OpenSSL's resolved capability word is an internal symbol and is not exported
// by every distribution's libcrypto, so reading it is not portable. What is
// portable is measuring the thing the mask is supposed to change: bulk
// AES-256-GCM costs a fraction of a cycle per byte with the AES round
// instructions and well over ten cycles per byte without them. The two regimes
// are two orders of magnitude apart, so a threshold anywhere between them
// classifies correctly without tuning.
//
// The probe runs once, on a buffer that stays in L2, and its result is cached.
double openssl_aes_cycles_per_byte() {
    static double cpb = [] {
        static const size_t kBuf = 64 * 1024;
        std::vector<unsigned char> in(kBuf, 0x5A), out(kBuf + 32);
        unsigned char key[32] = {0}, iv[12] = {0}, tag[16] = {0};
        for (int i = 0; i < 32; ++i) key[i] = (unsigned char)(i * 7 + 1);

        EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
        if (!c) return 0.0;
        EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, key, nullptr);
        EVP_CIPHER_CTX_set_padding(c, 0);

        auto pass = [&](int reps) {
            int len = 0;
            for (int r = 0; r < reps; ++r) {
                iv[11] = (unsigned char)r;
                EVP_EncryptInit_ex(c, nullptr, nullptr, nullptr, iv);
                EVP_EncryptUpdate(c, out.data(), &len, in.data(), (int)kBuf);
                EVP_EncryptFinal_ex(c, out.data() + len, &len);
                EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, tag);
            }
        };
        pass(4);                                  // warm-up, discarded
        const uint64_t t0 = clk::ticks();
        const int reps = 32;
        pass(reps);
        const uint64_t dt = clk::ticks() - t0;
        EVP_CIPHER_CTX_free(c);
        return (double)dt * clk::cycles_per_tick() / (double)(reps * kBuf);
    }();
    return cpb;
}

// Threshold sits an order of magnitude clear of both regimes.
static constexpr double kHwAesCpbThreshold = 2.0;

bool openssl_hw_aes_active() {
    return openssl_aes_cycles_per_byte() < kHwAesCpbThreshold;
}

} // namespace lr::plat
