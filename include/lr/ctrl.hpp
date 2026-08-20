// ctrl.hpp — the control channel between the measuring node and the load
// generator on the other node.
//
// The sweep runs 350-odd units. Driving each one over ssh from the laptop would
// spend more time on ssh than on measurement and would put the laptop's network
// inside the measurement loop. Instead the generator runs as a daemon on node A
// and node B drives it over a TCP control connection: one connection for the
// whole sweep, one short request per unit.
//
// TCP, not UDP, and text, not a binary struct: the control plane is not what is
// being measured, so the cheapest correct thing wins. Line-oriented text also
// means the daemon can be driven by hand from `nc` while debugging, which is
// worth more during a two-node bring-up than a few bytes on the wire.
//
//   -> START cipher=aes-256-gcm payload=512 pps=0 seconds=2 dst=10.0.0.3:41234
//            return_port=41235 epoch_packets=0
//   <- OK
//   -> STOP
//   <- DONE sent=812344 refused=12 rtt_p50_ns=41230 rtt_p99_ns=98120
//            rtt_p999_ns=210400 returned=811900 fault=
//
// Every value is one whitespace-free token, because both ends parse with
// parse_kv and parse_kv splits on whitespace. `fault` is empty on the happy
// path and carries a reason otherwise.
//   -> PING
//   <- PONG version=2 arch=x86_64 nic=ens4 driver=gve addr=10.0.0.2
#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace lr::ctrl {

static constexpr uint16_t kDefaultPort = 9099;

struct StartRequest {
    std::string cipher   = "aes-256-gcm";
    size_t      payload  = 512;
    double      pps      = 0;        // 0 = unpaced: send as fast as the socket allows
    double      seconds  = 2.0;
    std::string dst_addr = "127.0.0.1";
    uint16_t    dst_port = 0;
    uint16_t    return_port = 0;     // 0 = do not measure the return path
    uint64_t    epoch_packets = 0;
    int         cpu      = -1;
};

struct StopReply {
    uint64_t sent = 0, refused = 0, returned = 0;
    double   rtt_p50_ns = 0, rtt_p99_ns = 0, rtt_p999_ns = 0;
    // Empty when the unit ran. Otherwise the reason its sender thread could not
    // start, as a single whitespace-free token: a cipher the far node's factory
    // does not know, or a destination it could not open. Without this a
    // misconfigured unit reported zero packets sent, which reads as a measured
    // zero rather than as a failure.
    std::string fault;
    bool     ok = false;
};

// Client side, used by the bench. Every call returns false and fills `err` on a
// protocol or transport failure; none of them throw.
class Client {
public:
    Client() = default;
    ~Client();
    bool connect(const std::string& host, uint16_t port, std::string* err);
    bool ping(std::map<std::string, std::string>* info, std::string* err);
    bool start(const StartRequest& req, std::string* err);
    bool stop(StopReply* rep, std::string* err);
    void close();
    bool connected() const { return fd_ >= 0; }
private:
    bool request(const std::string& line, std::string* reply, std::string* err);
    int fd_ = -1;
};

// Parse "k=v k=v" into a map. Shared by both ends so they cannot disagree.
std::map<std::string, std::string> parse_kv(const std::string& s);

} // namespace lr::ctrl
