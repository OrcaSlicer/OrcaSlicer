#include "MqttWebSocketClient.hpp"

#include "AsioUtils.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/beast/websocket/stream_base.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r::Mqtt {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t max_remaining_length = 268435455;
constexpr std::size_t max_buffer_size      = 1024 * 1024;

void append_u16(Bytes& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

std::uint16_t read_u16(const std::uint8_t* bytes) { return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]); }

void append_string(Bytes& body, const std::string& value)
{
    if (value.size() > 65535)
        throw std::length_error("MQTT string exceeds its two-byte length");
    append_u16(body, static_cast<std::uint16_t>(value.size()));
    body.insert(body.end(), value.begin(), value.end());
}

void check_packet_id(std::uint16_t packet_id)
{
    if (packet_id == 0)
        throw std::invalid_argument("MQTT packet identifier must be nonzero");
}

Bytes frame(std::uint8_t header, Bytes body)
{
    assert(body.size() <= max_remaining_length);
    std::size_t remaining = body.size();
    std::uint8_t prefix[5]{header};
    std::size_t size = 1;
    do {
        const auto digit = static_cast<std::uint8_t>(remaining % 128);
        remaining /= 128;
        prefix[size++] = remaining ? static_cast<std::uint8_t>(digit | 0x80) : digit;
    } while (remaining);
    body.insert(body.begin(), prefix, prefix + size);
    return body;
}

} // namespace

Bytes encode_connect(const std::string& client_id, const std::string& user, const std::string& password, std::uint16_t keepalive_s)
{
    Bytes body{0, 4, 'M', 'Q', 'T', 'T', 4, 0xc2};
    append_u16(body, keepalive_s);
    append_string(body, client_id);
    append_string(body, user);
    append_string(body, password);
    return frame(0x10, std::move(body));
}

Bytes encode_subscribe(std::uint16_t packet_id, const std::vector<std::string>& topics)
{
    check_packet_id(packet_id);
    if (topics.empty())
        throw std::invalid_argument("MQTT SUBSCRIBE requires a topic filter");

    Bytes body;
    append_u16(body, packet_id);
    for (const auto& topic : topics) {
        if (topic.empty())
            throw std::invalid_argument("MQTT topic filter must be nonempty");
        append_string(body, topic);
        body.push_back(1);
        if (body.size() > max_remaining_length)
            throw std::length_error("MQTT SUBSCRIBE exceeds the maximum remaining length");
    }
    return frame(0x82, std::move(body));
}

Bytes encode_publish(const std::string& topic, const std::string& payload)
{
    if (topic.empty())
        throw std::invalid_argument("MQTT topic name must be nonempty");
    Bytes body;
    append_string(body, topic);
    if (payload.size() > max_remaining_length - body.size())
        throw std::length_error("MQTT PUBLISH exceeds the maximum remaining length");
    body.insert(body.end(), payload.begin(), payload.end());
    return frame(0x30, std::move(body));
}

Bytes encode_puback(std::uint16_t packet_id)
{
    check_packet_id(packet_id);
    Bytes bytes{0x40, 2};
    append_u16(bytes, packet_id);
    return bytes;
}

Bytes encode_disconnect() { return {0xe0, 0}; }

void PacketDecoder::feed(const std::uint8_t* data, std::size_t size)
{
    if (m_failed || size == 0)
        return;
    if (size > max_buffer_size - m_buffer.size()) {
        m_failed = true;
        return;
    }
    m_buffer.insert(m_buffer.end(), data, data + size);
    if (m_header_size == 0)
        read_header();
}

void PacketDecoder::read_header()
{
    m_header_size      = 0;
    std::size_t length = 0;
    for (std::size_t i = 1; i < m_buffer.size() && i <= 4; ++i) {
        const auto digit = m_buffer[i];
        length |= std::size_t(digit & 0x7f) << (7 * (i - 1));
        if ((digit & 0x80) == 0) {
            m_header_size = i + 1;
            m_body_size   = length;
            m_failed      = length > max_buffer_size - m_header_size;
            return;
        }
        if (i == 4)
            m_failed = true;
    }
}

