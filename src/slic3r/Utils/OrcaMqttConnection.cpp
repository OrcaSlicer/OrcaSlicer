#include "OrcaMqttConnection.hpp"
#include "Http.hpp"
#include "PrinterCommLogger.hpp"

#include <async_mqtt/protocol/connection_status.hpp>
#include <async_mqtt/protocol/event/event_variant.hpp>
#include <async_mqtt/protocol/error.hpp>
#include <async_mqtt/protocol/event/close.hpp>
#include <async_mqtt/protocol/event/packet_id_released.hpp>
#include <async_mqtt/protocol/event/packet_received.hpp>
#include <async_mqtt/protocol/event/send.hpp>
#include <async_mqtt/protocol/event/timer.hpp>
#include <async_mqtt/protocol/packet/packet_variant.hpp>
#include <async_mqtt/protocol/packet/qos.hpp>
#include <async_mqtt/protocol/packet/topic_subopts.hpp>
#include <async_mqtt/protocol/packet/topic_sharename.hpp>
#include <async_mqtt/protocol/packet/v3_1_1_connack.hpp>
#include <async_mqtt/protocol/packet/v3_1_1_connect.hpp>
#include <async_mqtt/protocol/packet/v3_1_1_publish.hpp>
#include <async_mqtt/protocol/packet/v3_1_1_suback.hpp>
#include <async_mqtt/protocol/packet/v3_1_1_subscribe.hpp>
#include <async_mqtt/protocol/packet/v3_1_1_unsuback.hpp>
#include <async_mqtt/protocol/packet/v3_1_1_unsubscribe.hpp>
#include <async_mqtt/protocol/protocol_version.hpp>
#include <async_mqtt/protocol/role.hpp>
#include <async_mqtt/protocol/rv_connection.hpp>
#include <async_mqtt/protocol/timer.hpp>
#include <async_mqtt/util/overload.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/ssl/verify_mode.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/field.hpp>
#include <nlohmann/json.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/beast/websocket/stream_base.hpp>
#include <boost/log/trivial.hpp>
#include <boost/system/error_code.hpp>

#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <streambuf>
#include <istream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace Slic3r {

namespace am        = async_mqtt;
namespace net       = boost::asio;
namespace beast     = boost::beast;
namespace websocket = boost::beast::websocket;
using tcp           = boost::asio::ip::tcp;

namespace {
// The handlers of one OrcaMqttConnection run on its worker thread; stop() and start()
// check this to avoid joining the thread from itself.
thread_local const void* t_worker_of = nullptr;

std::atomic<int> g_live_sessions{0};

// Deadline for the connect, TLS and WebSocket handshakes and the CONNACK.
constexpr std::chrono::seconds kSetupTimeout{10};

// The least time a broker is given to answer a PINGREQ.
constexpr std::chrono::seconds kMinPingRespTimeout{5};

// The longest stop() waits for queued requests to be sent.
constexpr std::chrono::seconds kStopFlushTimeout{1};

// A packet to write: the MQTT packet and the buffers that point into it. Never moved
// once built, and kept until the write completes.
struct OutPacket {
    explicit OutPacket(am::packet_variant pv) : packet(std::move(pv)), buffers(packet.const_buffer_sequence()) {}
    am::packet_variant           packet;
    std::vector<net::const_buffer> buffers;
};

// Follows the MQTT framing of the received byte stream only as far as each packet's
// declared length, so an absurd declaration is refused before the MQTT connection
// allocates a buffer for it.
class PacketSizeGuard
{
public:
    // False when a packet declares more than kMaxPacketBytes, or a malformed length.
    bool feed(std::string_view bytes)
    {
        std::size_t i = 0;
        while (i < bytes.size()) {
            if (m_state == State::Payload) {
                const std::size_t skipped = std::min(m_remaining, bytes.size() - i);
                m_remaining -= skipped;
                i += skipped;
                if (m_remaining == 0)
                    m_state = State::Header;
                continue;
            }
            const auto byte = static_cast<std::uint8_t>(bytes[i++]);
            if (m_state == State::Header) {
                m_state     = State::Length;
                m_remaining = 0;
                m_scale     = 1;
                m_length_bytes = 0;
                continue;
            }
            m_remaining += static_cast<std::size_t>(byte & 0x7f) * m_scale;
            m_scale *= 128;
            ++m_length_bytes;
            if ((byte & 0x80) == 0) {
                if (1 + m_length_bytes + m_remaining > kMaxPacketBytes)
                    return false;
                m_state = m_remaining == 0 ? State::Header : State::Payload;
            } else if (m_length_bytes == 4) {
                return false;
            }
        }
        return true;
    }

private:
    // MQTT permits ~256 MiB; nothing this client receives is close.
    static constexpr std::size_t kMaxPacketBytes = 16 * 1024 * 1024;
    enum class State { Header, Length, Payload };
    State       m_state = State::Header;
    std::size_t m_remaining = 0;
    std::size_t m_scale = 1;
    std::size_t m_length_bytes = 0;
};

std::string report_topic(const std::string& dev_id) { return "device/" + dev_id + "/report"; }
std::string request_topic(const std::string& dev_id) { return "device/" + dev_id + "/request"; }

// Reads a block of memory as a stream, without copying it.
struct MemoryBuffer : std::streambuf {
    MemoryBuffer(char* first, std::size_t size) { setg(first, first, first + size); }
};

// The device of a "device/<id>/..." topic, empty for any other topic.
std::string device_of(const std::string& topic)
{
    constexpr std::size_t id_start = 7; // "device/"
    const std::size_t     id_end   = topic.rfind('/');
    if (topic.rfind("device/", 0) == 0 && id_end != std::string::npos && id_end > id_start)
        return topic.substr(id_start, id_end - id_start);
    return {};
}

// The cloud form of the connection is the one that authenticates with a bearer token.
const char* transport_of(const OrcaMqttConnection::Config& config) { return config.bearer_provider ? "cloud-mqtt" : "lan-mqtt"; }

#ifdef ORCA_PRINTER_COMM_LOG
// Connection lifecycle as seen from this client, next to the plugin-reported events for Bambu printers.
void comm_lifecycle(const char* name, const OrcaMqttConnection::Config& config, nlohmann::json fields)
{
    fields["transport"] = transport_of(config);
    PrinterCommLogger::instance().log_event("lifecycle", name, std::move(fields));
}
#endif
} // namespace


