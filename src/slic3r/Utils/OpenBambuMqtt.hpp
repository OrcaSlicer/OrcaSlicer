#pragma once

// OpenBambuMqtt - Minimal MQTT 3.1.1 client over TLS for BambuLab LAN printing.
//
// Protocol knowledge derived from:
//   https://github.com/ClusterM/open-bamboo-networking (AGPL-3.0)
//   Copyright (C) 2026 Alexey Cluster and contributors
//   https://github.com/Doridian/OpenBambuAPI
//
// This is a clean-room implementation of the MQTT 3.1.1 subset required
// to communicate with BambuLab printers in LAN/Developer mode. It uses
// only OpenSSL (already a project dependency) for TLS, and implements
// the minimal MQTT protocol frames needed for CONNECT/SUBSCRIBE/PUBLISH.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Forward declarations for OpenSSL types
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace Slic3r {

/// Callback for connection status changes: (return_code)
/// 0 = connected, nonzero = error
using MqttConnectCb = std::function<void(int rc)>;

/// Callback for disconnect events: (return_code)
using MqttDisconnectCb = std::function<void(int rc)>;

/// Callback for incoming messages: (topic, payload)
using MqttMessageCb = std::function<void(const std::string& topic, const std::string& payload)>;

struct MqttConnectConfig {
    std::string host;
    int         port          = 8883;
    std::string username;
    std::string password;
    std::string client_id;
    bool        use_tls       = true;
    bool        tls_insecure  = true;  // Skip certificate verification (LAN mode)
    int         keepalive_s   = 60;
};

/// Minimal MQTT 3.1.1 client implementing only what BambuLab LAN mode needs:
/// CONNECT, SUBSCRIBE, PUBLISH, PINGREQ, and DISCONNECT.
class OpenBambuMqtt {
public:
    OpenBambuMqtt();
    ~OpenBambuMqtt();

    // Non-copyable
    OpenBambuMqtt(const OpenBambuMqtt&) = delete;
    OpenBambuMqtt& operator=(const OpenBambuMqtt&) = delete;

    void set_on_connect(MqttConnectCb cb);
    void set_on_disconnect(MqttDisconnectCb cb);
    void set_on_message(MqttMessageCb cb);

    /// Connect to the MQTT broker. Returns 0 on success, nonzero on failure.
    int connect(const MqttConnectConfig& cfg);

    /// Subscribe to a topic. Returns 0 on success.
    int subscribe(const std::string& topic, int qos = 0);

    /// Publish a message. Returns 0 on success.
    int publish(const std::string& topic, const std::string& payload, int qos = 0);

    /// Disconnect from broker.
    void disconnect();

    /// Check if connected.
    bool is_connected() const { return connected_.load(std::memory_order_acquire); }

private:
    void read_loop();
    bool send_raw(const std::string& data);
    std::string build_connect_packet(const MqttConnectConfig& cfg);
    std::string build_subscribe_packet(const std::string& topic, int qos);
    std::string build_publish_packet(const std::string& topic, const std::string& payload, int qos);
    std::string build_pingreq_packet();
    std::string build_disconnect_packet();
    std::string encode_remaining_length(uint32_t len);
    bool read_bytes(char* buf, size_t count, int timeout_ms = 10000);

    SSL_CTX*    ssl_ctx_ = nullptr;
    SSL*        ssl_     = nullptr;
    int         sock_fd_ = -1;

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::thread       read_thread_;
    std::mutex        write_mutex_;

    MqttConnectCb     on_connect_;
    MqttDisconnectCb  on_disconnect_;
    MqttMessageCb     on_message_;
    std::mutex        cb_mutex_;

    uint16_t          next_packet_id_ = 1;
};

} // namespace Slic3r