std::optional<Packet> PacketDecoder::next()
{
    if (m_failed || m_header_size == 0 || m_buffer.size() < m_header_size + m_body_size)
        return std::nullopt;
    const auto header = m_buffer.front();
    const auto body   = m_buffer.begin() + m_header_size;
    const auto end    = body + m_body_size;
    Packet packet{static_cast<std::uint8_t>(header >> 4), static_cast<std::uint8_t>(header & 0x0f), Bytes(body, end)};
    m_buffer.erase(m_buffer.begin(), end);
    read_header();
    return packet;
}

std::optional<Publish> parse_publish(const Packet& p)
{
    const unsigned qos = (p.flags >> 1) & 3;
    if (p.type != 3 || qos == 3 || (qos == 0 && (p.flags & 8)) || p.body.size() < 2)
        return std::nullopt;
    const std::size_t topic_size = read_u16(p.body.data());
    if (topic_size == 0 || topic_size > p.body.size() - 2)
        return std::nullopt;
    std::size_t offset = 2 + topic_size;
    std::optional<std::uint16_t> packet_id;
    if (qos != 0) {
        if (p.body.size() - offset < 2)
            return std::nullopt;
        packet_id = read_u16(p.body.data() + offset);
        if (*packet_id == 0)
            return std::nullopt;
        offset += 2;
    }
    return Publish{std::string(p.body.begin() + 2, p.body.begin() + 2 + topic_size), std::string(p.body.begin() + offset, p.body.end()),
                   packet_id};
}

} // namespace Slic3r::Mqtt

namespace Slic3r {
namespace net       = boost::asio;
namespace beast     = boost::beast;
namespace websocket = beast::websocket;
using tcp           = net::ip::tcp;

struct MqttWebSocketClient::Impl
{
    using Clock   = std::chrono::steady_clock;
    using Results = tcp::resolver::results_type;
    enum class State { Fresh, Connected, Closed };

    net::io_context ioc;
    websocket::stream<beast::tcp_stream> ws{ioc};
    net::steady_timer timer;
    std::atomic<bool> timed_out{false};
    State state = State::Fresh;
    Mqtt::PacketDecoder decoder;
    std::uint16_t packet_id = 0;
    std::deque<Mqtt::Publish> pending;
    std::size_t pending_bytes = 0;

    explicit Impl(Clock::time_point deadline) : timer(ioc, deadline)
    {
        timer.async_wait([this](beast::error_code ec) {
            if (!ec) {
                timed_out = true;
                close_socket();
            }
        });
    }

    void close_socket()
    {
        beast::error_code ignored;
        ws.next_layer().socket().close(ignored);
    }

    Status fail(Status reason)
    {
        state = State::Closed;
        close_socket();
        return reason;
    }

    Status status(beast::error_code ec = {}, Status failure = Status::Closed)
    {
        if (Clock::now() >= timer.expiry() || ec == beast::error::timeout)
            return fail(Status::Timeout);
        return ec ? fail(failure) : Status::Ok;
    }

    Status resolve(const std::string& host, const std::string& port, Results& endpoints)
    {
        beast::error_code ec;
        const auto address = net::ip::make_address(host, ec);
        if (!ec) {
            unsigned short number;
            const auto end    = port.data() + port.size();
            const auto parsed = std::from_chars(port.data(), end, number);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
                return fail(Status::ConnectFailed);
            endpoints = Results::create(tcp::endpoint(address, number), host, port);
            return Status::Ok;
        }

        using Reply = std::pair<beast::error_code, Results>;
        auto reply  = std::make_shared<std::promise<Reply>>();
        auto result = reply->get_future();
        try {
            // getaddrinfo cannot be cancelled, so a detached lookup lets the session return at its deadline.
            // Ceiling: one short-lived thread per host name lookup, and a hung DNS lookup can outlive this call.
            std::thread([host, port, reply] {
                try {
                    net::io_context lookup_ioc;
                    tcp::resolver resolver{lookup_ioc};
                    beast::error_code error;
                    auto addresses = resolver.resolve(host, port, error);
                    reply->set_value({error, std::move(addresses)});
                } catch (...) {
                    reply->set_exception(std::current_exception());
                }
            }).detach();
            if (result.wait_until(timer.expiry()) != std::future_status::ready)
                return fail(Status::Timeout);
            std::tie(ec, endpoints) = result.get();
        } catch (const std::exception&) {
            return fail(Status::ConnectFailed);
        }
        return status(ec, Status::ConnectFailed);
    }

