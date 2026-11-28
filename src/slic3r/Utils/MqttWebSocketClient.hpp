#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::Mqtt {

// Callers supply MQTT topic names, filters and UTF-8 strings. Fields that exceed
// their wire length throw std::length_error. Invalid identifiers throw std::invalid_argument.
std::vector<std::uint8_t> encode_connect(const std::string& client_id,
                                         const std::string& user,
                                         const std::string& password,
                                         std::uint16_t keepalive_s);
std::vector<std::uint8_t> encode_subscribe(std::uint16_t packet_id, const std::vector<std::string>& topics);
std::vector<std::uint8_t> encode_publish(const std::string& topic, const std::string& payload);
std::vector<std::uint8_t> encode_puback(std::uint16_t packet_id);
std::vector<std::uint8_t> encode_disconnect();

struct Packet
{
    std::uint8_t type  = 0;
    std::uint8_t flags = 0;
    std::vector<std::uint8_t> body;
};

class PacketDecoder
{
public:
    void feed(const std::uint8_t* data, std::size_t size);
    std::optional<Packet> next();
    bool failed() const { return m_failed; }

private:
    void read_header();

    std::deque<std::uint8_t> m_buffer;
    std::size_t m_header_size = 0;
    std::size_t m_body_size   = 0;
    bool m_failed             = false;
};

struct Publish
{
    std::string topic;
    std::string payload;
    std::optional<std::uint16_t> packet_id;
};

std::optional<Publish> parse_publish(const Packet& p);

} // namespace Slic3r::Mqtt

namespace Slic3r {

// One MQTT session. Its absolute deadline covers connection setup, all requests and shutdown.
class MqttWebSocketClient
{
public:
    enum class Status { Ok, Timeout, ConnectFailed, HandshakeFailed, ConnackRejected, SubscribeRejected, ProtocolError, Closed };

    explicit MqttWebSocketClient(std::chrono::steady_clock::time_point deadline);
    ~MqttWebSocketClient();
    Status connect(const std::string& host,
                   const std::string& port,
                   const std::string& client_id,
                   const std::string& user,
                   const std::string& password,
                   std::uint8_t& connack_code);
    Status subscribe(const std::vector<std::string>& topics);
    Status publish(const std::string& topic, const std::string& payload);
    Status wait_for(const std::function<bool(const std::string& topic, const std::string& payload)>& accept, std::string& payload_out);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r
