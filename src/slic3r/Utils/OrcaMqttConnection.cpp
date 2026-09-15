#include "OrcaMqttConnection.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Slic3r {

struct OrcaMqttConnection::Connection {
    boost::asio::io_context io_context;
    boost::asio::ssl::context ssl_context;
    boost::asio::ip::tcp::resolver resolver;
    // Exactly one of these is engaged once ws_handshake() has run: wss for
    // wss:// endpoints, ws for plaintext ws://.
    std::optional<TlsWebSocket>   wss;
    std::optional<PlainWebSocket> ws;

    Connection()
        : ssl_context(boost::asio::ssl::context::tls_client)
        , resolver(io_context)
    {}
};

namespace {
// Apply / clear a tcp_stream timeout on whichever websocket is engaged.
// Templated on the connection type only because Connection is a private nested
// type: a deduced parameter needs no (inaccessible) name for it.
template<class Conn> void expires_after(Conn& conn, std::chrono::seconds timeout) {
    if (conn.wss)     boost::beast::get_lowest_layer(*conn.wss).expires_after(timeout);
    else if (conn.ws) boost::beast::get_lowest_layer(*conn.ws).expires_after(timeout);
}
template<class Conn> void expires_never(Conn& conn) {
    if (conn.wss)     boost::beast::get_lowest_layer(*conn.wss).expires_never();
    else if (conn.ws) boost::beast::get_lowest_layer(*conn.ws).expires_never();
}
} // namespace

OrcaMqttConnection::~OrcaMqttConnection() { stop(); }

bool OrcaMqttConnection::start(const Config& config, MessageHandler on_message, StateHandler on_state) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex);
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT start url=" << config.url
                            << " use_tls=" << config.use_tls
                            << " bearer_provider=" << (config.bearer_provider ? "set" : "null")
                            << " username_present=" << (!config.username.empty())
                            << " password_present=" << (!config.password.empty())
                            << " client_id=" << config.client_id
                            << " keepalive_seconds=" << config.keepalive_seconds
                            << " message_callback=" << (on_message ? "set" : "null")
                            << " state_callback=" << (on_state ? "set" : "null");
    stop();
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

    std::unique_lock<std::mutex> lock(mutex);
    if (!initial_cv.wait_for(lock, std::chrono::seconds(10), [this] { return initial_completed; })) {
        initial_completed = true;
        initial_result    = false;
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: MQTT initial connection timed out after 10 seconds"
                                   << " url=" << current_config.url
                                   << " last_connack_rc=" << m_last_connack_rc.load()
                                   << " connected=" << connected.load()
                                   << "; worker will retry";
    }
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT start initial_result=" << initial_result
                            << " initial_completed=" << initial_completed
                            << " last_connack_rc=" << m_last_connack_rc.load()
                            << " worker_running=" << (worker.joinable() && !stopping.load());
    return initial_result;
}

void OrcaMqttConnection::stop() {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex);
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT stop requested"
                            << " url=" << current_config.url
                            << " connected=" << connected.load()
                            << " worker_joinable=" << worker.joinable()
                            << " last_connack_rc=" << m_last_connack_rc.load();
    stopping.store(true);
    state_cv.notify_all();
    {
        std::lock_guard<std::mutex> lock(connection_mutex);
        if (active_connection) {
            // Generic so it accepts either the TLS or the plaintext websocket.
            auto shutdown_socket = [](auto& websocket) {
                auto& socket = boost::beast::get_lowest_layer(websocket).socket();
                boost::system::error_code socket_error;
                socket.cancel(socket_error);
                socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, socket_error);
                socket.close(socket_error);
            };
            if (active_connection->wss)
                shutdown_socket(*active_connection->wss);
            else if (active_connection->ws)
                shutdown_socket(*active_connection->ws);
            active_connection->resolver.cancel();
        }
    }
    if (worker.joinable())
        worker.join();

    {
        std::lock_guard<std::mutex> lock(mutex);
        connected = false;
        acknowledged_subscriptions.clear();
        pending_subscribe_packets.clear();
        pending_requests.clear();
        if (!initial_completed) {
            initial_completed = true;
            initial_result    = false;
        }
    }
    initial_cv.notify_all();
}

