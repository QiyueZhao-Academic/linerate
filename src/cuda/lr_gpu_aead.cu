// lr_gpu_aead.cu — the same question, one level down the memory hierarchy.
//
// The study's central quantity is a crossover: the payload size s* = a/b at
// which a fixed per-packet cost equals a cost proportional to the payload. A
// GPU offload has exactly the same shape, with different constants:
//
//     T(n, s) = a_gpu + (s * n) / B_pcie + (s * n) / R_gpu
//               ^^^^^   ^^^^^^^^^^^^^^^^   ^^^^^^^^^^^^^^^^
//               launch  transfer over the  the kernel itself
//               and     bus, which the
//               sync    CPU path does not pay at all
//
// The offload wins only when the batch is large enough to amortise a_gpu and
// when the per-byte rate on the far side of the bus beats the CPU's, and the
// transfer term means the second condition is much harder than it looks: PCIe
// bandwidth is roughly one order of magnitude below what AES-NI sustains per
// core, so for a *streaming* data plane the bus is usually the answer before the
// kernel ever runs. Measuring where that stops being true is the point.
//
// The kernel here is AES-256 in counter mode, one thread per 16-byte block,
// with the key schedule computed once on the host and the S-box and round keys
// resident in constant memory. Authentication is deliberately not included: a
// GHASH over a batch is a parallel-prefix problem whose GPU implementation is a
// project of its own, and including a bad one would understate the offload. The
// report states that this measures the confidentiality half only, which makes
// the GPU numbers an optimistic bound and the conclusion — that the bus
// dominates — stronger rather than weaker.
//
// Built only when a CUDA toolkit is present. The experiment writes a fragment
// saying so when it is absent, so the dataset stays complete either way.
#include <cuda_runtime.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lr/clock.hpp"
#include "lr/json.hpp"
#include "lr/platform.hpp"
#include "lr/stats.hpp"

#include <sys/stat.h>

namespace {

#define CUDA_OK(call)                                                        \
    do {                                                                     \
        cudaError_t e_ = (call);                                             \
        if (e_ != cudaSuccess) {                                             \
            fprintf(stderr, "%s:%d %s: %s\n", __FILE__, __LINE__, #call,     \
                    cudaGetErrorString(e_));                                 \
            return -1;                                                       \
        }                                                                    \
    } while (0)

// Round keys and the forward S-box live in constant memory: every thread in a
// warp reads the same round key at the same time, which is the access pattern
// the constant cache broadcasts in one transaction.
__constant__ unsigned char d_sbox[256];
__constant__ unsigned int  d_rk[60];      // AES-256: 14 rounds + 1, four words each

// AES S-box, from the specification.
static const unsigned char h_sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

__device__ __forceinline__ unsigned char xtime(unsigned char x) {
    return (unsigned char)((x << 1) ^ ((x >> 7) * 0x1b));
}

// One AES-256 block encryption, plain byte-oriented rounds. A table-based
// implementation would be faster but would also make the kernel a study of
// shared-memory bank conflicts rather than of the offload decision.
__device__ void aes256_block(const unsigned char in[16], unsigned char out[16]) {
    unsigned char s[16];
    #pragma unroll
    for (int i = 0; i < 16; ++i) s[i] = in[i];

    // AddRoundKey, round 0.
    #pragma unroll
    for (int c = 0; c < 4; ++c) {
        unsigned int k = d_rk[c];
        s[4*c+0] ^= (unsigned char)(k >> 24);
        s[4*c+1] ^= (unsigned char)(k >> 16);
        s[4*c+2] ^= (unsigned char)(k >> 8);
        s[4*c+3] ^= (unsigned char)(k);
    }

    for (int round = 1; round <= 14; ++round) {
        #pragma unroll
        for (int i = 0; i < 16; ++i) s[i] = d_sbox[s[i]];

        // ShiftRows.
        unsigned char t;
        t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
        t = s[2];  s[2]  = s[10]; s[10] = t;
        t = s[6];  s[6]  = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7];  s[7]  = t;

        if (round != 14) {
            #pragma unroll
            for (int c = 0; c < 4; ++c) {
                unsigned char* p = s + 4 * c;
                const unsigned char a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                const unsigned char x = a0 ^ a1 ^ a2 ^ a3;
                p[0] = a0 ^ x ^ xtime((unsigned char)(a0 ^ a1));
                p[1] = a1 ^ x ^ xtime((unsigned char)(a1 ^ a2));
                p[2] = a2 ^ x ^ xtime((unsigned char)(a2 ^ a3));
                p[3] = a3 ^ x ^ xtime((unsigned char)(a3 ^ a0));
            }
        }
        #pragma unroll
        for (int c = 0; c < 4; ++c) {
            unsigned int k = d_rk[4 * round + c];
            s[4*c+0] ^= (unsigned char)(k >> 24);
            s[4*c+1] ^= (unsigned char)(k >> 16);
            s[4*c+2] ^= (unsigned char)(k >> 8);
            s[4*c+3] ^= (unsigned char)(k);
        }
    }
    #pragma unroll
    for (int i = 0; i < 16; ++i) out[i] = s[i];
}