// One connection attempt: resolve, connect, TLS, WebSocket upgrade, then the MQTT
// session. Everything but the constructor runs on the worker thread inside
// io.run(); other threads reach it only through io.post() and io.stop().
struct OrcaMqttConnection::Session
{
    using TlsWebSocket   = websocket::stream<net::ssl::stream<beast::tcp_stream>>;
    using PlainWebSocket = websocket::stream<beast::tcp_stream>;
    using MqttConnection = am::rv_connection<am::role::client>;

    explicit Session(OrcaMqttConnection& owner_)
        : owner(owner_)
        , config(owner_.current_config)
        , resolver(io)
        , ping_timer(io)
        , pingresp_timer(io)
        , mqtt(am::protocol_version::v3_1_1)
    {
        // PUBACK a QoS 1 report (and PUBREC a QoS 2 one) through the normal send path, so the broker's in-flight window does not fill.
        mqtt.set_auto_pub_response(true);
        ++g_live_sessions;
    }
    ~Session() { --g_live_sessions; }
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    template<class F> void with_ws(F&& f)
    {
        if (wss)
            f(*wss);
        else if (ws)
            f(*ws);
    }

    // CONNACK accepted; the MQTT connection tracks it.
    bool established() const { return mqtt.get_connection_status() == am::connection_status::connected; }

    // The session is over, or the owner was asked to stop.
    bool dead() const { return closing || owner.stopping.load(); }

    void begin()
    {
        if (!OrcaMqttConnection::parse_endpoint(config.url, endpoint))
            throw std::runtime_error("invalid Orca Cloud WebSocket endpoint");
        // The Host header of an IPv6 literal is bracketed; the resolver and the TLS checks take it bare.
        host_header = endpoint.host.find(':') == std::string::npos ? endpoint.host : "[" + endpoint.host + "]";
        if (config.bearer_provider)
            token = config.bearer_provider();
        if (config.use_tls) {
            ssl_ctx.emplace(net::ssl::context::tls_client);
            if (!config.ca_file.empty())
                ssl_ctx->load_verify_file(config.ca_file);
            else
                ssl_ctx->set_default_verify_paths();
            Http::add_platform_root_certificates(ssl_ctx->native_handle());
            wss.emplace(io, *ssl_ctx);
        } else {
            ws.emplace(io);
        }
        resolver.async_resolve(endpoint.host, endpoint.port, [this](const boost::system::error_code& ec, tcp::resolver::results_type found) {
            if (dead())
                return;
            if (ec)
                return fail(ec, "resolve Orca MQTT host");
            endpoints = std::move(found);
            with_ws([this](auto& stream) {
                auto& lowest = beast::get_lowest_layer(stream);
                lowest.expires_after(kSetupTimeout);
                lowest.async_connect(endpoints, [this](const boost::system::error_code& connect_error, const tcp::endpoint&) { on_connect(connect_error); });
            });
        });
    }