bool OrcaMqttConnection::is_running() const {
    return worker.joinable() && !stopping.load();
}

void OrcaMqttConnection::flush_subscription_change() {
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(connection_mutex);
        conn = active_connection;
    }
    bool connacked;
    {
        std::lock_guard<std::mutex> lock(mutex);
        connacked = connected;
    }
    if (!conn || !connacked)
        return; // no live MQTT session yet — the worker sends the set on CONNACK

    // beast permits a concurrent writer while the worker is blocked in
    // websocket.read(); every write is serialised by write_mutex inside ws_write().
    try {
        send_pending_subscriptions(*conn);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: direct subscription write failed (" << e.what()
                                   << "); worker will resend the full set on reconnect";
    }
}

bool OrcaMqttConnection::subscribe(const std::string& dev_id) {
    if (dev_id.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: MQTT subscribe rejected empty dev_id";
        return false;
    }
    const std::string topic = report_topic(dev_id);
    if (topic.size() > 96) { // MQTT topic filter cap enforced by the service
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: MQTT subscribe rejected oversized topic=" << topic;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (subscriptions.count(topic) != 0 && pending_unsubscriptions.count(topic) == 0) {
            BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT subscribe already queued or active topic=" << topic
                                    << " acknowledged=" << (acknowledged_subscriptions.count(topic) != 0);
            return true;
        }
        subscriptions.insert(topic);
        pending_unsubscriptions.erase(topic);
        pending_subscriptions.insert(topic);
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT subscribe queued topic=" << topic
                                << " total_subscriptions=" << subscriptions.size()
                                << " connected=" << connected.load();
    }
    state_cv.notify_all();
    flush_subscription_change(); // emit SUBSCRIBE now on the live socket (no reconnect)
    return true;
}

bool OrcaMqttConnection::unsubscribe(const std::string& dev_id) {
    const std::string topic = report_topic(dev_id);
    {
        std::lock_guard<std::mutex> lock(mutex);
        subscriptions.erase(topic);
        acknowledged_subscriptions.erase(topic);
        pending_subscriptions.erase(topic);
        pending_unsubscriptions.insert(topic);
        for (auto it = pending_subscribe_packets.begin(); it != pending_subscribe_packets.end();) {
            if (it->second == topic)
                it = pending_subscribe_packets.erase(it);
            else
                ++it;
        }
        for (auto it = pending_requests.begin(); it != pending_requests.end();) {
            if (it->first == dev_id)
                it = pending_requests.erase(it);
            else
                ++it;
        }
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT unsubscribe queued topic=" << topic
                                << " total_subscriptions=" << subscriptions.size()
                                << " connected=" << connected.load();
    }
    state_cv.notify_all();
    flush_subscription_change(); // emit UNSUBSCRIBE now on the live socket (no reconnect)
    return true;
}

void OrcaMqttConnection::clear_subscriptions() {
    std::lock_guard<std::mutex> lock(mutex);
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT clear subscriptions count=" << subscriptions.size();
    subscriptions.clear();
    pending_subscriptions.clear();
    pending_unsubscriptions.clear();
    acknowledged_subscriptions.clear();
    pending_subscribe_packets.clear();
    pending_requests.clear();
}

bool OrcaMqttConnection::parse_endpoint(const std::string& url, Endpoint& endpoint) {
    std::string rest;
    std::string default_port;
    if      (url.rfind("wss://", 0) == 0) { rest = url.substr(6); default_port = "443"; }
    else if (url.rfind("ws://",  0) == 0) { rest = url.substr(5); default_port = "80";  }
    else return false;

    const auto slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    endpoint.target = (slash == std::string::npos) ? "/" : rest.substr(slash);

    // host[:port] — leave an unbracketed IPv6 literal alone
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        endpoint.host = authority.substr(0, colon);
        endpoint.port = authority.substr(colon + 1);
    } else {
        endpoint.host = authority;
        endpoint.port = default_port;
    }
    return !endpoint.host.empty() && !endpoint.port.empty() && !endpoint.target.empty();
}