// One thread per 16-byte block. Counter mode has no chain between blocks, which
// is what makes it the right cipher mode for this comparison: any dependency
// would measure the serialisation rather than the arithmetic.
__global__ void aes_ctr_kernel(const unsigned char* __restrict__ in,
                               unsigned char* __restrict__ out,
                               const unsigned char* __restrict__ nonces,
                               int packets, int payload_bytes) {
    const int blocks_per_packet = (payload_bytes + 15) / 16;
    const long long gid = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    const long long total = (long long)packets * blocks_per_packet;
    if (gid >= total) return;

    const int pkt = (int)(gid / blocks_per_packet);
    const int blk = (int)(gid % blocks_per_packet);

    unsigned char ctr[16];
    #pragma unroll
    for (int i = 0; i < 12; ++i) ctr[i] = nonces[pkt * 12 + i];
    const unsigned int c = (unsigned int)blk + 1u;
    ctr[12] = (unsigned char)(c >> 24);
    ctr[13] = (unsigned char)(c >> 16);
    ctr[14] = (unsigned char)(c >> 8);
    ctr[15] = (unsigned char)(c);

    unsigned char ks[16];
    aes256_block(ctr, ks);

    const long long base = (long long)pkt * payload_bytes + (long long)blk * 16;
    const int n = min(16, payload_bytes - blk * 16);
    for (int i = 0; i < n; ++i) out[base + i] = in[base + i] ^ ks[i];
}

// AES-256 key expansion on the host, so the kernel never spends a thread on it.
void expand_key(const unsigned char key[32], unsigned int rk[60]) {
    static const unsigned char rcon[] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40};
    for (int i = 0; i < 8; ++i)
        rk[i] = ((unsigned int)key[4*i] << 24) | ((unsigned int)key[4*i+1] << 16) |
                ((unsigned int)key[4*i+2] << 8) | (unsigned int)key[4*i+3];
    for (int i = 8; i < 60; ++i) {
        unsigned int t = rk[i-1];
        if (i % 8 == 0) {
            t = (t << 8) | (t >> 24);
            t = ((unsigned int)h_sbox[(t >> 24) & 0xff] << 24) |
                ((unsigned int)h_sbox[(t >> 16) & 0xff] << 16) |
                ((unsigned int)h_sbox[(t >>  8) & 0xff] <<  8) |
                ((unsigned int)h_sbox[t & 0xff]);
            t ^= (unsigned int)rcon[i / 8 - 1] << 24;
        } else if (i % 8 == 4) {
            t = ((unsigned int)h_sbox[(t >> 24) & 0xff] << 24) |
                ((unsigned int)h_sbox[(t >> 16) & 0xff] << 16) |
                ((unsigned int)h_sbox[(t >>  8) & 0xff] <<  8) |
                ((unsigned int)h_sbox[t & 0xff]);
        }
        rk[i] = rk[i-8] ^ t;
    }
}

std::string opt(int argc, char** argv, const char* name, const char* def) {
    const std::string pre = std::string("--") + name + "=";
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], pre.c_str(), pre.size()) == 0) return argv[i] + pre.size();
    return def;
}

struct BatchPoint {
    int    packets = 0;
    double h2d_ns = 0, kernel_ns = 0, d2h_ns = 0, total_ns = 0;
    double ns_per_packet = 0;
    double gbps_effective = 0;
};

} // namespace

