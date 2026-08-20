// ctrl.cpp — the client half of the control channel. The daemon half lives in
// src/apps/lr_loadgend.cpp, and both use parse_kv so they cannot disagree about
// the wire format.
#include "lr/ctrl.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace lr::ctrl {

std::map<std::string, std::string> parse_kv(const std::string& s) {
    std::map<std::string, std::string> m;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) {
        auto eq = tok.find('=');
        if (eq == std::string::npos) continue;
        m[tok.substr(0, eq)] = tok.substr(eq + 1);
    }
    return m;
}

Client::~Client() { close(); }

void Client::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool Client::connect(const std::string& host, uint16_t port, std::string* err) {
    close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) { if (err) *err = "socket"; return false; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        if (err) *err = "bad control host: " + host;
        close(); return false;
    }
    if (::connect(fd_, (sockaddr*)&a, sizeof(a)) != 0) {
        if (err) *err = std::string("connect ") + host + ": " + strerror(errno);
        close(); return false;
    }
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // A generator that has wedged must not wedge the sweep with it.
    timeval tv{30, 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
}

bool Client::request(const std::string& line, std::string* reply, std::string* err) {
    if (fd_ < 0) { if (err) *err = "not connected"; return false; }
    std::string out = line + "\n";
    size_t off = 0;
    while (off < out.size()) {
        ssize_t w = ::send(fd_, out.data() + off, out.size() - off, 0);
        if (w <= 0) { if (err) *err = "control send failed"; return false; }
        off += (size_t)w;
    }
    std::string buf;
    char c;
    for (;;) {
        ssize_t r = ::recv(fd_, &c, 1, 0);
        if (r <= 0) { if (err) *err = "control connection closed"; return false; }
        if (c == '\n') break;
        buf += c;
        if (buf.size() > 4096) { if (err) *err = "control reply too long"; return false; }
    }
    *reply = buf;
    return true;
}

bool Client::ping(std::map<std::string, std::string>* info, std::string* err) {
    std::string rep;
    if (!request("PING", &rep, err)) return false;
    if (rep.rfind("PONG", 0) != 0) { if (err) *err = "unexpected reply: " + rep; return false; }
    if (info) *info = parse_kv(rep.substr(4));
    return true;
}

bool Client::start(const StartRequest& q, std::string* err) {
    char line[512];
    std::snprintf(line, sizeof(line),
        "START cipher=%s payload=%zu pps=%.3f seconds=%.3f dst=%s:%u "
        "return_port=%u epoch_packets=%llu cpu=%d",
        q.cipher.c_str(), q.payload, q.pps, q.seconds, q.dst_addr.c_str(),
        (unsigned)q.dst_port, (unsigned)q.return_port,
        (unsigned long long)q.epoch_packets, q.cpu);
    std::string rep;
    if (!request(line, &rep, err)) return false;
    if (rep.rfind("OK", 0) != 0) { if (err) *err = "generator refused: " + rep; return false; }
    return true;
}

bool Client::stop(StopReply* rep, std::string* err) {
    std::string s;
    if (!request("STOP", &s, err)) return false;
    if (s.rfind("DONE", 0) != 0) { if (err) *err = "unexpected reply: " + s; return false; }
    auto kv = parse_kv(s.substr(4));
    auto num = [&](const char* k) -> double {
        auto it = kv.find(k);
        return it == kv.end() ? 0.0 : strtod(it->second.c_str(), nullptr);
    };
    rep->sent       = (uint64_t)num("sent");
    rep->refused    = (uint64_t)num("refused");
    rep->returned   = (uint64_t)num("returned");
    rep->rtt_p50_ns = num("rtt_p50_ns");
    rep->rtt_p99_ns = num("rtt_p99_ns");
    rep->rtt_p999_ns= num("rtt_p999_ns");
    {
        auto it = kv.find("fault");
        rep->fault = (it == kv.end()) ? std::string() : it->second;
    }
    rep->ok = true;
    return true;
}

} // namespace lr::ctrl