void OrcaMqttConnection::append_string(std::vector<uint8_t>& packet, const std::string& value) {
    if (value.size() > 0xffff)
        throw std::runtime_error("MQTT string is too long");
    packet.push_back(static_cast<uint8_t>(value.size() >> 8));
    packet.push_back(static_cast<uint8_t>(value.size() & 0xff));
    packet.insert(packet.end(), value.begin(), value.end());
}

void OrcaMqttConnection::prepend_remaining_length(std::vector<uint8_t>& packet, size_t length) {
    std::vector<uint8_t> encoded;
    do {
        uint8_t byte = static_cast<uint8_t>(length % 128);
        length /= 128;
        if (length != 0)
            byte |= 0x80;
        encoded.push_back(byte);
    } while (length != 0);
    packet.insert(packet.begin() + 1, encoded.begin(), encoded.end());
}

std::vector<uint8_t> OrcaMqttConnection::make_connect_packet(
    const std::string& client_id, const std::string& username,
    const std::string& password, int keepalive_seconds) {
    std::vector<uint8_t> packet{0x10};
    append_string(packet, "MQTT");
    packet.push_back(4); // protocol level 3.1.1

    uint8_t flags = 0x02; // clean session
    if (!username.empty()) { flags |= 0x80; if (!password.empty()) flags |= 0x40; }
    packet.push_back(flags);

    packet.push_back(static_cast<uint8_t>(keepalive_seconds >> 8));
    packet.push_back(static_cast<uint8_t>(keepalive_seconds & 0xff));

    append_string(packet, client_id.empty() ? "OrcaSlicer" : client_id);
    if (!username.empty()) {
        append_string(packet, username);
        if (!password.empty()) append_string(packet, password);
    }
    prepend_remaining_length(packet, packet.size() - 1);
    return packet;
}

std::string OrcaMqttConnection::report_topic(const std::string& device_id) { return "device/" + device_id + "/report"; }

std::string OrcaMqttConnection::request_topic(const std::string& id) { return "device/" + id + "/request"; }

std::vector<uint8_t> OrcaMqttConnection::make_publish_packet(const std::string& topic, const std::string& payload) {
    std::vector<uint8_t> packet{0x30}; // PUBLISH, QoS 0, no retain
    append_string(packet, topic);      // no packet id at QoS 0
    packet.insert(packet.end(), payload.begin(), payload.end());
    prepend_remaining_length(packet, packet.size() - 1);
    return packet;
}

std::vector<uint8_t> OrcaMqttConnection::make_subscribe_packet(uint16_t id, const std::string& topic, uint8_t qos) {
    std::vector<uint8_t> packet{0x82};
    packet.push_back(id >> 8); packet.push_back(id & 0xff);
    append_string(packet, topic);
    packet.push_back(qos);
    prepend_remaining_length(packet, packet.size() - 1);
    return packet;
}

std::vector<uint8_t> OrcaMqttConnection::make_unsubscribe_packet(uint16_t id, const std::string& topic) {
    std::vector<uint8_t> packet{0xA2};
    packet.push_back(id >> 8); packet.push_back(id & 0xff);
    append_string(packet, topic);
    prepend_remaining_length(packet, packet.size() - 1);
    return packet;
}

std::vector<uint8_t> OrcaMqttConnection::make_ping_packet() { return {0xc0, 0}; }

void OrcaMqttConnection::ws_write(Connection& conn, const std::vector<uint8_t>& packet) {
    if (packet.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: attempted to send empty MQTT packet";
        return;
    }
    // Writes come from the worker thread AND, for dynamic (un)subscribes, the
    // caller thread. Serialise them; the worker's concurrent read is fine (beast
    // allows one reader + one writer).
    std::lock_guard<std::mutex> lock(write_mutex);
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: sending MQTT packet type=0x" << std::hex
                            << static_cast<unsigned int>(packet[0] >> 4) << std::dec
                            << " bytes=" << packet.size();
    if (conn.wss) {
        conn.wss->binary(true);
        conn.wss->write(boost::asio::buffer(packet));
    } else if (conn.ws) {
        conn.ws->binary(true);
        conn.ws->write(boost::asio::buffer(packet));
    }
}