    void on_connect(const boost::system::error_code& ec)
    {
        if (dead())
            return;
        if (ec)
            return fail(ec, "connect to Orca MQTT host");
        if (!wss)
            return upgrade();
        // Set SNI before the TLS handshake so the cloud edge selects the correct certificate.
        auto& tls = wss->next_layer();
        if (!SSL_set_tlsext_host_name(tls.native_handle(), endpoint.host.c_str()))
            return fail_message("failed to set Orca Cloud TLS server name");
        tls.set_verify_mode(net::ssl::verify_peer);
        tls.set_verify_callback(net::ssl::host_name_verification(endpoint.host));
        tls.async_handshake(net::ssl::stream_base::client, [this](const boost::system::error_code& tls_error) {
            if (dead())
                return;
            if (tls_error)
                return fail(tls_error, "Orca MQTT TLS handshake");
            upgrade();
        });
    }

    void upgrade()
    {
        with_ws([this](auto& stream) {
            stream.binary(true);
            stream.set_option(websocket::stream_base::decorator([bearer = token](websocket::request_type& request) {
                request.set(beast::http::field::user_agent, "OrcaSlicer");
                if (!bearer.empty())
                    request.set(beast::http::field::authorization, "Bearer " + bearer);
                request.set("Sec-WebSocket-Protocol", "mqtt");
            }));
            stream.async_handshake(upgrade_response, host_header, endpoint.target, [this](const boost::system::error_code& ec) { on_upgrade(ec); });
        });
    }

    // One PINGREQ every half keepalive, as long as the session lasts.
    std::chrono::seconds ping_interval() const
    {
        return std::chrono::seconds(std::max(1, std::clamp(config.keepalive_seconds, 0, 65535) / 2));
    }

    void on_upgrade(const boost::system::error_code& ec)
    {
        if (dead())
            return;
        if (ec)
            return fail(ec, "Orca WebSocket handshake");
        if (upgrade_response["Sec-WebSocket-Protocol"] != "mqtt")
            return fail_message("Orca WebSocket did not negotiate MQTT");
        do_read();
        // Auth precedence: a bearer_provider authenticates the WebSocket upgrade, so the
        // CONNECT username/password fields are omitted entirely (the cloud form).
        std::optional<std::string> username;
        std::optional<std::string> password;
        if (!config.bearer_provider && !config.username.empty()) {
            username = config.username;
            if (!config.password.empty())
                password = config.password;
        }
        // The connection defaults to the keepalive as its ping interval and asks for that on
        // every send; a set interval is kept, and CONNACK starts the timer.
        if (config.keepalive_seconds > 0) {
            handle_events(mqtt.set_pingreq_send_interval(ping_interval()));
            // A broker that stays silent past this is gone, though the socket looks open.
            mqtt.set_pingresp_recv_timeout(std::max<std::chrono::milliseconds>(ping_interval(), kMinPingRespTimeout));
        }
        handle_events(mqtt.send(am::v3_1_1::connect_packet{
            true, // clean session
            static_cast<std::uint16_t>(std::clamp(config.keepalive_seconds, 0, 65535)),
            config.client_id.empty() ? std::string("OrcaSlicer") : config.client_id,
            std::move(username), std::move(password)}));
    }

    void do_read()
    {
        with_ws([this](auto& stream) {
            stream.async_read(read_buffer, [this](const boost::system::error_code& ec, std::size_t) {
                if (dead())
                    return;
                if (ec)
                    return fail(ec, "read Orca MQTT message");
                // A WebSocket message holds an arbitrary byte range of the MQTT stream:
                // part of a packet, or several. The MQTT connection reassembles it.
                const auto  message = read_buffer.data(); // contiguous
                char* const first   = const_cast<char*>(static_cast<const char*>(message.data()));
                if (!size_guard.feed(std::string_view(first, message.size())))
                    return fail_message("Orca MQTT packet too large");
                MemoryBuffer view(first, message.size());
                std::istream bytes(&view);
                while (!dead() && bytes.rdbuf()->in_avail() > 0)
                    handle_events(mqtt.recv(bytes));
                read_buffer.consume(read_buffer.size());
                if (!dead())
                    do_read();
            });
        });
    }

