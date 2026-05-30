// OpenBambuMqtt - Minimal MQTT 3.1.1 client over TLS for BambuLab LAN printing.
//
// Protocol knowledge derived from:
//   https://github.com/ClusterM/open-bamboo-networking (AGPL-3.0)
//   Copyright (C) 2026 Alexey Cluster and contributors
//   https://github.com/Doridian/OpenBambuAPI
//
// BambuLab printers in LAN/Developer mode accept MQTT connections on port 8883
// with TLS. The username is "bblp" and the password is the printer's access code.
// Topics are: device/{serial}/request (publish to) and device/{serial}/report (subscribe).

#include "OpenBambuMqtt.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using ssize_t = int;
#  define SHUT_RDWR SD_BOTH
#  define close closesocket
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif

namespace Slic3r {

namespace {

std::string generate_client_id()
{
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream oss;
    oss << "orca-" << std::hex << rng();
    return oss.str();
}

#ifdef _WIN32
void init_winsock()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}
#endif

int connect_tcp(const std::string& host, int port, int timeout_ms)
{
#ifdef _WIN32
    init_winsock();
#endif
    struct addrinfo hints{}, *ai = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    int rv = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &ai);
    if (rv != 0 || !ai) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuMqtt: getaddrinfo failed for " << host << ":" << port;
        return -1;
    }

    int fd = -1;
    for (struct addrinfo* a = ai; a; a = a->ai_next) {
        fd = static_cast<int>(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
        if (fd < 0) continue;

        // Set non-blocking for connect timeout
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(fd, FIONBIO, &nb);
#else
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

        int rc = ::connect(fd, a->ai_addr, static_cast<int>(a->ai_addrlen));
        if (rc == 0) break;

#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
#else
        int err = errno;
        if (err == EINPROGRESS) {
#endif
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
#ifdef _WIN32
            int pr = WSAPoll(&pfd, 1, timeout_ms);
#else
            int pr = ::poll(&pfd, 1, timeout_ms);
#endif
            if (pr > 0) {
                int so_err = 0;
                socklen_t slen = sizeof(so_err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &slen);
                if (so_err == 0) break;
            }
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(ai);

    if (fd >= 0) {
        // Set back to blocking
#ifdef _WIN32
        u_long nb = 0;
        ioctlsocket(fd, FIONBIO, &nb);
#else
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
        // Enable TCP_NODELAY
        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
    }
    return fd;
}

} // namespace

OpenBambuMqtt::OpenBambuMqtt() = default;

OpenBambuMqtt::~OpenBambuMqtt()
{
    disconnect();
}

void OpenBambuMqtt::set_on_connect(MqttConnectCb cb)
{
    std::lock_guard<std::mutex> lk(cb_mutex_);
    on_connect_ = std::move(cb);
}

void OpenBambuMqtt::set_on_disconnect(MqttDisconnectCb cb)
{
    std::lock_guard<std::mutex> lk(cb_mutex_);
    on_disconnect_ = std::move(cb);
}

void OpenBambuMqtt::set_on_message(MqttMessageCb cb)
{
    std::lock_guard<std::mutex> lk(cb_mutex_);
    on_message_ = std::move(cb);
}

std::string OpenBambuMqtt::encode_remaining_length(uint32_t len)
{
    std::string result;
    do {
        uint8_t byte = len & 0x7F;
        len >>= 7;
        if (len > 0) byte |= 0x80;
        result.push_back(static_cast<char>(byte));
    } while (len > 0);
    return result;
}

std::string OpenBambuMqtt::build_connect_packet(const MqttConnectConfig& cfg)
{
    // Variable header
    std::string var_header;
    // Protocol Name: "MQTT"
    var_header.push_back(0x00); var_header.push_back(0x04);
    var_header += "MQTT";
    // Protocol Level: 4 (MQTT 3.1.1)
    var_header.push_back(0x04);
    // Connect Flags: username + password + clean session
    uint8_t flags = 0x02; // Clean session
    if (!cfg.username.empty()) flags |= 0x80;
    if (!cfg.password.empty()) flags |= 0x40;
    var_header.push_back(static_cast<char>(flags));
    // Keep Alive
    uint16_t ka = static_cast<uint16_t>(cfg.keepalive_s);
    var_header.push_back(static_cast<char>((ka >> 8) & 0xFF));
    var_header.push_back(static_cast<char>(ka & 0xFF));

    // Payload
    std::string payload;
    auto add_string = [&](const std::string& s) {
        uint16_t len = static_cast<uint16_t>(s.size());
        payload.push_back(static_cast<char>((len >> 8) & 0xFF));
        payload.push_back(static_cast<char>(len & 0xFF));
        payload += s;
    };

    std::string client_id = cfg.client_id.empty() ? generate_client_id() : cfg.client_id;
    add_string(client_id);
    if (!cfg.username.empty()) add_string(cfg.username);
    if (!cfg.password.empty()) add_string(cfg.password);

    // Fixed header: CONNECT = 0x10
    std::string packet;
    packet.push_back(0x10);
    uint32_t remaining = static_cast<uint32_t>(var_header.size() + payload.size());
    packet += encode_remaining_length(remaining);
    packet += var_header;
    packet += payload;
    return packet;
}

std::string OpenBambuMqtt::build_subscribe_packet(const std::string& topic, int qos)
{
    uint16_t pid = next_packet_id_++;
    if (next_packet_id_ == 0) next_packet_id_ = 1;

    std::string var_header;
    var_header.push_back(static_cast<char>((pid >> 8) & 0xFF));
    var_header.push_back(static_cast<char>(pid & 0xFF));

    std::string payload;
    uint16_t tlen = static_cast<uint16_t>(topic.size());
    payload.push_back(static_cast<char>((tlen >> 8) & 0xFF));
    payload.push_back(static_cast<char>(tlen & 0xFF));
    payload += topic;
    payload.push_back(static_cast<char>(qos & 0x03));

    // Fixed header: SUBSCRIBE = 0x82 (type 8, flags 0x02)
    std::string packet;
    packet.push_back(static_cast<char>(0x82));
    uint32_t remaining = static_cast<uint32_t>(var_header.size() + payload.size());
    packet += encode_remaining_length(remaining);
    packet += var_header;
    packet += payload;
    return packet;
}

std::string OpenBambuMqtt::build_publish_packet(const std::string& topic, const std::string& payload, int qos)
{
    std::string var_header;
    uint16_t tlen = static_cast<uint16_t>(topic.size());
    var_header.push_back(static_cast<char>((tlen >> 8) & 0xFF));
    var_header.push_back(static_cast<char>(tlen & 0xFF));
    var_header += topic;

    if (qos > 0) {
        uint16_t pid = next_packet_id_++;
        if (next_packet_id_ == 0) next_packet_id_ = 1;
        var_header.push_back(static_cast<char>((pid >> 8) & 0xFF));
        var_header.push_back(static_cast<char>(pid & 0xFF));
    }

    // Fixed header: PUBLISH = 0x30 + qos flags
    uint8_t type_byte = 0x30;
    if (qos == 1) type_byte |= 0x02;
    else if (qos == 2) type_byte |= 0x04;

    std::string packet;
    packet.push_back(static_cast<char>(type_byte));
    uint32_t remaining = static_cast<uint32_t>(var_header.size() + payload.size());
    packet += encode_remaining_length(remaining);
    packet += var_header;
    packet += payload;
    return packet;
}

std::string OpenBambuMqtt::build_pingreq_packet()
{
    std::string packet;
    packet.push_back(static_cast<char>(0xC0)); // PINGREQ
    packet.push_back(0x00);                     // Remaining length = 0
    return packet;
}

std::string OpenBambuMqtt::build_disconnect_packet()
{
    std::string packet;
    packet.push_back(static_cast<char>(0xE0)); // DISCONNECT
    packet.push_back(0x00);
    return packet;
}

bool OpenBambuMqtt::send_raw(const std::string& data)
{
    std::lock_guard<std::mutex> lk(write_mutex_);
    if (!connected_.load(std::memory_order_acquire)) return false;

    const char* ptr = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        int n;
        if (ssl_) {
            n = SSL_write(ssl_, ptr, static_cast<int>(remaining));
            if (n <= 0) return false;
        } else {
#ifdef _WIN32
            n = ::send(sock_fd_, ptr, static_cast<int>(remaining), 0);
#else
            n = static_cast<int>(::send(sock_fd_, ptr, remaining, 0));
#endif
            if (n <= 0) return false;
        }
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

bool OpenBambuMqtt::read_bytes(char* buf, size_t count, int timeout_ms)
{
    size_t offset = 0;
    while (offset < count) {
        if (!running_.load(std::memory_order_acquire)) return false;

        // Poll for data availability
        struct pollfd pfd{};
        pfd.fd = sock_fd_;
        pfd.events = POLLIN;
#ifdef _WIN32
        int pr = WSAPoll(&pfd, 1, std::min(timeout_ms, 1000));
#else
        int pr = ::poll(&pfd, 1, std::min(timeout_ms, 1000));
#endif
        if (pr < 0) return false;
        if (pr == 0) {
            // Timeout - check if still running
            if (!running_.load(std::memory_order_acquire)) return false;
            continue;
        }

        int n;
        if (ssl_) {
            n = SSL_read(ssl_, buf + offset, static_cast<int>(count - offset));
            if (n <= 0) return false;
        } else {
#ifdef _WIN32
            n = ::recv(sock_fd_, buf + offset, static_cast<int>(count - offset), 0);
#else
            n = static_cast<int>(::recv(sock_fd_, buf + offset, count - offset, 0));
#endif
            if (n <= 0) return false;
        }
        offset += static_cast<size_t>(n);
    }
    return true;
}

int OpenBambuMqtt::connect(const MqttConnectConfig& cfg)
{
    BOOST_LOG_TRIVIAL(info) << "OpenBambuMqtt: connecting to " << cfg.host << ":" << cfg.port
                            << " tls=" << cfg.use_tls << " user=" << cfg.username;

    // TCP connect
    sock_fd_ = connect_tcp(cfg.host, cfg.port, 10000);
    if (sock_fd_ < 0) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuMqtt: TCP connect failed";
        return -1;
    }

    // TLS setup
    if (cfg.use_tls) {
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx_) {
            BOOST_LOG_TRIVIAL(error) << "OpenBambuMqtt: SSL_CTX_new failed";
            ::close(sock_fd_);
            sock_fd_ = -1;
            return -1;
        }

        if (cfg.tls_insecure) {
            SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);
        }

        ssl_ = SSL_new(ssl_ctx_);
        if (!ssl_) {
            BOOST_LOG_TRIVIAL(error) << "OpenBambuMqtt: SSL_new failed";
            SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr;
            ::close(sock_fd_); sock_fd_ = -1;
            return -1;
        }

        SSL_set_fd(ssl_, sock_fd_);
        int rc = SSL_connect(ssl_);
        if (rc != 1) {
            int ssl_err = SSL_get_error(ssl_, rc);
            BOOST_LOG_TRIVIAL(error) << "OpenBambuMqtt: SSL_connect failed, ssl_error=" << ssl_err;
            SSL_free(ssl_); ssl_ = nullptr;
            SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr;
            ::close(sock_fd_); sock_fd_ = -1;
            return -1;
        }
    }

    connected_.store(true, std::memory_order_release);

    // Send MQTT CONNECT packet
    std::string connect_pkt = build_connect_packet(cfg);
    if (!send_raw(connect_pkt)) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuMqtt: failed to send CONNECT";
        disconnect();
        return -1;
    }

    // Read CONNACK (4 bytes: fixed header + remaining length + return code)
    char connack[4];
    if (!read_bytes(connack, 4, 10000)) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuMqtt: failed to read CONNACK";
        disconnect();
        return -1;
    }

    // Verify CONNACK: type=0x20, length=0x02, flags, rc
    if ((connack[0] & 0xF0) != 0x20 || connack[1] != 0x02) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuMqtt: invalid CONNACK response";
        disconnect();
        return -1;
    }

    int return_code = static_cast<uint8_t>(connack[3]);
    if (return_code != 0) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuMqtt: CONNACK rejected, rc=" << return_code;
        disconnect();
        return return_code;
    }

    BOOST_LOG_TRIVIAL(info) << "OpenBambuMqtt: connected successfully";

    // Start read loop
    running_.store(true, std::memory_order_release);
    read_thread_ = std::thread([this, keepalive = cfg.keepalive_s]() {
        read_loop();
    });

    // Notify callback
    {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        if (on_connect_) on_connect_(0);
    }

    return 0;
}