std::size_t OrcaMqttConnection::ws_read(Connection& conn, boost::beast::flat_buffer& buffer,
                                        boost::system::error_code& ec) {
    if (conn.wss)
        return conn.wss->read(buffer, ec);
    if (conn.ws)
        return conn.ws->read(buffer, ec);
    ec = boost::asio::error::not_connected;
    return 0;
}

void OrcaMqttConnection::ws_close(Connection& conn) {
    boost::system::error_code close_error;
    if (conn.wss)
        conn.wss->close(boost::beast::websocket::close_code::normal, close_error);
    else if (conn.ws)
        conn.ws->close(boost::beast::websocket::close_code::normal, close_error);
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT connection closed code=" << close_error.value()
                            << " message=" << close_error.message();
}

void OrcaMqttConnection::ws_handshake(Connection& conn, const Config& config, const Endpoint& endpoint) {
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: WebSocket resolve starting host=" << endpoint.host
                            << " port=" << endpoint.port << " target=" << endpoint.target
                            << " tls=" << config.use_tls;
    const auto results = conn.resolver.resolve(endpoint.host, endpoint.port);
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT DNS resolution succeeded host=" << endpoint.host;

    std::string token;
    if (config.bearer_provider) {
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: requesting bearer token for WebSocket upgrade";
        token = config.bearer_provider();
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: bearer token callback completed token_present=" << !token.empty();
    }
    auto decorator = [token](boost::beast::websocket::request_type& request) {
        request.set(boost::beast::http::field::user_agent, "OrcaSlicer");
        if (!token.empty())
            request.set(boost::beast::http::field::authorization, "Bearer " + token);
        request.set("Sec-WebSocket-Protocol", "mqtt");
    };
    boost::beast::http::response<boost::beast::http::string_body> response;
    boost::system::error_code handshake_error;

    if (config.use_tls) {
        // stop() inspects the engaged optional under connection_mutex; publish it
        // under the same lock, then release before the blocking connect.
        {
            std::lock_guard<std::mutex> lock(connection_mutex);
            conn.wss.emplace(conn.io_context, conn.ssl_context);
        }
        auto& websocket = *conn.wss;
        auto& stream    = boost::beast::get_lowest_layer(websocket);
        stream.expires_after(std::chrono::seconds(10));
        stream.connect(results);
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT TCP connection established host=" << endpoint.host
                                << " port=" << endpoint.port;
        // Set SNI before the TLS handshake so the cloud edge selects the correct
        // certificate.
        auto& tls_stream = websocket.next_layer();
        if (!SSL_set_tlsext_host_name(tls_stream.native_handle(), endpoint.host.c_str()))
            throw std::runtime_error("failed to set Orca Cloud TLS server name");
        conn.ssl_context.set_default_verify_paths();
        tls_stream.set_verify_mode(boost::asio::ssl::verify_peer);
        tls_stream.set_verify_callback(boost::asio::ssl::host_name_verification(endpoint.host));
        tls_stream.handshake(boost::asio::ssl::stream_base::client);
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT TLS handshake completed host=" << endpoint.host;
        websocket.set_option(boost::beast::websocket::stream_base::decorator(decorator));
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: sending TLS WebSocket upgrade target=" << endpoint.target
                                << " bearer_header=" << (!token.empty());
        websocket.handshake(response, endpoint.host, endpoint.target, handshake_error);
    } else {
        {
            std::lock_guard<std::mutex> lock(connection_mutex);
            conn.ws.emplace(conn.io_context);
        }
        auto& websocket = *conn.ws;
        auto& stream    = boost::beast::get_lowest_layer(websocket);
        stream.expires_after(std::chrono::seconds(10));
        stream.connect(results);
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT TCP connection established host=" << endpoint.host
                                << " port=" << endpoint.port << " (plaintext)";
        websocket.set_option(boost::beast::websocket::stream_base::decorator(decorator));
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: sending plaintext WebSocket upgrade target=" << endpoint.target
                                << " bearer_header=" << (!token.empty());
        websocket.handshake(response, endpoint.host, endpoint.target, handshake_error);
    }

    if (handshake_error) {
        // Surface the server's HTTP status so a persistent rejection (stale token,
        // missing api key, wrong route) is diagnosable from the log rather than an
        // opaque "handshake declined".
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: WS handshake rejected http="
                                   << response.result_int() << " (" << response.reason() << "), "
                                   << handshake_error.message();
        throw boost::system::system_error(handshake_error, "Orca WebSocket handshake");
    }
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: WebSocket handshake completed http=" << response.result_int()
                            << " negotiated_protocol=" << response["Sec-WebSocket-Protocol"];
    if (response["Sec-WebSocket-Protocol"] != "mqtt") {
        BOOST_LOG_TRIVIAL(error) << "Orca diagnostic: WebSocket handshake did not negotiate MQTT";
        throw std::runtime_error("Orca WebSocket did not negotiate MQTT");
    }
}