    Status write(const std::vector<std::uint8_t>& bytes)
    {
        if (auto result = status(); result != Status::Ok)
            return result;
        return status(run_until_done(ioc, [&](auto done) { ws.async_write(net::buffer(bytes), done); }));
    }

    Status read(Mqtt::Packet& packet)
    {
        std::array<std::uint8_t, 8192> bytes;
        for (;;) {
            if (auto result = status(); result != Status::Ok)
                return result;
            if (auto next = decoder.next()) {
                packet = std::move(*next);
                return Status::Ok;
            }
            if (decoder.failed())
                return fail(Status::ProtocolError);
            std::size_t size = 0;
            const auto ec    = run_until_done(ioc, [&](auto done) {
                ws.async_read_some(net::buffer(bytes), [&, done](beast::error_code error, std::size_t n) {
                    size = n;
                    done(error);
                });
            });
            if (auto result = status(ec); result != Status::Ok)
                return result;
            if (!ws.got_binary())
                return fail(Status::ProtocolError);
            decoder.feed(bytes.data(), size);
        }
    }

    Status receive_publish(const Mqtt::Packet& packet, Mqtt::Publish& message)
    {
        auto parsed = Mqtt::parse_publish(packet);
        if (!parsed || (packet.flags & 6) == 4) // Subscriptions request at most QoS 1.
            return fail(Status::ProtocolError);
        message = std::move(*parsed);
        return message.packet_id ? write(Mqtt::encode_puback(*message.packet_id)) : Status::Ok;
    }
};

MqttWebSocketClient::MqttWebSocketClient(std::chrono::steady_clock::time_point deadline) : m_impl(std::make_unique<Impl>(deadline)) {}
MqttWebSocketClient::~MqttWebSocketClient() { close(); }

MqttWebSocketClient::Status MqttWebSocketClient::connect(const std::string& host,
                                                         const std::string& port,
                                                         const std::string& client_id,
                                                         const std::string& user,
                                                         const std::string& password,
                                                         std::uint8_t& connack_code)
{
    auto& session = *m_impl;
    if (session.state != Impl::State::Fresh)
        return Status::Closed;
    if (host.empty() || port.empty())
        return session.fail(Status::ConnectFailed);
    if (auto result = session.status(); result != Status::Ok)
        return result;

    const auto request = Mqtt::encode_connect(client_id, user, password, 30);
    Impl::Results endpoints;
    if (auto result = session.resolve(host, port, endpoints); result != Status::Ok)
        return result;
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(session.timer.expiry() - Impl::Clock::now());
    auto ec              = connect_until_done(session.ioc, session.ws.next_layer(), endpoints, remaining, session.timed_out);
    if (auto result = session.status(ec, Status::ConnectFailed); result != Status::Ok)
        return result;

    session.ws.next_layer().expires_never();
    session.ws.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) { req.set(beast::http::field::sec_websocket_protocol, "mqtt"); }));
    const std::string authority = (host.find(':') == std::string::npos ? host : "[" + host + "]") + ":" + port;
    websocket::response_type response;
    ec = run_until_done(session.ioc, [&](auto done) { session.ws.async_handshake(response, authority, "/", done); });
    if (auto result = session.status(ec, Status::HandshakeFailed); result != Status::Ok)
        return result;
    if (response[beast::http::field::sec_websocket_protocol] != "mqtt")
        return session.fail(Status::HandshakeFailed);

    session.ws.binary(true);
    if (auto result = session.write(request); result != Status::Ok)
        return result;
    Mqtt::Packet ack;
    if (auto result = session.read(ack); result != Status::Ok)
        return result;
    if (ack.type != 2 || ack.flags != 0 || ack.body.size() != 2 || ack.body[0] != 0 || ack.body[1] > 5)
        return session.fail(Status::ProtocolError);
    connack_code = ack.body[1];
    if (connack_code != 0)
        return session.fail(Status::ConnackRejected);
    session.state = Impl::State::Connected;
    return Status::Ok;
}