    void do_write()
    {
        if (writing || outgoing.empty() || dead())
            return;
        writing = true;
        with_ws([this](auto& stream) {
            stream.async_write(outgoing.front()->buffers, [this](const boost::system::error_code& ec, std::size_t) {
                writing = false;
                if (dead())
                    return;
                if (ec)
                    return fail(ec, "write Orca MQTT message");
                outgoing.pop_front();
                do_write();
                report_if_flushed();
            });
        });
    }

    void handle_events(std::vector<am::event_variant> events)
    {
        bool close_requested = false;
        for (am::event_variant& event : events) {
            if (dead())
                return;
            std::visit(am::overload{
                           [&](const am::error_code& ec) { fail(ec, "Orca MQTT protocol error"); },
                           [&](am::event::send& send) {
                               outgoing.push_back(std::make_unique<OutPacket>(std::move(send.get())));
                               do_write();
                           },
                           [&](const am::event::packet_id_released&) {},
                           [&](am::event::packet_received& received) { on_packet(received.get()); },
                           [&](const am::event::timer& timer) { on_timer(timer); },
                           [&](const am::event::close&) { close_requested = true; }},
                       event);
        }
        if (close_requested && !dead())
            fail_message("Orca MQTT connection closed");
    }

    void on_packet(am::packet_variant& packet)
    {
        packet.visit(am::overload{
            [&](const am::v3_1_1::connack_packet& connack) { on_connack(static_cast<int>(connack.code())); },
            [&](const am::v3_1_1::suback_packet& suback) { on_suback(suback); },
            [&](am::v3_1_1::publish_packet& publish) {
                // topic is "device/<id>/report" (or "/request"); hand the id up, drop anything else.
                const std::string dev_id = device_of(publish.topic());
                if (!dev_id.empty() && owner.on_message)
                    owner.on_message(dev_id, publish.payload());
            },
            [](const auto&) {} // UNSUBACK, PINGRESP: nothing to do
        });
    }

    void on_connack(int rc)
    {
        owner.m_last_connack_rc.store(rc);
        PRINTER_COMM_LOG(comm_lifecycle("mqtt_connack", config, {{"url", config.url}, {"rc", rc}}));
        BOOST_LOG_TRIVIAL(info) << "OrcaMqtt mqtt_connack: transport=" << transport_of(config) << " url=" << config.url << " rc=" << rc;
        if (rc != 0) {
            // Bad credentials / not authorized: retrying cannot help.
            terminal = rc == 4 || rc == 5;
            return fail_message("Orca MQTT CONNECT refused rc=" + std::to_string(rc));
        }

        // Beast leaves the setup deadline armed; disable it for the long-lived session.
        with_ws([](auto& stream) { beast::get_lowest_layer(stream).expires_never(); });
        // The subscription acknowledgements belong to this MQTT session. Clear the previous
        // session's state before notifying the owner, because the reconnect callback
        // immediately queues the printer's initial requests.
        {
            std::lock_guard<std::mutex> lock(owner.mutex);
            for (auto& subscription : owner.subscriptions)
                subscription.second = Subscription{};
            owner.requests.erase(std::remove_if(owner.requests.begin(), owner.requests.end(), [](const Request& request) { return !request.held; }),
                                 owner.requests.end());
        }
        owner.notify_state(true);
        if (dead())
            return;
        if (config.keepalive_seconds > 0)
            handle_events(mqtt.set_pingreq_send_interval(ping_interval()));
        flush_subscriptions();
    }

    void on_timer(const am::event::timer& timer)
    {
        if (timer.get_kind() == am::timer_kind::pingresp_recv) {
            // Started by the first unanswered PINGREQ and ended by a PINGRESP, whatever is sent meanwhile.
            if (timer.get_op() == am::timer_op::cancel) {
                ++pingresp_generation;
                pingresp_timer.cancel();
                pingresp_armed = false;
            } else if (!pingresp_armed && timer.get_ms()) {
                pingresp_armed = true;
                pingresp_timer.expires_after(*timer.get_ms());
                pingresp_timer.async_wait([this, generation = pingresp_generation](const boost::system::error_code& ec) {
                    if (!ec && !dead() && generation == pingresp_generation)
                        fail_message("Orca MQTT broker did not answer a PINGREQ");
                });
            }
            return;
        }
        // A cancel of the ping timer only comes with the end of the session, which cancels it itself.
        // Every packet sent asks for the timer to restart; the cadence here is fixed, so
        // only a timer that is not already running is started.
        if (timer.get_kind() != am::timer_kind::pingreq_send || timer.get_op() != am::timer_op::reset)
            return;
        if (!established() || ping_armed || !timer.get_ms())
            return;
        ping_armed = true;
        ping_timer.expires_after(*timer.get_ms());
        ping_timer.async_wait([this](const boost::system::error_code& ec) {
            if (ec || dead())
                return;
            ping_armed = false;
            handle_events(mqtt.notify_timer_fired(am::timer_kind::pingreq_send));
        });
    }