bool OrcaMqttConnection::send_request(const std::string& dev_id, const std::string& payload) {
    if (dev_id.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: MQTT send_request rejected empty dev_id";
        return false;
    }
    if (!connected.load()) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: MQTT send_request rejected because connection is not ready"
                                   << " dev_id=" << dev_id << " last_connack_rc=" << m_last_connack_rc.load();
        return false;
    }
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(connection_mutex);
        conn = active_connection;
    }
    if (!conn) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: MQTT send_request rejected because active connection is null"
                                   << " dev_id=" << dev_id;
        return false;
    }
    const std::string report = report_topic(dev_id);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (subscriptions.count(report) != 0 && acknowledged_subscriptions.count(report) == 0) {
            pending_requests.emplace_back(dev_id, payload);
            BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT request queued until SUBACK"
                                    << " dev_id=" << dev_id << " payload_bytes=" << payload.size()
                                    << " pending_requests=" << pending_requests.size();
            return true;
        }
    }
    try {
        // ws_write() serialises the write via write_mutex; do not lock it here.
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT PUBLISH request dev_id=" << dev_id
                                << " topic=" << request_topic(dev_id)
                                << " payload_bytes=" << payload.size();
        ws_write(*conn, make_publish_packet(request_topic(dev_id), payload));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: send_request failed dev_id=" << dev_id
                                   << " (" << e.what() << ")";
        return false;
    }
    return true;
}