MqttWebSocketClient::Status MqttWebSocketClient::subscribe(const std::vector<std::string>& topics)
{
    auto& session = *m_impl;
    if (session.state != Impl::State::Connected)
        return Status::Closed;
    if (++session.packet_id == 0)
        ++session.packet_id;
    if (auto result = session.write(Mqtt::encode_subscribe(session.packet_id, topics)); result != Status::Ok)
        return result;
    Mqtt::Packet ack;
    for (;;) {
        if (auto result = session.read(ack); result != Status::Ok)
            return result;
        if (ack.type != 3)
            break;
        // MQTT permits matching publications before SUBACK. Acknowledge now and deliver in wire order.
        Mqtt::Publish message;
        if (auto result = session.receive_publish(ack, message); result != Status::Ok)
            return result;
        session.pending_bytes += sizeof(message) + message.topic.size() + message.payload.size();
        if (session.pending_bytes > Mqtt::max_buffer_size)
            return session.fail(Status::ProtocolError);
        session.pending.push_back(std::move(message));
    }
    if (ack.type != 9 || ack.flags != 0 || ack.body.size() != topics.size() + 2 || Mqtt::read_u16(ack.body.data()) != session.packet_id)
        return session.fail(Status::ProtocolError);
    bool rejected = false;
    for (std::size_t i = 2; i < ack.body.size(); ++i) {
        if (ack.body[i] == 0x80)
            rejected = true;
        else if (ack.body[i] > 1)
            return session.fail(Status::ProtocolError);
    }
    return rejected ? Status::SubscribeRejected : Status::Ok;
}

MqttWebSocketClient::Status MqttWebSocketClient::publish(const std::string& topic, const std::string& payload)
{
    if (m_impl->state != Impl::State::Connected)
        return Status::Closed;
    return m_impl->write(Mqtt::encode_publish(topic, payload));
}

MqttWebSocketClient::Status MqttWebSocketClient::wait_for(const std::function<bool(const std::string&, const std::string&)>& accept,
                                                          std::string& payload_out)
{
    auto& session = *m_impl;
    if (session.state != Impl::State::Connected)
        return Status::Closed;
    for (;;) {
        if (auto result = session.status(); result != Status::Ok)
            return result;
        Mqtt::Publish message;
        if (!session.pending.empty()) {
            message = std::move(session.pending.front());
            session.pending.pop_front();
            session.pending_bytes -= sizeof(message) + message.topic.size() + message.payload.size();
        } else {
            Mqtt::Packet packet;
            if (auto result = session.read(packet); result != Status::Ok)
                return result;
            if (auto result = session.receive_publish(packet, message); result != Status::Ok)
                return result;
        }
        if (accept(message.topic, message.payload)) {
            payload_out = std::move(message.payload);
            return Status::Ok;
        }
    }
}

void MqttWebSocketClient::close()
{
    auto& session = *m_impl;
    if (session.state == Impl::State::Connected && session.write(Mqtt::encode_disconnect()) == Status::Ok) {
        // MQTT 3.1.1 lets the client close the connection after DISCONNECT, so the close echo gets only a short wait.
        // The session deadline still closes the socket first when it comes sooner.
        session.ws.next_layer().expires_after(std::chrono::milliseconds(250));
        run_until_done(session.ioc, [&](auto done) { session.ws.async_close(websocket::close_code::normal, done); });
    }
    session.state = Impl::State::Closed;
    session.close_socket();
}

} // namespace Slic3r