    // stop() asked for what is queued to be sent first: queue what other threads asked for and
    // tell stop() when everything is written.
    void flush_and_report()
    {
        flush_subscriptions();
        publish_requests();
        flush_wanted = true;
        report_if_flushed();
    }

    void report_if_flushed()
    {
        if (!flush_wanted || !outgoing.empty())
            return;
        std::lock_guard<std::mutex> lock(owner.mutex);
        owner.flushed = true;
        owner.state_cv.notify_all();
    }

    // SUBSCRIBE every subscription that is Wanted: all of them at the start of a session, a new one later.
    void flush_subscriptions()
    {
        if (!established() || dead())
            return;
        std::vector<std::pair<std::uint16_t, std::string>> subscribes;
        {
            std::lock_guard<std::mutex> lock(owner.mutex);
            for (auto& [topic, subscription] : owner.subscriptions) {
                if (subscription.state != SubscriptionState::Wanted)
                    continue;
                const auto packet_id = mqtt.acquire_unique_packet_id();
                if (!packet_id)
                    return fail_message("Orca MQTT packet identifiers exhausted");
                subscription = Subscription{SubscriptionState::Sent, *packet_id};
                subscribes.emplace_back(*packet_id, topic);
            }
        }
        for (const auto& [packet_id, topic] : subscribes)
            handle_events(mqtt.send(am::v3_1_1::subscribe_packet{packet_id, {{topic, am::qos::at_least_once}}}));
    }

    void unsubscribe(const std::string& topic)
    {
        if (!established() || dead())
            return;
        {
            // Subscribed again since: the SUBSCRIBE is already on its way or sent, and must stand.
            std::lock_guard<std::mutex> lock(owner.mutex);
            if (owner.subscriptions.count(topic) != 0)
                return;
        }
        const auto packet_id = mqtt.acquire_unique_packet_id();
        if (!packet_id)
            return fail_message("Orca MQTT packet identifiers exhausted");
        handle_events(mqtt.send(am::v3_1_1::unsubscribe_packet{*packet_id, {am::topic_sharename{topic}}}));
    }

    void on_suback(const am::v3_1_1::suback_packet& suback)
    {
        // MQTT grants QoS 0 or 1 for a requested QoS 1 subscription; 0x80 means it was rejected.
        const std::uint8_t result  = suback.entries().empty() ? 0x80 : static_cast<std::uint8_t>(suback.entries().front());
        const bool         granted = result == 0 || result == 1;
        std::string        topic;
        std::size_t        released = 0;
        {
            std::lock_guard<std::mutex> lock(owner.mutex);
            // An acknowledgement of a subscription that was withdrawn or replaced matches no
            // pending packet identifier and is ignored.
            for (auto& [name, subscription] : owner.subscriptions) {
                if (subscription.state == SubscriptionState::Sent && subscription.packet_id == suback.packet_id()) {
                    topic              = name;
                    subscription.state = granted ? SubscriptionState::Granted : SubscriptionState::Rejected;
                    break;
                }
            }
            // Requests held behind the subscription go out with it, or are dropped with it.
            for (auto it = owner.requests.begin(); !topic.empty() && it != owner.requests.end();) {
                if (it->held && report_topic(it->dev_id) == topic) {
                    ++released;
                    if (granted) {
                        it->held = false;
                        ++it;
                    } else {
                        it = owner.requests.erase(it);
                    }
                } else {
                    ++it;
                }
            }
        }
        if (topic.empty())
            return;
        PRINTER_COMM_LOG(comm_lifecycle("mqtt_suback", config,
                                        {{"dev_id", device_of(topic)}, {"result", result}, {"granted", granted}, {"queued_requests", released}}));
        BOOST_LOG_TRIVIAL(info) << "OrcaMqtt mqtt_suback: transport=" << transport_of(config) << " dev_id=" << device_of(topic)
                                << " result=" << static_cast<int>(result) << " granted=" << granted << " queued_requests=" << released;
        publish_requests();
    }