void OrcaMqttConnection::connect_and_read() {
    m_connection_stage = "creating connection";
    auto connection = std::make_shared<Connection>();
    {
        std::lock_guard<std::mutex> lock(connection_mutex);
        active_connection = connection;
        if (stopping.load())
            return;
    }

    m_connection_stage = "parsing endpoint";
    Endpoint endpoint;
    if (!parse_endpoint(current_config.url, endpoint)) {
        BOOST_LOG_TRIVIAL(error) << "Orca diagnostic: invalid MQTT endpoint=" << current_config.url;
        throw std::runtime_error("invalid Orca Cloud WebSocket endpoint");
    }

    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT connecting host=" << endpoint.host
                            << " port=" << endpoint.port << " target=" << endpoint.target;

    m_connection_stage = "WebSocket handshake";
    ws_handshake(*connection, current_config, endpoint);

    m_connection_stage = "sending MQTT CONNECT";
    expires_never(*connection);
    // Auth precedence: a bearer_provider authenticates the WebSocket upgrade, so the
    // CONNECT username/password fields are omitted entirely (the cloud form).
    const bool use_bearer = static_cast<bool>(current_config.bearer_provider);
    ws_write(*connection, make_connect_packet(current_config.client_id,
                                              use_bearer ? std::string() : current_config.username,
                                              use_bearer ? std::string() : current_config.password,
                                              current_config.keepalive_seconds));
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT CONNECT packet sent";

    m_connection_stage = "waiting for MQTT CONNACK";
    boost::beast::flat_buffer buffer;
    expires_after(*connection, std::chrono::seconds(10));
    boost::system::error_code connack_error;
    ws_read(*connection, buffer, connack_error);
    if (connack_error)
        throw boost::system::system_error(connack_error, "read Orca MQTT CONNACK");
    const std::string connack = boost::beast::buffers_to_string(buffer.data());
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT CONNACK received bytes=" << connack.size()
                            << " header=" << (connack.empty() ? -1 : static_cast<int>(static_cast<uint8_t>(connack[0])))
                            << " return_code=" << (connack.size() > 3 ? static_cast<int>(static_cast<uint8_t>(connack[3])) : -1);
    // rc: 0 accepted, 1..5 refusal, -1 malformed/not a CONNACK.
    const int rc = (connack.size() == 4 && static_cast<uint8_t>(connack[0]) == 0x20)
                       ? static_cast<int>(static_cast<uint8_t>(connack[3]))
                       : -1;
    m_last_connack_rc.store(rc);
    if (rc != 0) {
        BOOST_LOG_TRIVIAL(error) << "Orca diagnostic: MQTT CONNECT refused rc=" << rc;
        if (rc == 4 || rc == 5) {
            // Bad credentials / not authorized — retrying cannot help. Make run()'s
            // loop exit and unblock any waiting start().
            stopping.store(true);
            {
                std::lock_guard<std::mutex> lock(mutex);
                initial_completed = true;
                initial_result    = false;
            }
            initial_cv.notify_all();
        }
        throw std::runtime_error("Orca MQTT CONNECT refused rc=" + std::to_string(rc));
    }

    m_connection_stage = "reading MQTT messages";
    // The subscription acknowledgement belongs to this MQTT session. Clear
    // the previous session's state before notifying the owner, because the
    // reconnect callback immediately queues the printer's initial requests.
    {
        std::lock_guard<std::mutex> lock(mutex);
        acknowledged_subscriptions.clear();
        pending_subscribe_packets.clear();
    }
    notify_state(true);
    reconnect_delay_seconds.store(1); // a fresh CONNACK resets the backoff
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT connection is ready; sending current subscriptions";
    send_current_subscriptions(*connection);
    std::chrono::steady_clock::time_point next_ping = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (!stopping.load()) {
        send_pending_subscriptions(*connection);
        // Keepalive is driven every iteration, not only from the read-timeout branch:
        // a printer pushing faster than the 1s read deadline would otherwise keep the
        // read hot and the broker would drop us at 1.5 x keepalive.
        if (std::chrono::steady_clock::now() >= next_ping) {
            ws_write(*connection, make_ping_packet());
            BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT PINGREQ sent";
            next_ping = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        }
        buffer.consume(buffer.size());
        m_connection_stage = "reading MQTT frame";
        expires_after(*connection, std::chrono::seconds(1));
        boost::system::error_code error;
        ws_read(*connection, buffer, error);
        if (error == boost::beast::error::timeout)
            continue;
        if (error) {
            BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: MQTT WebSocket read failed code=" << error.value()
                                       << " message=" << error.message();
            throw boost::system::system_error(error, "read Orca MQTT message");
        }
        handle_packet(boost::beast::buffers_to_string(buffer.data()));
    }

    ws_close(*connection);
    if (!stopping.load())
        notify_state(false);
}

void OrcaMqttConnection::send_current_subscriptions(Connection& conn) {
    std::vector<std::string> topics;
    {
        std::lock_guard<std::mutex> lock(mutex);
        topics.assign(subscriptions.begin(), subscriptions.end());
        acknowledged_subscriptions.clear();
        pending_subscribe_packets.clear();
        for (const std::string& topic : topics)
            pending_subscriptions.erase(topic);
    }
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: sending current MQTT subscriptions count=" << topics.size();
    for (const std::string& topic : topics) {
        const uint16_t packet_id = next_packet_id++;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_subscribe_packets[packet_id] = topic;
        }
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: sending SUBSCRIBE topic=" << topic << " packet_id=" << packet_id;
        ws_write(conn, make_subscribe_packet(packet_id, topic, 1));
    }
}

