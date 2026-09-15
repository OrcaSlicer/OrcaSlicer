#ifndef slic3r_OrcaMqttConnection_hpp_
#define slic3r_OrcaMqttConnection_hpp_

#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <cstddef>
#include <cstdint>

namespace Slic3r {

// Minimal MQTT 3.1.1 codec + WebSocket transport (ws:// and wss://), shared by the
// LAN (OrcaSonar) and cloud (fleet) printer connections. Both PUBLISH
// commands to device/<id>/request and SUBSCRIBE device/<id>/report; Config is the
// only per-transport difference.
class OrcaMqttConnection
{
public:
    using TokenProvider  = std::function<std::string()>;
    using MessageHandler = std::function<void(const std::string&, const std::string&)>;
    using StateHandler   = std::function<void(bool connected, bool initial)>;

    struct Endpoint { std::string host; std::string port; std::string target; };

    struct Config {
        std::string   url;
        bool          use_tls = false;
        TokenProvider bearer_provider;       // set => bearer on WS upgrade, CONNECT creds omitted
        std::string   username;
        std::string   password;
        std::string   client_id  = "OrcaSlicer";
        int           keepalive_seconds = 60;
    };

    static bool parse_endpoint(const std::string& url, Endpoint& endpoint);

    // Build an MQTT 3.1.1 CONNECT packet. Clean-session is always set; the
    // username/password connect flags and payload fields are added only when
    // username is non-empty (the cloud form authenticates via a bearer on the
    // WebSocket upgrade and omits CONNECT credentials). Public for unit tests.
    static std::vector<uint8_t> make_connect_packet(const std::string& client_id,
                                                    const std::string& username,
                                                    const std::string& password,
                                                    int keepalive_seconds);

    // Topic-string helpers for the per-device request/report channels and the
    // MQTT 3.1.1 PUBLISH / SUBSCRIBE / UNSUBSCRIBE packet builders. All public
    // for unit tests. make_publish_packet emits QoS 0 (no packet identifier).
    static std::string request_topic(const std::string& dev_id);            // "device/<id>/request"
    static std::string report_topic(const std::string& dev_id);             // "device/<id>/report"
    static std::vector<uint8_t> make_publish_packet(const std::string& topic, const std::string& payload);
    static std::vector<uint8_t> make_subscribe_packet(uint16_t packet_id, const std::string& topic, uint8_t qos);
    static std::vector<uint8_t> make_unsubscribe_packet(uint16_t packet_id, const std::string& topic);

    ~OrcaMqttConnection();

    bool start(const Config& config, MessageHandler on_message, StateHandler on_state);
    void stop();
    // True while the worker thread is alive (connected OR retrying). Lets callers
    // avoid restarting a healthy connection.
    bool is_running() const;
    // True once CONNACK has been received and the socket has not since dropped.
    bool is_connected() const { return connected.load(); }
    bool subscribe(const std::string& dev_id);
    bool unsubscribe(const std::string& dev_id);
    // Last MQTT CONNACK return code: 0 ok, 1..5 refusal, -1 none seen this attempt.
    int  last_connack_rc() const { return m_last_connack_rc.load(); }
    void clear_subscriptions();
    bool send_request(const std::string& dev_id, const std::string& payload);

private:
    // The endpoint may be either a TLS (wss://) or a plaintext (ws://) WebSocket;
    // Connection holds whichever one is engaged and the ws_* helpers below
    // dispatch on it.
    using TlsWebSocket   = boost::beast::websocket::stream<
        boost::asio::ssl::stream<boost::beast::tcp_stream>>;
    using PlainWebSocket = boost::beast::websocket::stream<boost::beast::tcp_stream>;
    struct Connection;

    static void append_string(std::vector<uint8_t>& packet, const std::string& value);
    static void prepend_remaining_length(std::vector<uint8_t>& packet, size_t length);
    static std::vector<uint8_t> make_ping_packet();

    // Transport dispatch: each forwards to conn.wss (TLS) or conn.ws (plaintext).
    void        ws_write(Connection& conn, const std::vector<uint8_t>& packet); // locks write_mutex
    std::size_t ws_read(Connection& conn, boost::beast::flat_buffer& buffer, boost::system::error_code& ec);
    void        ws_handshake(Connection& conn, const Config& config, const Endpoint& endpoint);
    void        ws_close(Connection& conn);
    // Emit a queued SUBSCRIBE/UNSUBSCRIBE on the live socket right now (from the
    // caller thread), so a selection change is applied without waiting for the
    // blocking read loop to next return. No-op if no CONNACKed socket exists yet
    // (the worker sends the set on connect). The WebSocket is never dropped for a
    // subscription change.
    void flush_subscription_change();
    void connect_and_read();
    void send_current_subscriptions(Connection& conn);
    void send_pending_subscriptions(Connection& conn);
    void handle_packet(const std::string& packet);
    void notify_state(bool is_now_connected);
    void run();

    std::atomic_bool stopping{true};
    std::atomic_int reconnect_delay_seconds{1};
    // Serialises the whole of start() and stop() against each other, so the UI
    // thread's stop() (disconnect / dtor) cannot race the connect thread's start()
    // into a concurrent worker.join(). Recursive because start() calls stop().
    std::recursive_mutex lifecycle_mutex;
    std::thread worker;
    std::mutex mutex;
    std::mutex connection_mutex;
    std::mutex write_mutex; // serialises every websocket write (worker + caller threads)
    std::shared_ptr<Connection> active_connection;
    std::condition_variable initial_cv;
    std::condition_variable state_cv;
    Config current_config;
    MessageHandler on_message;
    StateHandler on_state;
    // Full report-topic strings ("device/<id>/report"), not bare device ids.
    std::set<std::string> subscriptions;
    std::set<std::string> pending_subscriptions;
    std::set<std::string> pending_unsubscriptions;
    // Requests for a subscribed device wait until the corresponding SUBACK is
    // received. Otherwise an immediate pushall response can be published by
    // the broker before this client is actually subscribed to the report topic.
    std::set<std::string> acknowledged_subscriptions;
    std::map<uint16_t, std::string> pending_subscribe_packets;
    std::deque<std::pair<std::string, std::string>> pending_requests;
    std::atomic<uint16_t> next_packet_id{1};
    std::atomic<int> m_last_connack_rc{-1};
    uint64_t m_attempt_number{0};       // worker-thread diagnostic sequence
    std::string m_connection_stage;     // worker-thread diagnostic stage
    bool initial_result{false};
    bool initial_completed{false};
    std::atomic_bool connected{false};
};

} // namespace Slic3r

#endif // slic3r_OrcaMqttConnection_hpp_