int OpenBambuMqtt::subscribe(const std::string& topic, int qos)
{
    if (!connected_.load(std::memory_order_acquire)) return -1;
    std::string pkt = build_subscribe_packet(topic, qos);
    return send_raw(pkt) ? 0 : -1;
}

int OpenBambuMqtt::publish(const std::string& topic, const std::string& payload, int qos)
{
    if (!connected_.load(std::memory_order_acquire)) return -1;
    std::string pkt = build_publish_packet(topic, payload, qos);
    return send_raw(pkt) ? 0 : -1;
}

void OpenBambuMqtt::disconnect()
{
    bool was_connected = connected_.exchange(false, std::memory_order_acq_rel);
    running_.store(false, std::memory_order_release);

    if (was_connected && sock_fd_ >= 0) {
        // Try to send DISCONNECT packet
        std::string pkt = build_disconnect_packet();
        // Best-effort send
        if (ssl_) {
            SSL_write(ssl_, pkt.data(), static_cast<int>(pkt.size()));
        } else if (sock_fd_ >= 0) {
            ::send(sock_fd_, pkt.data(), static_cast<int>(pkt.size()), 0);
        }
    }

    if (sock_fd_ >= 0) {
        ::shutdown(sock_fd_, SHUT_RDWR);
        ::close(sock_fd_);
        sock_fd_ = -1;
    }

    if (ssl_) { SSL_free(ssl_); ssl_ = nullptr; }
    if (ssl_ctx_) { SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr; }

    if (read_thread_.joinable()) {
        read_thread_.join();
    }

    if (was_connected) {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        if (on_disconnect_) on_disconnect_(0);
    }
}