int main(int argc, char** argv) {
    const std::string outdir = opt(argc, argv, "out", "results/raw");
    const int payload = atoi(opt(argc, argv, "payload", "1432").c_str());
    const int reps    = atoi(opt(argc, argv, "reps", "9").c_str());
    ::mkdir(outdir.c_str(), 0755);
    const std::string path = outdir + "/e8_gpu.json";

    int ndev = 0;
    if (cudaGetDeviceCount(&ndev) != cudaSuccess || ndev == 0) {
        lr::json::Writer w;
        w.obj_open();
          w.key("e8").obj_open();
            w.kv("available", false);
            w.kv("reason", "no CUDA device present");
          w.obj_close();
        w.obj_close();
        w.write_file(path);
        printf("lr_gpu_aead: no CUDA device; wrote %s\n", path.c_str());
        return 0;
    }

    cudaDeviceProp prop{};
    CUDA_OK(cudaGetDeviceProperties(&prop, 0));

    unsigned char key[32];
    for (int i = 0; i < 32; ++i) key[i] = (unsigned char)(0x9f ^ (i * 13));
    unsigned int rk[60];
    expand_key(key, rk);
    CUDA_OK(cudaMemcpyToSymbol(d_sbox, h_sbox, sizeof(h_sbox)));
    CUDA_OK(cudaMemcpyToSymbol(d_rk, rk, sizeof(rk)));

    // Batch sizes spanning four orders of magnitude: the small end is where the
    // launch cost dominates and the offload cannot possibly win, the large end
    // is where the bus does.
    std::vector<int> batches = {1, 4, 16, 64, 256, 1024, 4096, 16384, 65536};
    std::vector<BatchPoint> points;

    const int max_packets = batches.back();
    unsigned char *h_in = nullptr, *h_out = nullptr, *h_non = nullptr;
    // Pinned host memory: pageable memory would add a staging copy inside the
    // driver and the transfer term would then be measuring that copy.
    CUDA_OK(cudaMallocHost(&h_in,  (size_t)max_packets * payload));
    CUDA_OK(cudaMallocHost(&h_out, (size_t)max_packets * payload));
    CUDA_OK(cudaMallocHost(&h_non, (size_t)max_packets * 12));
    for (size_t i = 0; i < (size_t)max_packets * payload; ++i)
        h_in[i] = (unsigned char)(i * 31u + 7u);
    for (size_t i = 0; i < (size_t)max_packets * 12; ++i)
        h_non[i] = (unsigned char)(i * 17u + 3u);

    unsigned char *d_in = nullptr, *d_out = nullptr, *d_non = nullptr;
    CUDA_OK(cudaMalloc(&d_in,  (size_t)max_packets * payload));
    CUDA_OK(cudaMalloc(&d_out, (size_t)max_packets * payload));
    CUDA_OK(cudaMalloc(&d_non, (size_t)max_packets * 12));

    cudaEvent_t e0, e1, e2, e3;
    CUDA_OK(cudaEventCreate(&e0)); CUDA_OK(cudaEventCreate(&e1));
    CUDA_OK(cudaEventCreate(&e2)); CUDA_OK(cudaEventCreate(&e3));

    const int blocks_per_packet = (payload + 15) / 16;

    for (int n : batches) {
        std::vector<double> h2d, ker, d2h, tot;
        for (int r = 0; r < reps; ++r) {
            const size_t bytes = (size_t)n * payload;
            const int threads = 256;
            const long long total_blocks = (long long)n * blocks_per_packet;
            const int grid = (int)((total_blocks + threads - 1) / threads);

            CUDA_OK(cudaEventRecord(e0));
            CUDA_OK(cudaMemcpyAsync(d_in, h_in, bytes, cudaMemcpyHostToDevice));
            CUDA_OK(cudaMemcpyAsync(d_non, h_non, (size_t)n * 12, cudaMemcpyHostToDevice));
            CUDA_OK(cudaEventRecord(e1));
            aes_ctr_kernel<<<grid, threads>>>(d_in, d_out, d_non, n, payload);
            CUDA_OK(cudaEventRecord(e2));
            CUDA_OK(cudaMemcpyAsync(h_out, d_out, bytes, cudaMemcpyDeviceToHost));
            CUDA_OK(cudaEventRecord(e3));
            CUDA_OK(cudaEventSynchronize(e3));
            CUDA_OK(cudaGetLastError());

            float a = 0, b = 0, c = 0, d = 0;
            CUDA_OK(cudaEventElapsedTime(&a, e0, e1));
            CUDA_OK(cudaEventElapsedTime(&b, e1, e2));
            CUDA_OK(cudaEventElapsedTime(&c, e2, e3));
            CUDA_OK(cudaEventElapsedTime(&d, e0, e3));
            // The first replicate of the first batch pays for context creation
            // and module load; it is discarded rather than averaged in.
            if (r == 0 && n == batches.front()) continue;
            h2d.push_back(a * 1e6); ker.push_back(b * 1e6);
            d2h.push_back(c * 1e6); tot.push_back(d * 1e6);
        }
        if (tot.empty()) continue;
        BatchPoint p;
        p.packets   = n;
        p.h2d_ns    = lr::stats::median(h2d);
        p.kernel_ns = lr::stats::median(ker);
        p.d2h_ns    = lr::stats::median(d2h);
        p.total_ns  = lr::stats::median(tot);
        p.ns_per_packet = p.total_ns / n;
        p.gbps_effective = p.total_ns > 0
            ? (double)n * payload * 8.0 / p.total_ns : 0;
        points.push_back(p);
        printf("  n=%-6d  h2d %8.1f  kernel %8.1f  d2h %8.1f  total %9.1f us  "
               "%7.2f Gb/s\n", n, p.h2d_ns / 1e3, p.kernel_ns / 1e3,
               p.d2h_ns / 1e3, p.total_ns / 1e3, p.gbps_effective);
    }

    // The fixed-versus-marginal decomposition, the same shape as C(s)=a+b*s but
    // with the batch count as the independent variable: a_gpu is the launch and
    // synchronisation cost, and the marginal term is per packet.
    double a_gpu = 0, b_gpu = 0;
    if (points.size() >= 2) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const size_t n = points.size();
        for (const auto& p : points) {
            const double x = p.packets, y = p.total_ns;
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double den = n * sxx - sx * sx;
        if (den != 0) { b_gpu = (n * sxy - sx * sy) / den; a_gpu = (sy - b_gpu * sx) / n; }
    }
    // Batch size at which the offload's per-packet cost falls to twice its
    // asymptote: the practical "large enough to be worth it" point.
    double knee = 0;
    for (const auto& p : points)
        if (b_gpu > 0 && p.ns_per_packet <= 2.0 * b_gpu) { knee = p.packets; break; }

    lr::json::Writer w;
    w.obj_open();
      w.key("e8").obj_open();
        w.kv("available", true);
        w.kv("device", std::string(prop.name));
        w.kv("compute_capability",
             std::to_string(prop.major) + "." + std::to_string(prop.minor));
        w.kv("sm_count", prop.multiProcessorCount);
        w.kv("memory_clock_khz", (long long)prop.memoryClockRate);
        w.kv("memory_bus_width_bits", prop.memoryBusWidth);
        w.kv("payload_bytes", payload);
        w.kv("replicates", reps);
        w.kv("cipher", "aes-256-ctr");
        w.kv("authentication", false);
        w.kv("authentication_note",
             "confidentiality only: a parallel GHASH is a separate problem, and "
             "omitting it makes these numbers an optimistic bound on the offload");
        w.kv("launch_fixed_ns", a_gpu, 1);
        w.kv("marginal_ns_per_packet", b_gpu, 4);
        w.kv("worthwhile_batch_packets", knee, 0);
        w.key("points").arr_open();
        for (const auto& p : points) {
            w.obj_open();
              w.kv("packets", p.packets);
              w.kv("h2d_ns", p.h2d_ns, 1);
              w.kv("kernel_ns", p.kernel_ns, 1);
              w.kv("d2h_ns", p.d2h_ns, 1);
              w.kv("total_ns", p.total_ns, 1);
              w.kv("ns_per_packet", p.ns_per_packet, 3);
              w.kv("gbps_effective", p.gbps_effective, 4);
              w.kv("transfer_share",
                   p.total_ns > 0 ? (p.h2d_ns + p.d2h_ns) / p.total_ns : 0.0, 4);
            w.obj_close();
        }
        w.arr_close();
      w.obj_close();
    w.obj_close();
    if (!w.write_file(path)) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    printf("lr_gpu_aead: wrote %s (%zu batch sizes on %s)\n",
           path.c_str(), points.size(), prop.name);

    cudaFreeHost(h_in); cudaFreeHost(h_out); cudaFreeHost(h_non);
    cudaFree(d_in); cudaFree(d_out); cudaFree(d_non);
    return 0;
}