    // Publish the requests that are not held, in the order they were made.
    void publish_requests()
    {
        for (;;) {
            if (!established() || dead())
                return;
            Request request;
            {
                std::lock_guard<std::mutex> lock(owner.mutex);
                const auto ready = std::find_if(owner.requests.begin(), owner.requests.end(), [](const Request& r) { return !r.held; });
                if (ready == owner.requests.end())
                    return;
                request = std::move(*ready);
                owner.requests.erase(ready);
            }
            handle_events(mqtt.send(am::v3_1_1::publish_packet{request_topic(request.dev_id), request.payload, am::qos::at_most_once}));
            // Every request this client publishes passes here, including the agents' own (subscription setup,
            // pushing stop, file requests), and a held one when it is released.
            PRINTER_COMM_LOG(PrinterCommLogger::instance().log_mqtt("send", transport_of(config), request.dev_id,
                                                                    PrinterCommLogger::body_for_log(request.payload, 1024 * 1024), 0, 0));
            BOOST_LOG_TRIVIAL(debug) << "OrcaMqtt send: transport=" << transport_of(config) << " dev_id=" << request.dev_id << " bytes=" << request.payload.size();
        }
    }

    void fail(const boost::system::error_code& ec, const char* what)
    {
        if (closing)
            return;
        clean_close = ec == websocket::error::closed;
        fail_message(std::string(what) + ": " + ec.message());
    }

    void fail_message(std::string message)
    {
        if (closing)
            return;
        error = std::move(message);
        shutdown();
    }

    // Close the transport and make io.run() return.
    void shutdown()
    {
        closing = true;
        ping_timer.cancel();
        pingresp_timer.cancel();
        resolver.cancel();
        with_ws([](auto& stream) { beast::get_lowest_layer(stream).close(); });
        io.stop();
    }

    // Run the cancellations the close caused, so no handler stays queued.
    void finish()
    {
        shutdown();
        io.restart();
        io.poll();
    }

    OrcaMqttConnection& owner;
    const Config        config;
    Endpoint            endpoint;
    std::string         host_header;
    std::string         token;
    // What a pending operation reads or writes outlives io, whose teardown completes the operations
    // still queued on it; the sockets below are destroyed before it.
    tcp::resolver::results_type      endpoints;
    beast::flat_buffer               read_buffer;
    beast::http::response<beast::http::string_body> upgrade_response;
    std::deque<std::unique_ptr<OutPacket>> outgoing;
    net::io_context                  io;
    std::optional<net::ssl::context> ssl_ctx;
    tcp::resolver                    resolver;
    net::steady_timer                ping_timer;
    net::steady_timer                pingresp_timer;
    std::optional<TlsWebSocket>      wss;
    std::optional<PlainWebSocket>    ws;
    MqttConnection                   mqtt;
    PacketSizeGuard                  size_guard;
    bool                             writing     = false;
    bool                             flush_wanted = false; // stop() waits for the queued writes
    bool                             ping_armed  = false;
    bool                             pingresp_armed = false;
    unsigned                         pingresp_generation = 0; // an expiry already queued when a PINGRESP came is stale
    bool                             terminal    = false;
    bool                             clean_close = false;
    bool                             closing     = false;
    std::string                      error;
};

bool OrcaMqttConnection::parse_endpoint(const std::string& url, Endpoint& endpoint)
{
    std::string rest;
    std::string default_port;
    if      (url.rfind("wss://", 0) == 0) { rest = url.substr(6); default_port = "443"; }
    else if (url.rfind("ws://",  0) == 0) { rest = url.substr(5); default_port = "80";  }
    else return false;

    const auto slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    endpoint.target = (slash == std::string::npos) ? "/" : rest.substr(slash);

    // host[:port], with an IPv6 literal in brackets ("[::1]:8883"); a host with several colons and
    // no brackets is a bare IPv6 literal and has no port. The host is kept without brackets.
    endpoint.port = default_port;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos)
            return false;
        endpoint.host     = authority.substr(1, close - 1);
        const auto after  = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':')
                return false;
            endpoint.port = after.substr(1);
        }
    } else if (std::count(authority.begin(), authority.end(), ':') == 1) {
        const auto colon = authority.find(':');
        endpoint.host    = authority.substr(0, colon);
        endpoint.port    = authority.substr(colon + 1);
    } else {
        endpoint.host = authority;
    }
    return !endpoint.host.empty() && !endpoint.port.empty() && !endpoint.target.empty();
}

int OrcaMqttConnection::live_sessions() { return g_live_sessions.load(); }

OrcaMqttConnection::~OrcaMqttConnection() { stop(); }

