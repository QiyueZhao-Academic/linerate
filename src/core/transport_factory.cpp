// transport_factory.cpp — backend selection and enumeration, in one place.
#include "lr/transport.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

namespace lr {

std::unique_ptr<Transport> make_transport_posix(const TransportConfig& cfg, std::string* err);
std::unique_ptr<Transport> make_transport_mmsg (const TransportConfig& cfg, std::string* err);
std::unique_ptr<Transport> make_transport_uring(const TransportConfig& cfg, bool sqpoll,
                                                std::string* err);

std::unique_ptr<Transport> make_transport(const TransportConfig& cfg, std::string* err) {
    if (cfg.backend == "posix")        return make_transport_posix(cfg, err);
    if (cfg.backend == "mmsg")         return make_transport_mmsg(cfg, err);
    if (cfg.backend == "uring")        return make_transport_uring(cfg, false, err);
    if (cfg.backend == "uring-sqpoll") return make_transport_uring(cfg, true, err);
    if (err) *err = "unknown transport backend: " + cfg.backend;
    return nullptr;
}

uint16_t transport_local_port(const Transport& t) {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (::getsockname(t.fd(), (sockaddr*)&a, &len) != 0) return 0;
    return ntohs(a.sin_port);
}

// Enumerated once. A backend the kernel refuses is absent from the sweep and
// its absence is recorded, rather than producing a column of failed units.
const std::vector<std::string>& transport_backends() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> b{"posix"};
        if (batched_io_available())        b.push_back("mmsg");
        if (uring_available(nullptr))      b.push_back("uring");
        if (uring_sqpoll_available(nullptr)) b.push_back("uring-sqpoll");
        return b;
    }();
    return v;
}

} // namespace lr