void OpenBambuMqtt::read_loop()
{
    auto last_ping = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        // Check if we need to send a PING
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count() >= 30) {
            std::string ping = build_pingreq_packet();
            if (!send_raw(ping)) break;
            last_ping = now;
        }

        // Poll for incoming data
        struct pollfd pfd{};
        pfd.fd = sock_fd_;
        pfd.events = POLLIN;
#ifdef _WIN32
        int pr = WSAPoll(&pfd, 1, 1000);
#else
        int pr = ::poll(&pfd, 1, 1000);
#endif
        if (pr < 0) break;
        if (pr == 0) continue; // timeout, loop back

        // Read fixed header byte
        char hdr;
        if (!read_bytes(&hdr, 1, 5000)) break;

        // Decode remaining length (variable-length encoding)
        uint32_t remaining = 0;
        uint32_t multiplier = 1;
        for (int i = 0; i < 4; ++i) {
            char len_byte;
            if (!read_bytes(&len_byte, 1, 5000)) goto disconnected;
            remaining += (static_cast<uint8_t>(len_byte) & 0x7F) * multiplier;
            multiplier *= 128;
            if (!(static_cast<uint8_t>(len_byte) & 0x80)) break;
        }

        // Read the rest of the packet
        std::string body;
        if (remaining > 0) {
            if (remaining > 4 * 1024 * 1024) break; // Sanity limit
            body.resize(remaining);
            if (!read_bytes(body.data(), remaining, 30000)) break;
        }

        uint8_t pkt_type = (static_cast<uint8_t>(hdr) >> 4) & 0x0F;

        switch (pkt_type) {
        case 3: { // PUBLISH
            if (body.size() < 2) break;
            uint16_t topic_len = (static_cast<uint8_t>(body[0]) << 8) | static_cast<uint8_t>(body[1]);
            if (body.size() < 2u + topic_len) break;
            std::string topic = body.substr(2, topic_len);

            size_t payload_offset = 2 + topic_len;
            uint8_t qos_level = (static_cast<uint8_t>(hdr) >> 1) & 0x03;
            if (qos_level > 0) {
                payload_offset += 2; // Skip packet ID
                // Send PUBACK for QoS 1
                if (qos_level == 1 && body.size() >= 2u + topic_len + 2) {
                    uint16_t pid = (static_cast<uint8_t>(body[2 + topic_len]) << 8) |
                                    static_cast<uint8_t>(body[2 + topic_len + 1]);
                    std::string puback;
                    puback.push_back(0x40); // PUBACK
                    puback.push_back(0x02);
                    puback.push_back(static_cast<char>((pid >> 8) & 0xFF));
                    puback.push_back(static_cast<char>(pid & 0xFF));
                    send_raw(puback);
                }
            }

            std::string payload;
            if (payload_offset < body.size()) {
                payload = body.substr(payload_offset);
            }

            {
                std::lock_guard<std::mutex> lk(cb_mutex_);
                if (on_message_) on_message_(topic, payload);
            }
            break;
        }
        case 13: // PINGRESP
            break;
        case 9: // SUBACK
            break;
        case 4: // PUBACK
            break;
        default:
            break;
        }
    }

disconnected:
    bool was_connected = connected_.exchange(false, std::memory_order_acq_rel);
    if (was_connected) {
        BOOST_LOG_TRIVIAL(info) << "OpenBambuMqtt: connection lost";
        std::lock_guard<std::mutex> lk(cb_mutex_);
        if (on_disconnect_) on_disconnect_(-1);
    }
}

} // namespace Slic3r