bool OrcaMqttConnection::start(const Config& config, MessageHandler on_message, StateHandler on_state)
{
    if (t_worker_of == this)
        return false; // a handler cannot restart the connection it runs on
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
        stop_and_join();
        {
            std::lock_guard<std::mutex> lock(mutex);
            current_config    = config;
            this->on_message  = std::move(on_message);
            this->on_state    = std::move(on_state);
            initial_result    = false;
            initial_completed = false;
            connected         = false;
            m_last_connack_rc.store(-1);
        }
        stopping.store(false);
        worker = std::thread(&OrcaMqttConnection::run, this);
    }

    // Wait without holding lifecycle_mutex so a concurrent stop() can cancel the
    // attempt and signal completion instead of blocking until the timeout.
    std::unique_lock<std::mutex> lock(mutex);
    if (!state_cv.wait_for(lock, std::chrono::seconds(10), [this] { return initial_completed; })) {
        initial_completed = true;
        initial_result    = false;
    }
    return initial_result;
}

void OrcaMqttConnection::signal_stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping.store(true);
        connected = false;
        if (!initial_completed) {
            initial_completed = true;
            initial_result    = false;
        }
        // Under the lock, without a reference of its own: the worker holds the last one, so a
        // session is never torn down on a caller's thread.
        if (active_session)
            active_session->io.stop();
    }
    state_cv.notify_all();
}

void OrcaMqttConnection::stop_and_join()
{
    signal_stop();
    if (worker.joinable())
        worker.join();
    std::lock_guard<std::mutex> lock(mutex);
    connected = false; // a CONNACK that landed while stopping may have set it again
    requests.clear();
}

void OrcaMqttConnection::stop()
{
    if (t_worker_of == this) {
        // Called from a handler: the worker leaves its loop when the handler returns,
        // and the next stop() or start() joins it.
        signal_stop();
        return;
    }
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
    flush_queued();
    stop_and_join();
}

// Give an established session the chance to send what has been queued (a last request, an
// UNSUBSCRIBE) before it is closed, for no longer than kStopFlushTimeout.
void OrcaMqttConnection::flush_queued()
{
    std::unique_lock<std::mutex> lock(mutex);
    if (!active_session || stopping.load())
        return;
    flushed = false;
    net::post(active_session->io, [weak = std::weak_ptr<Session>(active_session)] {
        if (const auto alive = weak.lock())
            alive->flush_and_report();
    });
    state_cv.wait_for(lock, kStopFlushTimeout, [this] { return flushed || stopping.load() || !active_session; });
}

bool OrcaMqttConnection::is_running() const
{
    return !stopping.load();
}

void OrcaMqttConnection::post_to_session(std::function<void(Session&)> work)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!active_session)
        return; // no session yet: the worker sends everything once the session is up
    net::post(active_session->io, [weak = std::weak_ptr<Session>(active_session), work = std::move(work)] {
        if (const auto alive = weak.lock())
            work(*alive);
    });
}

bool OrcaMqttConnection::subscribe(const std::string& dev_id)
{
    if (dev_id.empty())
        return false;
    const std::string topic = report_topic(dev_id);
    if (topic.size() > 96) // MQTT topic filter cap enforced by the service
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto existing = subscriptions.find(topic);
        if (existing != subscriptions.end() && existing->second.state != SubscriptionState::Rejected)
            return true;
        subscriptions[topic] = Subscription{};
    }
    post_to_session([](Session& session) { session.flush_subscriptions(); }); // SUBSCRIBE now, without a reconnect
    return true;
}

bool OrcaMqttConnection::unsubscribe(const std::string& dev_id)
{
    const std::string topic = report_topic(dev_id);
    {
        std::lock_guard<std::mutex> lock(mutex);
        // A SUBACK still on its way for this topic then matches nothing and is ignored.
        subscriptions.erase(topic);
        requests.erase(std::remove_if(requests.begin(), requests.end(), [&](const Request& request) { return request.held && request.dev_id == dev_id; }),
                       requests.end());
    }
    post_to_session([topic](Session& session) { session.unsubscribe(topic); }); // a clean session has nothing to unsubscribe from when disconnected
    return true;
}

void OrcaMqttConnection::clear_subscriptions()
{
    std::lock_guard<std::mutex> lock(mutex);
    subscriptions.clear();
    requests.erase(std::remove_if(requests.begin(), requests.end(), [](const Request& request) { return request.held; }), requests.end());
}

