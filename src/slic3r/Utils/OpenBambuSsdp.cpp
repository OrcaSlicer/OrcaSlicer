// OpenBambuSsdp - SSDP discovery for BambuLab printers on LAN.
//
// Protocol knowledge derived from:
//   https://github.com/ClusterM/open-bamboo-networking (AGPL-3.0)
//   Copyright (C) 2026 Alexey Cluster and contributors
//   https://github.com/Doridian/OpenBambuAPI
//
// BambuLab printers broadcast SSDP NOTIFY on 239.255.255.250:1990.
// Headers include: USN (serial), Location (IP), DevModel.bambu.com,
// DevName.bambu.com, DevConnect.bambu.com, DevBind.bambu.com, etc.
// We parse these and emit a JSON string matching DeviceManager expectations.

#include "OpenBambuSsdp.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <sstream>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <poll.h>
#endif

namespace Slic3r {

namespace {

// BambuLab SSDP multicast group and port
constexpr const char* BAMBU_SSDP_MULTICAST = "239.255.255.250";
constexpr int         BAMBU_SSDP_PORT      = 1990;

std::string to_lower(const std::string& s)
{
    std::string r = s;
    for (auto& c : r) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string json_escape_str(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('"');
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

#ifdef _WIN32
void init_winsock_ssdp()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}
#endif

} // namespace

OpenBambuSsdp::OpenBambuSsdp() = default;

OpenBambuSsdp::~OpenBambuSsdp()
{
    stop();
}

bool OpenBambuSsdp::start(SsdpMessageCb cb)
{
    if (running_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        callback_ = std::move(cb);
        return true;
    }

    {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        callback_ = std::move(cb);
    }

#ifdef _WIN32
    init_winsock_ssdp();
    SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuSsdp: socket() failed";
        return false;
    }
#else
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuSsdp: socket() failed: " << strerror(errno);
        return false;
    }
#endif

    int yes = 1;
    setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));
    setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(BAMBU_SSDP_PORT);

    if (::bind(static_cast<int>(fd), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuSsdp: bind(:1990) failed (another SSDP listener?)";
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
        return false;
    }

    // Join multicast group
    struct ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr(BAMBU_SSDP_MULTICAST);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(static_cast<int>(fd), IPPROTO_IP, IP_ADD_MEMBERSHIP,
               reinterpret_cast<const char*>(&mreq), sizeof(mreq));

#ifdef _WIN32
    sock_ = static_cast<uintptr_t>(fd);
#else
    sock_ = fd;
#endif

    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run_loop(); });

    BOOST_LOG_TRIVIAL(info) << "OpenBambuSsdp: listening on " << BAMBU_SSDP_MULTICAST
                            << ":" << BAMBU_SSDP_PORT;
    return true;
}

void OpenBambuSsdp::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

#ifdef _WIN32
    SOCKET fd = static_cast<SOCKET>(sock_);
    sock_ = static_cast<uintptr_t>(INVALID_SOCKET);
    ::shutdown(fd, SD_BOTH);
    ::closesocket(fd);
#else
    int fd = sock_;
    sock_ = -1;
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
#endif

    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lk(cb_mutex_);
    callback_ = {};
    BOOST_LOG_TRIVIAL(info) << "OpenBambuSsdp: stopped";
}

void OpenBambuSsdp::run_loop()
{
    char buf[4096];

    while (running_.load(std::memory_order_acquire)) {
        struct pollfd pfd{};
#ifdef _WIN32
        pfd.fd = static_cast<SOCKET>(sock_);
#else
        pfd.fd = sock_;
#endif
        pfd.events = POLLIN;

#ifdef _WIN32
        int pr = WSAPoll(&pfd, 1, 1000);
#else
        int pr = ::poll(&pfd, 1, 1000);
#endif
        if (pr <= 0) continue;

        struct sockaddr_in src{};
        socklen_t slen = sizeof(src);
#ifdef _WIN32
        int n = ::recvfrom(static_cast<SOCKET>(sock_), buf, sizeof(buf) - 1, 0,
                           reinterpret_cast<struct sockaddr*>(&src), &slen);
#else
        ssize_t n = ::recvfrom(sock_, buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<struct sockaddr*>(&src), &slen);
#endif
        if (n <= 0) {
            if (!running_.load(std::memory_order_acquire)) break;
            continue;
        }

        buf[n] = '\0';
        std::string json = parse_to_json(buf, static_cast<size_t>(n));
        if (json.empty()) continue;

        SsdpMessageCb cb;
        {
            std::lock_guard<std::mutex> lk(cb_mutex_);
            cb = callback_;
        }
        if (cb) cb(json);
    }
}

std::string OpenBambuSsdp::parse_to_json(const char* data, size_t len)
{
    // Parse HTTP/SSDP-style headers
    std::string msg(data, len);

    // Validate it's an HTTP-style message
    if (msg.find("HTTP/1.") == std::string::npos) return {};

    // Parse headers into a case-insensitive map
    std::map<std::string, std::string> headers;
    size_t pos = msg.find('\n');
    if (pos == std::string::npos) return {};
    pos++;

    while (pos < msg.size()) {
        size_t eol = msg.find('\n', pos);
        if (eol == std::string::npos) eol = msg.size();
        size_t line_end = eol;
        if (line_end > pos && msg[line_end - 1] == '\r') --line_end;
        if (line_end == pos) break; // empty line

        std::string line = msg.substr(pos, line_end - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = to_lower(trim(line.substr(0, colon)));
            std::string val = trim(line.substr(colon + 1));
            if (!key.empty()) headers[key] = val;
        }
        pos = eol + 1;
    }

    // Filter: must have USN and DevModel to be a Bambu printer
    auto it_usn = headers.find("usn");
    auto it_model = headers.find("devmodel.bambu.com");
    if (it_usn == headers.end() || it_model == headers.end()) return {};
    if (it_usn->second.empty() || it_model->second.empty()) return {};

    // Build JSON matching DeviceManager::on_machine_alive expectations
    auto get = [&](const std::string& key) -> std::string {
        auto it = headers.find(key);
        return (it != headers.end()) ? it->second : "";
    };

    std::ostringstream os;
    os << "{"
       << "\"dev_name\":"        << json_escape_str(get("devname.bambu.com"))
       << ",\"dev_id\":"         << json_escape_str(get("usn"))
       << ",\"dev_ip\":"         << json_escape_str(get("location"))
       << ",\"dev_type\":"       << json_escape_str(get("devmodel.bambu.com"))
       << ",\"dev_signal\":"     << json_escape_str("")
       << ",\"connect_type\":"   << json_escape_str(get("devconnect.bambu.com"))
       << ",\"bind_state\":"     << json_escape_str(get("devbind.bambu.com"))
       << ",\"sec_link\":"       << json_escape_str(get("devseclink.bambu.com"))
       << ",\"ssdp_version\":"   << json_escape_str(get("devversion.bambu.com"))
       << ",\"connection_name\":" << json_escape_str(get("devinf.bambu.com"))
       << "}";

    return os.str();
}

} // namespace Slic3r