void OrcaMqttConnection::send_pending_subscriptions(Connection& conn) {
    std::vector<std::string> subscribe_topics;
    std::vector<std::string> unsubscribe_topics;
    {
        std::lock_guard<std::mutex> lock(mutex);
        subscribe_topics.assign(pending_subscriptions.begin(), pending_subscriptions.end());
        unsubscribe_topics.assign(pending_unsubscriptions.begin(), pending_unsubscriptions.end());
        pending_subscriptions.clear();
        pending_unsubscriptions.clear();
    }
    for (const std::string& topic : subscribe_topics) {
        const uint16_t packet_id = next_packet_id++;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_subscribe_packets[packet_id] = topic;
        }
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: sending pending SUBSCRIBE topic=" << topic
                                << " packet_id=" << packet_id;
        ws_write(conn, make_subscribe_packet(packet_id, topic, 1));
    }
    for (const std::string& topic : unsubscribe_topics) {
        const uint16_t packet_id = next_packet_id++;
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: sending pending UNSUBSCRIBE topic=" << topic
                                << " packet_id=" << packet_id;
        ws_write(conn, make_unsubscribe_packet(packet_id, topic));
    }
}

void OrcaMqttConnection::handle_packet(const std::string& packet) {
    if (packet.size() < 2) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: received undersized MQTT packet bytes=" << packet.size();
        return;
    }
    const uint8_t header = static_cast<uint8_t>(packet[0]);
    const uint8_t packet_type = header >> 4;
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: received MQTT packet type=" << static_cast<unsigned int>(packet_type)
                            << " header=0x" << std::hex << static_cast<unsigned int>(header) << std::dec
                            << " bytes=" << packet.size();
    if (packet_type != 3) { // Only QoS 0 PUBLISH carries printer status.
        if (packet_type == 9 && packet.size() >= 5) {
            const uint16_t packet_id = (static_cast<unsigned int>(static_cast<uint8_t>(packet[2])) << 8) |
                                       static_cast<unsigned int>(static_cast<uint8_t>(packet[3]));
            std::ostringstream result_codes;
            for (size_t index = 4; index < packet.size(); ++index) {
                if (index != 4)
                    result_codes << ',';
                result_codes << "0x" << std::hex << static_cast<unsigned int>(static_cast<uint8_t>(packet[index]));
            }
            BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: received SUBACK packet_id="
                                    << packet_id
                                    << " result_codes=" << result_codes.str();

            // Each production SUBSCRIBE packet currently contains one topic.
            // MQTT grants QoS 0 or 1 for a requested QoS 1 subscription; 0x80
            // means the subscription was rejected.
            const uint8_t result = static_cast<uint8_t>(packet[4]);
            std::string topic;
            std::deque<std::pair<std::string, std::string>> requests;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto pending = pending_subscribe_packets.find(packet_id);
                if (pending != pending_subscribe_packets.end()) {
                    topic = pending->second;
                    pending_subscribe_packets.erase(pending);
                    for (auto it = pending_requests.begin(); it != pending_requests.end();) {
                        if (report_topic(it->first) == topic) {
                            requests.push_back(std::move(*it));
                            it = pending_requests.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    if (result == 0 || result == 1) {
                        acknowledged_subscriptions.insert(topic);
                    }
                }
            }
            if (topic.empty()) {
                BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: SUBACK has no pending topic packet_id=" << packet_id;
            } else if (result == 0 || result == 1) {
                BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: report subscription active topic=" << topic
                                        << " granted_qos=" << static_cast<unsigned int>(result)
                                        << " releasing_requests=" << requests.size();
                for (const auto& request : requests) {
                    if (!send_request(request.first, request.second)) {
                        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: queued MQTT request could not be sent"
                                                   << " after SUBACK dev_id=" << request.first;
                    }
                }
            } else {
                BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: report subscription rejected topic=" << topic
                                           << " result_code=0x" << std::hex << static_cast<unsigned int>(result) << std::dec
                                           << " dropped_requests=" << requests.size();
            }
        }
        return;
    }
    size_t index = 1;
    size_t multiplier = 1;
    size_t remaining = 0;
    uint8_t encoded = 0;
    do {
        if (index >= packet.size() || multiplier > 128 * 128 * 128) {
            BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: malformed MQTT PUBLISH remaining length";
            return;
        }
        encoded = static_cast<uint8_t>(packet[index++]);
        remaining += (encoded & 0x7f) * multiplier;
        multiplier *= 128;
    } while ((encoded & 0x80) != 0);
    const size_t remaining_end = index + remaining;
    if (remaining_end > packet.size() || remaining < 2 || index + 2 > remaining_end) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: malformed MQTT PUBLISH body remaining=" << remaining
                                   << " packet_bytes=" << packet.size();
        return;
    }
    const uint16_t topic_length = (static_cast<uint8_t>(packet[index]) << 8) |
                                  static_cast<uint8_t>(packet[index + 1]);
    index += 2;
    if (topic_length > packet.size() - index) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: malformed MQTT PUBLISH topic length=" << topic_length;
        return;
    }
    const std::string topic(packet.data() + index, topic_length);
    index += topic_length;
    if (((header >> 1) & 0x03) != 0) {
        if (index + 2 > remaining_end) {
            BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: malformed MQTT PUBLISH packet identifier";
            return;
        }
        index += 2; // QoS 1/2 packet identifier; the service currently sends QoS 0.
    }
    const size_t payload_size = remaining_end - index;
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: received PUBLISH topic=" << topic
                            << " payload_bytes=" << payload_size
                            << " message_callback=" << (on_message ? "set" : "null");
    // topic is "device/<id>/report" (or "/request"); hand the id up, drop anything else.
    std::string dev_id;
    if (topic.rfind("device/", 0) == 0) {
        const size_t id_start = 7;
        const size_t id_end   = topic.rfind('/');
        if (id_end != std::string::npos && id_end > id_start)
            dev_id = topic.substr(id_start, id_end - id_start);
    }
    if (dev_id.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: dropping PUBLISH on unrecognized topic=" << topic;
    } else if (on_message) {
        on_message(dev_id, packet.substr(index, remaining_end - index));
    } else {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: dropping PUBLISH because message callback is not set";
    }
}