bool OrcaMqttConnection::send_request(const std::string& dev_id, const std::string& payload)
{
    if (dev_id.empty() || !connected.load())
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!active_session)
            return false;
        const auto subscription = subscriptions.find(report_topic(dev_id));
        if (subscription != subscriptions.end() && subscription->second.state == SubscriptionState::Rejected) {
            // The broker refused this device's subscription: nothing will ever release the request.
            BOOST_LOG_TRIVIAL(warning) << "OrcaMqtt send refused: transport=" << transport_of(current_config) << " dev_id=" << dev_id
                                       << " reason=subscription rejected";
            return false;
        }
        // Held until the SUBACK: otherwise an immediate pushall response can be published by the
        // broker before this client is actually subscribed to the report topic.
        requests.push_back(Request{dev_id, payload, subscription != subscriptions.end() && subscription->second.state != SubscriptionState::Granted});
    }
    post_to_session([](Session& session) { session.publish_requests(); });
    return true;
}

// Runs one connection attempt to its end; false when stop() came before it began.
bool OrcaMqttConnection::run_session(const std::shared_ptr<Session>& session)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping.load())
            return false;
        active_session = session;
    }
    try {
        session->begin();
        session->io.run();
    } catch (const std::exception& error) {
        // Also an exception thrown by a handler: the session is dropped and retried.
        session->fail_message(error.what());
    } catch (...) {
        session->fail_message("unknown exception");
    }
    try {
        session->finish();
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaMqtt closing a session failed";
    }
    std::lock_guard<std::mutex> lock(mutex);
    active_session.reset();
    state_cv.notify_all(); // a stop() waiting for this session to flush
    return true;
}

void OrcaMqttConnection::notify_state(bool is_now_connected)
{
    StateHandler callback;
    bool initial = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        // Atomic with signal_stop(): once stopping, a CONNACK in flight is not reported.
        if (is_now_connected && stopping.load())
            return;
        connected = is_now_connected;
        initial   = !initial_completed;
        if (initial) {
            initial_result    = is_now_connected;
            initial_completed = true;
        }
        callback = on_state;
    }
    if (initial)
        state_cv.notify_all();
    else if (callback)
        callback(is_now_connected, false);
}

void OrcaMqttConnection::run()
{
    t_worker_of = this;
    try {
        run_attempts();
    } catch (...) {
        // Nothing may leave the worker thread; the connection is over.
        BOOST_LOG_TRIVIAL(error) << "OrcaMqtt worker stopped by an exception";
        stopping.store(true);
    }
    t_worker_of = nullptr;
}

void OrcaMqttConnection::run_attempts()
{
    int           retry_seconds = 1;
    std::uint64_t attempt       = 0;
    while (!stopping.load()) {
        ++attempt;
        bool        established = false;
        bool        terminal    = false;
        bool        clean_close = false;
        std::string error       = "cannot create a session";
        {
            std::shared_ptr<Session> session;
            try {
                session = std::make_shared<Session>(*this);
            } catch (const std::exception& e) {
                error = e.what(); // a failed attempt like any other
            }
            if (session) {
                if (!run_session(session))
                    break;
                established = session->established();
                terminal    = session->terminal;
                clean_close = session->clean_close;
                error       = session->error;
            }
        } // the session is released here, on the worker
        if (terminal)
            stopping.store(true); // run() leaves the loop and start() learns the attempt is over
        else if (stopping.load())
            break;
        // The backoff grows across attempts that never reached CONNACK; a session that did
        // starts over from the shortest wait.
        if (established)
            retry_seconds = 1;
        if (clean_close) {
            PRINTER_COMM_LOG(comm_lifecycle("mqtt_closed", current_config, nlohmann::json::object()));
            BOOST_LOG_TRIVIAL(info) << "OrcaMqtt mqtt_closed: transport=" << transport_of(current_config) << " url=" << current_config.url;
        } else if (!error.empty() && !terminal) {
            PRINTER_COMM_LOG(comm_lifecycle("mqtt_error", current_config, {{"attempt", attempt}, {"error", error}, {"retry_in_s", retry_seconds}}));
            BOOST_LOG_TRIVIAL(warning) << "OrcaMqtt mqtt_error: transport=" << transport_of(current_config) << " attempt=" << attempt
                                       << " error=" << error << " retry_in_s=" << retry_seconds;
        }
        try {
            notify_state(false);
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "OrcaMqtt the state handler threw";
        }
        if (terminal)
            break;
        {
            std::unique_lock<std::mutex> lock(mutex);
            state_cv.wait_for(lock, retry_wait ? retry_wait(retry_seconds) : std::chrono::milliseconds(retry_seconds * 1000),
                              [this] { return stopping.load(); });
        }
        retry_seconds = std::min(retry_seconds * 2, 30);
    }
}

} // namespace Slic3r
