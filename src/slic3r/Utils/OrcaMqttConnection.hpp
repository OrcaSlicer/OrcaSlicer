#ifndef slic3r_OrcaMqttConnection_hpp_
#define slic3r_OrcaMqttConnection_hpp_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace Slic3r {

struct OrcaMqttConnectionTestAccess;

// MQTT 3.1.1 client over a WebSocket (ws:// and wss://), shared by the LAN (OrcaSonar)
// and cloud (fleet) printer connections. Both PUBLISH commands to device/<id>/request
// and SUBSCRIBE device/<id>/report; Config is the only per-transport difference.
//
// The MQTT protocol (packets, packet identifiers, keepalive) is async_mqtt's
// I/O-independent connection; the WebSocket transport is Boost.Beast. Each start()
// runs one worker thread with its own io_context per connection attempt.
//
// The handlers run on the worker thread and may call any member function, including
// stop(); the join then happens in the next stop(), start() or the destructor. The
// object must not be destroyed from one of its own handlers.
class OrcaMqttConnection
{
public:
    using TokenProvider  = std::function<std::string()>;
    using MessageHandler = std::function<void(const std::string&, const std::string&)>;
    using StateHandler   = std::function<void(bool connected, bool initial)>;

    struct Config {
        std::string   url;
        bool          use_tls = false;
        std::string   ca_file;
        TokenProvider bearer_provider;       // set => bearer on WS upgrade, CONNECT creds omitted
        std::string   username;
        std::string   password;
        std::string   client_id  = "OrcaSlicer";
        int           keepalive_seconds = 60;
    };

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
    // Last MQTT CONNACK return code: 0 ok, 1..5 refusal, -1 none seen since start().
    int  last_connack_rc() const { return m_last_connack_rc.load(); }
    void clear_subscriptions();
    bool send_request(const std::string& dev_id, const std::string& payload);

private:
    friend struct OrcaMqttConnectionTestAccess;

    struct Session;
    struct Endpoint { std::string host; std::string port; std::string target; };
    // host[:port] of a ws:// or wss:// url; an IPv6 literal is bracketed, or bare without a port.
    static bool parse_endpoint(const std::string& url, Endpoint& endpoint);

    // A report topic's subscription: sent when Wanted (at the start of a session and on
    // subscribe()), then Sent until its SUBACK, which makes it Granted or Rejected.
    enum class SubscriptionState { Wanted, Sent, Granted, Rejected };
    struct Subscription {
        SubscriptionState state     = SubscriptionState::Wanted;
        std::uint16_t     packet_id = 0; // of the SUBSCRIBE, while Sent
    };
    // A request is held until the SUBACK of its device's subscription, and published in the order made.
    struct Request {
        std::string dev_id;
        std::string payload;
        bool        held = false;
    };

    // Run `work` on the worker thread, in the live session if there is one.
    void post_to_session(std::function<void(Session&)> work);
    void signal_stop();
    void stop_and_join();
    void flush_queued();
    bool run_session(const std::shared_ptr<Session>& session);
    void run_attempts();
    void notify_state(bool is_now_connected);
    void run();
    // Sessions alive in this process; the tests use it to check that none outlives its attempt.
    static int live_sessions();

    // False from start() until stop() or a refusal that retrying cannot cure.
    std::atomic_bool stopping{true};
    // Serialises start() and stop() against each other.
    std::mutex lifecycle_mutex;
    std::thread worker;
    // Guards the members below, except the atomics.
    std::mutex mutex;
    std::shared_ptr<Session> active_session;
    std::condition_variable state_cv;
    Config current_config;
    MessageHandler on_message;
    StateHandler on_state;
    // By full report-topic string ("device/<id>/report"), not bare device id.
    std::map<std::string, Subscription> subscriptions;
    std::deque<Request> requests;
    std::atomic<int> m_last_connack_rc{-1};
    bool initial_result{false};
    bool initial_completed{false};
    bool flushed{false}; // the session has sent what stop() found queued
    std::atomic_bool connected{false};
    // Test hook: maps the retry delay in seconds to the time actually waited.
    std::function<std::chrono::milliseconds(int)> retry_wait;
};

} // namespace Slic3r

#endif // slic3r_OrcaMqttConnection_hpp_