void OrcaMqttConnection::notify_state(bool is_now_connected) {
    StateHandler callback;
    bool initial = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        connected = is_now_connected;
        initial   = !initial_completed;
        if (initial) {
            initial_result    = is_now_connected;
            initial_completed = true;
        }
        callback = on_state;
    }
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT state changed connected=" << is_now_connected
                            << " initial=" << initial << " state_callback=" << (callback ? "set" : "null");
    if (initial)
        initial_cv.notify_all();
    else if (callback)
        callback(is_now_connected, false);
}

void OrcaMqttConnection::run() {
    while (!stopping.load()) {
        const int retry_seconds = reconnect_delay_seconds.load();
        const uint64_t attempt = ++m_attempt_number;
        m_connection_stage = "starting attempt";
        try {
            BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT connection attempt=" << attempt
                                    << " retry_delay=" << retry_seconds
                                    << " url=" << current_config.url;
            connect_and_read();
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: MQTT connection attempt=" << attempt
                                       << " failed stage=" << m_connection_stage
                                       << " error=" << error.what()
                                       << " last_connack_rc=" << m_last_connack_rc.load()
                                       << " stopping=" << stopping.load();
            if (!stopping.load())
                notify_state(false);
        }
        if (stopping.load())
            break;
        // Grow the backoff only across attempts that never reached CONNACK; a
        // successful connection resets reconnect_delay_seconds to 1 (connect_and_read).
        reconnect_delay_seconds.store(std::min(retry_seconds * 2, 30));
        std::unique_lock<std::mutex> lock(mutex);
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: MQTT waiting before reconnect seconds=" << retry_seconds;
        state_cv.wait_for(lock, std::chrono::seconds(retry_seconds), [this] { return stopping.load(); });
    }
}

} // namespace Slic3r
