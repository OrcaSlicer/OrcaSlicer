#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <slic3r/Utils/OrcaMqttConnection.hpp>

#include "orca_mqtt_mock_broker.hpp"
#include "test_utils.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <ios>
#include <memory>
#include <mutex>
#include <ratio>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r {
// Reaches the connection's test hooks.
struct OrcaMqttConnectionTestAccess {
    static void set_retry_wait(OrcaMqttConnection& connection, std::function<std::chrono::milliseconds(int)> wait)
    {
        connection.retry_wait = std::move(wait);
    }
    static int live_sessions() { return OrcaMqttConnection::live_sessions(); }
    static bool parse_endpoint(const std::string& url, std::string& host, std::string& port, std::string& target)
    {
        OrcaMqttConnection::Endpoint endpoint;
        const bool                   ok = OrcaMqttConnection::parse_endpoint(url, endpoint);
        host   = endpoint.host;
        port   = endpoint.port;
        target = endpoint.target;
        return ok;
    }
    // The connection's state lock, for holding the worker where it needs it.
    static std::unique_lock<std::mutex> hold_state_lock(OrcaMqttConnection& connection) { return std::unique_lock<std::mutex>(connection.mutex); }
    static void forget_connack_rc(OrcaMqttConnection& connection) { connection.m_last_connack_rc.store(-1); }
    // What signal_stop() does under that lock, without waiting for it.
    static void mark_stopping(OrcaMqttConnection& connection)
    {
        connection.stopping.store(true);
        connection.connected = false;
    }
};
} // namespace Slic3r

using Slic3r::OrcaMqttConnection;
using Slic3r::OrcaMqttConnectionTestAccess;
using orca_mqtt_test::MockBroker;
using orca_mqtt_test::Stall;

namespace {

// Guard for waits; a wait only ever ends early, so this bounds how long a failing test hangs,
// not how long anything may take.
constexpr std::chrono::seconds kWait{30};

// Records what the client's handlers are told. Declare it before the connection so it
// outlives the worker thread that calls into it.
class Inbox
{
public:
    OrcaMqttConnection::MessageHandler message_handler()
    {
        return [this](const std::string& dev_id, const std::string& payload) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_messages.emplace_back(dev_id, payload);
            }
            m_cv.notify_all();
        };
    }

    OrcaMqttConnection::StateHandler state_handler()
    {
        return [this](bool connected, bool initial) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_states.emplace_back(connected, initial);
            }
            m_cv.notify_all();
        };
    }

    bool wait_messages(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, kWait, [&] { return m_messages.size() >= count; });
    }

    bool wait_states(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, kWait, [&] { return m_states.size() >= count; });
    }

    std::vector<std::pair<std::string, std::string>> messages() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_messages;
    }

    std::vector<std::pair<bool, bool>> states() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_states;
    }

private:
    mutable std::mutex                               m_mutex;
    std::condition_variable                          m_cv;
    std::vector<std::pair<std::string, std::string>> m_messages;
    std::vector<std::pair<bool, bool>>               m_states;
};

// Replaces the retry wait: records every delay the connection asks for and waits `actual`
// instead, so a test sees the 1, 2, 4 ... 30 second schedule without living through it.
class RetryLog
{
public:
    explicit RetryLog(std::chrono::milliseconds actual = std::chrono::milliseconds(1)) : m_actual(actual) {}

    std::function<std::chrono::milliseconds(int)> hook()
    {
        return [this](int seconds) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_delays.push_back(seconds);
            }
            m_cv.notify_all();
            return m_actual;
        };
    }

    bool wait_count(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, kWait, [&] { return m_delays.size() >= count; });
    }

    std::vector<int> delays() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_delays;
    }

private:
    const std::chrono::milliseconds m_actual;
    mutable std::mutex              m_mutex;
    std::condition_variable         m_cv;
    std::vector<int>                m_delays;
};

OrcaMqttConnection::Config lan_config(const MockBroker& broker)
{
    OrcaMqttConnection::Config config;
    config.url      = broker.url();
    config.username = "orcasonar";
    config.password = "code";
    return config;
}

// Writes the broker's certificate where start() can load it as the CA file.
struct CaFile {
    explicit CaFile(const std::string& pem)
    {
        std::ofstream file(file_guard.string(), std::ios::binary);
        file << pem;
    }
    ScopedTemporaryFile file_guard{".pem"};
    std::string                 path() const { return file_guard.string(); }
};

MockBroker::Options tls_options()
{
    MockBroker::Options options;
    options.tls = true;
    return options;
}

// Waits until a request that went out after `marker` reached the broker, which also means
// the client has processed everything the broker sent before the marker.
bool sync_through_marker(MockBroker& broker, Inbox& inbox, std::size_t messages_before)
{
    broker.push_publish("device/marker/report", "marker");
    return inbox.wait_messages(messages_before + 1);
}

const std::string kReportTopic = "device/dev-1/report";

} // namespace

TEST_CASE("start connects to a url without a path and refuses one that is not a websocket endpoint", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    OrcaMqttConnection::Config config = lan_config(broker);

    SECTION("no path")
    {
        config.url = config.url.substr(0, config.url.size() - std::string("/mqtt").size());
        REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
        CHECK(broker.connect_count() == 1);
    }
    SECTION("not a websocket scheme")
    {
        config.url = GENERATE(as<std::string>{}, "http://127.0.0.1:1/mqtt", "printer.local:8280/mqtt", "");
        CHECK_FALSE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    }
    SECTION("no host")
    {
        config.url = "ws:///mqtt";
        CHECK_FALSE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    }
    CHECK(broker.accept_count() <= 1);
}

TEST_CASE("send_request refuses while not connected", "[OrcaMqtt]")
{
    OrcaMqttConnection connection;
    CHECK_FALSE(connection.send_request("abc", R"({"pushing":{"command":"pushall"}})"));
    CHECK_FALSE(connection.is_connected());
    CHECK_FALSE(connection.is_running());
    CHECK(connection.last_connack_rc() == -1);
}

TEST_CASE("start connects with username and password, a clean session and no Origin header", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    OrcaMqttConnection::Config config = lan_config(broker);
    config.client_id         = "orcaslicer-lan-x";
    config.keepalive_seconds = 42;

    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    CHECK(connection.is_connected());
    CHECK(connection.is_running());
    CHECK(connection.last_connack_rc() == 0);

    const orca_mqtt_test::ConnectRecord connect = broker.last_connect();
    CHECK(connect.protocol_level == 4);
    CHECK(connect.clean_session);
    CHECK(connect.client_id == "orcaslicer-lan-x");
    CHECK(connect.keepalive == 42);
    CHECK(connect.has_username);
    CHECK(connect.username == "orcasonar");
    CHECK(connect.has_password);
    CHECK(connect.password == "code");

    const orca_mqtt_test::UpgradeRecord upgrade = broker.last_upgrade();
    CHECK_FALSE(upgrade.has("authorization"));
    CHECK_FALSE(upgrade.has("origin"));
    CHECK(upgrade.header("sec-websocket-protocol") == "mqtt");
    CHECK(upgrade.header("user-agent") == "OrcaSlicer");
    CHECK(inbox.states().empty()); // the first outcome is start()'s return value
}

TEST_CASE("start sends a username without a password and ignores a password without a username", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    OrcaMqttConnection::Config config;
    config.url = broker.url();

    SECTION("username only")
    {
        config.username = "orcasonar";
        REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
        CHECK(broker.last_connect().has_username);
        CHECK_FALSE(broker.last_connect().has_password);
    }
    SECTION("password only")
    {
        config.password = "code";
        REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
        CHECK_FALSE(broker.last_connect().has_username);
        CHECK_FALSE(broker.last_connect().has_password);
    }
}

TEST_CASE("start with an empty client id connects as OrcaSlicer", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    OrcaMqttConnection::Config config = lan_config(broker);
    config.client_id = "";

    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    CHECK(broker.last_connect().client_id == "OrcaSlicer");
}

TEST_CASE("start sends a bearer on the upgrade and omits the CONNECT credentials", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    OrcaMqttConnection::Config config = lan_config(broker); // credentials set, but the bearer takes precedence
    config.bearer_provider = [] { return std::string("tok-1"); };

    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    const orca_mqtt_test::UpgradeRecord upgrade = broker.last_upgrade();
    CHECK(upgrade.header("authorization") == "Bearer tok-1");
    CHECK_FALSE(upgrade.has("origin"));
    CHECK(upgrade.header("sec-websocket-protocol") == "mqtt");
    CHECK_FALSE(broker.last_connect().has_username);
    CHECK_FALSE(broker.last_connect().has_password);
    CHECK(broker.last_connect().clean_session);
}

TEST_CASE("each reconnect asks the bearer provider for a fresh token", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    std::atomic<int> calls{0};
    OrcaMqttConnection::Config config;
    config.url             = broker.url();
    config.bearer_provider = [&calls] { return "tok-" + std::to_string(++calls); };

    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    broker.drop_client();
    REQUIRE(broker.wait_upgrades(2));
    CHECK(broker.upgrades()[0].header("authorization") == "Bearer tok-1");
    CHECK(broker.upgrades()[1].header("authorization") == "Bearer tok-2");
}

TEST_CASE("a bearer provider returning no token sends no Authorization header", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    OrcaMqttConnection::Config config;
    config.url             = broker.url();
    config.bearer_provider = [] { return std::string(); };

    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    CHECK_FALSE(broker.last_upgrade().has("authorization"));
    CHECK_FALSE(broker.last_connect().has_username);
}

TEST_CASE("an upgrade that does not negotiate the mqtt subprotocol is rejected", "[OrcaMqtt]")
{
    MockBroker::Options options;
    options.subprotocol = GENERATE(as<std::string>{}, "", "mqttv3.1");
    MockBroker         broker(options);
    Inbox              inbox;
    OrcaMqttConnection connection;

    CHECK_FALSE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    CHECK(broker.upgrades().size() >= 1);
    CHECK(broker.connect_count() == 0); // no MQTT over a connection that did not agree to it
    CHECK_FALSE(connection.is_connected());
    CHECK(connection.last_connack_rc() == -1);
}

TEST_CASE("a CONNECT refused for bad credentials is final and reported by last_connack_rc", "[OrcaMqtt]")
{
    const int rc = GENERATE(4, 5);
    MockBroker::Options options;
    options.connack_rc = rc;
    MockBroker         broker(options);
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());

    // A second connection that is refused for a reason that may clear up, as a clock: by the time
    // it has retried many times, a retry of the first would have shown.
    MockBroker::Options retry_options;
    retry_options.connack_rc = 3;
    MockBroker         retry_broker(retry_options);
    Inbox              retry_inbox;
    RetryLog           retry_retries;
    OrcaMqttConnection retrying;
    OrcaMqttConnectionTestAccess::set_retry_wait(retrying, retry_retries.hook());

    CHECK_FALSE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    CHECK_FALSE(retrying.start(lan_config(retry_broker), retry_inbox.message_handler(), retry_inbox.state_handler()));
    REQUIRE(retry_broker.wait_connects(25));
    CHECK(connection.last_connack_rc() == rc);
    CHECK_FALSE(connection.is_running());
    CHECK_FALSE(connection.is_connected());
    CHECK(broker.connect_count() == 1);
    CHECK(retries.delays().empty());
    CHECK(retrying.is_running());
}

TEST_CASE("a CONNECT refused for a reason that may clear up is retried with a growing delay", "[OrcaMqtt]")
{
    const int rc = GENERATE(1, 2, 3);
    MockBroker::Options options;
    options.connack_rc = rc;
    MockBroker         broker(options);
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());

    CHECK_FALSE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    CHECK(connection.last_connack_rc() == rc);
    CHECK(connection.is_running());
    REQUIRE(retries.wait_count(3));
    CHECK(retries.delays()[0] == 1);
    CHECK(retries.delays()[1] == 2);
    CHECK(retries.delays()[2] == 4);
}

TEST_CASE("the retry delay grows to a 30 second cap", "[OrcaMqtt]")
{
    MockBroker         broker;
    broker.set_close_on_accept(true); // every attempt fails at once, on every OS
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());

    CHECK_FALSE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(retries.wait_count(9));
    connection.stop();
    const std::vector<int> delays = retries.delays();
    const std::vector<int> expected{1, 2, 4, 8, 16, 30, 30, 30, 30};
    CHECK(std::vector<int>(delays.begin(), delays.begin() + 9) == expected);
}

TEST_CASE("the retry delay starts over after a session that reached CONNACK", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    broker.script_connack_rcs({3, 3, 3}); // three refusals, then the CONNECT is accepted

    REQUIRE_FALSE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    // The refusals of the 2nd and 3rd attempts and the session of the 4th reach the state handler.
    REQUIRE(inbox.wait_states(3));
    CHECK(inbox.states().back() == std::make_pair(true, false));
    // Drop that session, and refuse what follows.
    broker.set_connack_rc(3);
    broker.drop_client();
    REQUIRE(retries.wait_count(6));
    const std::vector<int> delays = retries.delays();
    // 1, 2, 4 across the refusals; the session that was up resets it to 1; the refusal after it doubles it.
    CHECK(delays[0] == 1);
    CHECK(delays[1] == 2);
    CHECK(delays[2] == 4);
    CHECK(delays[3] == 1);
    CHECK(delays[4] == 2);
    CHECK(delays[5] == 4);
}

TEST_CASE("a refusal for bad credentials on a later attempt is reported to the state handler", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());

    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    broker.set_connack_rc(5);
    broker.drop_client();
    REQUIRE(inbox.wait_states(2));
    const std::vector<std::pair<bool, bool>> expected{{false, false}, {false, false}}; // the drop, then the final refusal
    CHECK(inbox.states() == expected);
    CHECK(connection.last_connack_rc() == 5);
    CHECK_FALSE(connection.is_running());
    CHECK_FALSE(connection.is_connected());
    connection.stop();
    CHECK(broker.connect_count() == 2);
}

TEST_CASE("last_connack_rc is kept across transport failures and cleared by a restart", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());

    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    CHECK(connection.last_connack_rc() == 0);
    broker.set_connack_rc(3);
    broker.drop_client();
    REQUIRE(broker.wait_connects(2));
    REQUIRE(inbox.wait_states(2));
    CHECK(connection.last_connack_rc() == 3);

    // From here every attempt fails before any MQTT.
    const int accepts = broker.accept_count();
    broker.set_close_on_accept(true);
    REQUIRE(broker.wait_accepts(accepts + 3));
    CHECK(connection.last_connack_rc() == 3);

    CHECK_FALSE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    CHECK(connection.last_connack_rc() == -1);
}

TEST_CASE("the first outcome is the return of start and later ones go to the state handler", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());

    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    CHECK(inbox.states().empty());

    broker.drop_client();
    REQUIRE(inbox.wait_states(2));
    const std::vector<std::pair<bool, bool>> expected{{false, false}, {true, false}}; // dropped, then restored
    CHECK(inbox.states() == expected);
    CHECK(connection.is_connected());
}

TEST_CASE("start fails when the connection is closed at once and the worker keeps retrying until stopped", "[OrcaMqtt]")
{
    MockBroker         broker;
    broker.set_close_on_accept(true);
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());

    CHECK_FALSE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    CHECK_FALSE(connection.is_connected());
    CHECK(connection.is_running());
    CHECK(connection.last_connack_rc() == -1);
    REQUIRE(retries.wait_count(3)); // it is still trying
    connection.stop();
    CHECK_FALSE(connection.is_running());
}

TEST_CASE("start gives up waiting after ten seconds while the CONNACK does not come", "[OrcaMqtt]")
{
    MockBroker::Options options;
    options.stall = Stall::Connack;
    MockBroker         broker(options);
    Inbox              inbox;
    OrcaMqttConnection connection;

    const auto started_at = std::chrono::steady_clock::now();
    CHECK_FALSE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    CHECK(std::chrono::steady_clock::now() - started_at >= std::chrono::seconds(10));
    CHECK(broker.connect_count() == 1);
    CHECK(connection.is_running()); // the attempt goes on
    CHECK_FALSE(connection.is_connected());
}

TEST_CASE("subscribe sends a QoS 1 SUBSCRIBE for the report topic", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribed(kReportTopic));
    CHECK(broker.subscribed_qos(kReportTopic) == 1);
}

TEST_CASE("a subscription made before start is sent once the session is up", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.subscribe("dev-1"));

    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(broker.wait_subscribed(kReportTopic));
}

TEST_CASE("subscribe rejects a missing id and a topic over the service limit", "[OrcaMqtt]")
{
    OrcaMqttConnection connection;
    CHECK_FALSE(connection.subscribe(""));
    // "device/" + id + "/report" may not exceed 96 bytes.
    CHECK(connection.subscribe(std::string(96 - std::string("device//report").size(), 'a')));
    CHECK_FALSE(connection.subscribe(std::string(96 - std::string("device//report").size() + 1, 'a')));
}

TEST_CASE("subscribing twice sends one SUBSCRIBE", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(connection.subscribe("dev-2"));
    REQUIRE(broker.wait_subscribed("device/dev-2/report"));
    CHECK(broker.subscribe_count() == 2);
}

TEST_CASE("unsubscribe removes the subscription at the broker", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribed(kReportTopic));

    REQUIRE(connection.unsubscribe("dev-1"));
    REQUIRE(broker.wait_unsubscribed(kReportTopic));
}

TEST_CASE("a dropped connection is re-established and its subscriptions are sent again", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(connection.subscribe("dev-2"));
    REQUIRE(broker.wait_subscribed("device/dev-2/report"));
    REQUIRE(broker.wait_subscribed(kReportTopic));

    broker.drop_client();
    REQUIRE(broker.wait_connects(2));
    REQUIRE(broker.wait_subscribed(kReportTopic));
    REQUIRE(broker.wait_subscribed("device/dev-2/report"));
    CHECK(broker.last_connect().clean_session);
    CHECK(retries.delays().front() == 1);
}

TEST_CASE("unsubscribe while connected sends an UNSUBSCRIBE and while disconnected sends nothing", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;

    // Disconnected: the subscription made before start is withdrawn, so it is never sent.
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(connection.unsubscribe("dev-1"));
    REQUIRE(connection.subscribe("dev-2"));
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(broker.wait_subscribed("device/dev-2/report"));
    CHECK_FALSE(broker.is_subscribed(kReportTopic));
    CHECK(broker.subscribe_count() == 1);
    CHECK(broker.unsubscribe_count() == 0);

    // Connected: the broker is told.
    REQUIRE(connection.unsubscribe("dev-2"));
    REQUIRE(broker.wait_unsubscribed("device/dev-2/report"));
    CHECK(broker.unsubscribe_count() == 1);
}

TEST_CASE("an unsubscribed device is not subscribed again after a reconnect", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(connection.subscribe("dev-2"));
    REQUIRE(broker.wait_subscribed("device/dev-2/report"));
    REQUIRE(connection.unsubscribe("dev-1"));
    REQUIRE(broker.wait_unsubscribed(kReportTopic));

    broker.drop_client();
    REQUIRE(broker.wait_connects(2));
    REQUIRE(broker.wait_subscribed("device/dev-2/report"));
    CHECK_FALSE(broker.is_subscribed(kReportTopic));
}

TEST_CASE("clear_subscriptions forgets every device so a reconnect subscribes to none", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribed(kReportTopic));

    connection.clear_subscriptions();
    broker.drop_client();
    REQUIRE(broker.wait_connects(2));
    REQUIRE(inbox.wait_states(2));
    CHECK_FALSE(broker.is_subscribed(kReportTopic));
    CHECK(broker.subscribe_count() == 1);
}

TEST_CASE("send_request publishes to the request topic at QoS 0", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    REQUIRE(connection.send_request("dev-1", R"({"pushing":{"command":"pushall"}})"));
    REQUIRE(broker.wait_publishes(1));
    const orca_mqtt_test::PublishRecord publish = broker.publishes().front();
    CHECK(publish.topic == "device/dev-1/request");
    CHECK(publish.payload == R"({"pushing":{"command":"pushall"}})");
    CHECK(publish.qos == 0);
    CHECK_FALSE(connection.send_request("", "x"));
}

TEST_CASE("a report on a subscribed device reaches the message handler with its id", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribed(kReportTopic));

    broker.push_report("dev-1", R"({"print":{"command":"push_status"}})");
    REQUIRE(inbox.wait_messages(1));
    CHECK(inbox.messages()[0].first == "dev-1");
    CHECK(inbox.messages()[0].second == R"({"print":{"command":"push_status"}})");
}

TEST_CASE("publications outside device/<id>/ topics are not passed on", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    broker.push_publish("other/topic", "ignored");
    broker.push_publish("device//report", "ignored");
    broker.push_publish("device/x", "ignored");
    broker.push_publish("device/dev-9/report", "kept");
    REQUIRE(inbox.wait_messages(1));
    CHECK(inbox.messages().size() == 1);
    CHECK(inbox.messages()[0].first == "dev-9");
    CHECK(inbox.messages()[0].second == "kept");
}

TEST_CASE("a QoS 1 report is delivered and acknowledged with its packet identifier", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    broker.push_raw(orca_mqtt_test::mqtt_publish_qos1(kReportTopic, 7, "at least once"));
    REQUIRE(inbox.wait_messages(1));
    CHECK(inbox.messages()[0].second == "at least once");
    REQUIRE(broker.wait_pubacks(1));
    CHECK(broker.puback_ids() == std::vector<int>{7});
}

TEST_CASE("a broker with a small in-flight window keeps delivering QoS 1 reports", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    broker.set_inflight_limit(2);

    std::vector<std::string> payloads;
    for (int i = 0; i < 12; ++i)
        payloads.push_back("report-" + std::to_string(i));
    broker.queue_qos1(kReportTopic, payloads); // only the PUBACKs let the rest through
    REQUIRE(inbox.wait_messages(payloads.size()));
    for (std::size_t i = 0; i < payloads.size(); ++i)
        CHECK(inbox.messages()[i].second == payloads[i]);
}

TEST_CASE("a request for a subscribed device waits for the SUBACK", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    broker.set_hold_subacks(true);
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribes(1));

    REQUIRE(connection.send_request("dev-1", "held"));
    REQUIRE(connection.send_request("dev-2", "direct")); // not subscribed: goes out at once
    REQUIRE(broker.wait_publishes(1));
    CHECK(broker.received_requests() == std::vector<std::string>{"direct"});

    broker.release_subacks();
    REQUIRE(broker.wait_publishes(2));
    CHECK(broker.received_requests() == std::vector<std::string>{"direct", "held"});
}

TEST_CASE("requests queued behind a rejected subscription are dropped and never sent", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    broker.set_hold_subacks(true);
    broker.set_reject_subscriptions(true);
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribes(1));
    REQUIRE(connection.send_request("dev-1", "held"));

    broker.release_subacks();
    REQUIRE(sync_through_marker(broker, inbox, 0)); // the client has handled the rejection
    REQUIRE(connection.send_request("dev-2", "sentinel"));
    REQUIRE(broker.wait_publishes(1));
    CHECK(broker.received_requests() == std::vector<std::string>{"sentinel"});
}

TEST_CASE("requests held for a device are dropped when it is unsubscribed and never sent", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    broker.set_hold_subacks(true);
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribes(1));
    REQUIRE(connection.send_request("dev-1", "held"));

    REQUIRE(connection.unsubscribe("dev-1"));
    broker.release_subacks(); // the late SUBACK of the withdrawn subscription
    REQUIRE(sync_through_marker(broker, inbox, 0));
    REQUIRE(connection.send_request("dev-2", "sentinel"));
    REQUIRE(broker.wait_publishes(1));
    CHECK(broker.received_requests() == std::vector<std::string>{"sentinel"});
}

TEST_CASE("a late rejection of a withdrawn subscription does not reject its replacement", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    broker.set_hold_subacks(true);
    broker.set_reject_subscriptions(true);
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribes(1)); // SUBACK 1: rejected, held
    REQUIRE(connection.unsubscribe("dev-1"));
    broker.set_reject_subscriptions(false);
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribes(2)); // SUBACK 2: granted, held
    REQUIRE(connection.send_request("dev-1", "held"));

    broker.release_subacks(); // SUBACK 1 first, then SUBACK 2
    REQUIRE(broker.wait_publishes(1));
    CHECK(broker.received_requests() == std::vector<std::string>{"held"});
}

TEST_CASE("a late grant of a withdrawn subscription does not approve its replacement", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    broker.set_hold_subacks(true);
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribes(1)); // SUBACK 1: granted, held
    REQUIRE(connection.unsubscribe("dev-1"));
    broker.set_reject_subscriptions(true);
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribes(2)); // SUBACK 2: rejected, held
    REQUIRE(connection.send_request("dev-1", "held"));

    broker.release_subacks(1); // only the old grant
    REQUIRE(sync_through_marker(broker, inbox, 0));
    REQUIRE(connection.send_request("dev-2", "sentinel-1"));
    REQUIRE(broker.wait_publishes(1));
    CHECK(broker.received_requests() == std::vector<std::string>{"sentinel-1"}); // not released by the old grant

    broker.release_subacks(1); // the rejection of the current subscription
    REQUIRE(sync_through_marker(broker, inbox, 1));
    REQUIRE(connection.send_request("dev-2", "sentinel-2"));
    REQUIRE(broker.wait_publishes(2));
    CHECK(broker.received_requests() == std::vector<std::string>{"sentinel-1", "sentinel-2"}); // and dropped by it
}

TEST_CASE("requests released by a SUBACK go out in order, ahead of those sent after it", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    constexpr int      kHeld  = 40;
    constexpr int      kAfter = 40;

    // The first report after the SUBACK makes the handler send more requests for the device.
    std::atomic<int> reports{0};
    REQUIRE(connection.start(lan_config(broker),
        [&](const std::string& dev_id, const std::string& payload) {
            inbox.message_handler()(dev_id, payload);
            if (reports++ == 0)
                for (int i = 0; i < kAfter; ++i)
                    connection.send_request("dev-1", "handler-" + std::to_string(i));
        },
        inbox.state_handler()));
    broker.set_hold_subacks(true);
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribes(1));
    for (int i = 0; i < kHeld; ++i)
        REQUIRE(connection.send_request("dev-1", "held-" + std::to_string(i)));

    // Another thread keeps sending while the SUBACK is released.
    std::atomic_bool release{false};
    std::thread      sender([&] {
        while (!release.load())
            std::this_thread::yield();
        for (int i = 0; i < kAfter; ++i)
            connection.send_request("dev-1", "thread-" + std::to_string(i));
    });
    release.store(true);
    broker.release_subacks();
    broker.push_publish("device/dev-1/report", "go");
    sender.join();

    const std::size_t total = kHeld + 2 * kAfter;
    REQUIRE(broker.wait_publishes(total));
    const std::vector<std::string> sent = broker.received_requests();
    for (int i = 0; i < kHeld; ++i)
        CHECK(sent[static_cast<std::size_t>(i)] == "held-" + std::to_string(i)); // all of them first, in order
    int next_handler = 0;
    int next_thread  = 0;
    for (std::size_t i = kHeld; i < sent.size(); ++i) {
        if (sent[i].rfind("handler-", 0) == 0)
            CHECK(sent[i] == "handler-" + std::to_string(next_handler++));
        else
            CHECK(sent[i] == "thread-" + std::to_string(next_thread++));
    }
    CHECK(next_handler == kAfter);
    CHECK(next_thread == kAfter);
}

TEST_CASE("a request made while the subscription is acknowledged again after a reconnect is held until then", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribed(kReportTopic));
    REQUIRE(connection.send_request("dev-1", "first")); // acknowledged: goes out
    REQUIRE(broker.wait_publishes(1));

    broker.set_hold_subacks(true);
    broker.drop_client();
    REQUIRE(inbox.wait_states(2)); // dropped, then connected again
    REQUIRE(broker.wait_subscribes(2));
    REQUIRE(connection.send_request("dev-1", "second"));
    REQUIRE(connection.send_request("dev-2", "sentinel"));
    REQUIRE(broker.wait_publishes(2));
    CHECK(broker.received_requests() == std::vector<std::string>{"first", "sentinel"});

    broker.release_subacks();
    REQUIRE(broker.wait_publishes(3));
    CHECK(broker.received_requests().back() == "second");
}

TEST_CASE("a report split across WebSocket messages is delivered once", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    const std::string packet = orca_mqtt_test::mqtt_publish(kReportTopic, std::string(200, 'x'));
    // Where the packet is cut: in the payload, after the fixed header byte, after the first
    // remaining-length byte, or into single bytes.
    const std::size_t cut = GENERATE(std::size_t(40), std::size_t(1), std::size_t(2), std::size_t(0));
    if (cut == 0) {
        for (const char byte : packet)
            broker.push_raw(std::string(1, byte));
    } else {
        broker.push_raw(packet.substr(0, cut));
        broker.push_raw(packet.substr(cut));
    }
    REQUIRE(inbox.wait_messages(1));
    CHECK(inbox.messages().size() == 1);
    CHECK(inbox.messages()[0].first == "dev-1");
    CHECK(inbox.messages()[0].second == std::string(200, 'x'));
}

TEST_CASE("two reports in one WebSocket message are both delivered in order", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    broker.push_raw(orca_mqtt_test::mqtt_publish(kReportTopic, "first") + orca_mqtt_test::mqtt_publish(kReportTopic, "second"));
    REQUIRE(inbox.wait_messages(2));
    CHECK(inbox.messages()[0].second == "first");
    CHECK(inbox.messages()[1].second == "second");
}

TEST_CASE("packets that straddle WebSocket messages are delivered in order", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    const std::string a = orca_mqtt_test::mqtt_publish(kReportTopic, "alpha");
    const std::string b = orca_mqtt_test::mqtt_publish(kReportTopic, "bravo");
    const std::string c = orca_mqtt_test::mqtt_publish(kReportTopic, "charlie");
    const std::string all = a + b + c;
    broker.push_raw(all.substr(0, a.size() + 5));  // a, and the start of b
    broker.push_raw(all.substr(a.size() + 5, b.size() - 5 + 3)); // the rest of b, and the start of c
    broker.push_raw(all.substr(a.size() + b.size() + 3));
    REQUIRE(inbox.wait_messages(3));
    CHECK(inbox.messages()[0].second == "alpha");
    CHECK(inbox.messages()[1].second == "bravo");
    CHECK(inbox.messages()[2].second == "charlie");
}

TEST_CASE("a message split into continuation frames is delivered once", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    broker.set_fragment_size(8); // from the CONNACK on, every message arrives as 8-byte frames
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribed(kReportTopic));

    const std::string payload(100, 'y');
    broker.push_report("dev-1", payload);
    broker.push_raw(orca_mqtt_test::mqtt_publish(kReportTopic, "a") + orca_mqtt_test::mqtt_publish(kReportTopic, "b"));
    REQUIRE(inbox.wait_messages(3));
    CHECK(inbox.messages()[0].second == payload);
    CHECK(inbox.messages()[1].second == "a");
    CHECK(inbox.messages()[2].second == "b");
}

TEST_CASE("bytes that follow the CONNACK in the same message are not lost", "[OrcaMqtt]")
{
    MockBroker::Options options;
    options.connack_trailer = orca_mqtt_test::mqtt_publish(kReportTopic, "early");
    MockBroker         broker(options);
    Inbox              inbox;
    OrcaMqttConnection connection;

    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(inbox.wait_messages(1));
    CHECK(inbox.messages()[0].second == "early");
}

TEST_CASE("a large report is delivered whole", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribed(kReportTopic));

    std::string payload(300 * 1024, 'a');
    for (std::size_t i = 0; i < payload.size(); i += 97)
        payload[i] = static_cast<char>('a' + (i / 97) % 26);

    broker.set_fragment_size(GENERATE(std::size_t(0), std::size_t(1000))); // one frame, or many
    broker.push_report("dev-1", payload);
    REQUIRE(inbox.wait_messages(1));
    CHECK(inbox.messages()[0].second == payload);
}

TEST_CASE("large requests arrive intact and in order", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    constexpr int kCount = 60;
    std::vector<std::string> payloads;
    for (int i = 0; i < kCount; ++i)
        payloads.push_back(std::string(64 * 1024, static_cast<char>('A' + i % 26)) + std::to_string(i));
    for (const std::string& payload : payloads)
        REQUIRE(connection.send_request("dev-1", payload));
    REQUIRE(broker.wait_publishes(kCount));
    CHECK(broker.received_requests() == payloads);
}

TEST_CASE("stop right after queuing requests leaves nothing running", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    for (int i = 0; i < 20; ++i)
        connection.send_request("dev-1", std::string(256 * 1024, 'q'));
    connection.stop();
    CHECK_FALSE(connection.is_running());
    CHECK(OrcaMqttConnectionTestAccess::live_sessions() == 0);
}

// The pings are timed in ping intervals (half the keepalive). A whole-keepalive first ping, which
// is what a missing interval gives, arrives at 2 intervals; the right one at 1. The bound between
// them leaves 0.6 intervals for a late ping, which a loaded machine can only add to.
constexpr int kPingKeepalive = 8;
constexpr std::chrono::milliseconds kPingInterval{kPingKeepalive * 1000 / 2};

double in_intervals(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration) / kPingInterval;
}

// The first ping after `connected_at`.
std::chrono::steady_clock::time_point first_ping_after(const MockBroker& broker, std::chrono::steady_clock::time_point connected_at)
{
    for (const auto ping : broker.ping_times())
        if (ping > connected_at)
            return ping;
    return std::chrono::steady_clock::time_point::max();
}

TEST_CASE("pings start half a keepalive after the CONNECT with subscriptions made before it", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    OrcaMqttConnection::Config config = lan_config(broker);
    config.keepalive_seconds = kPingKeepalive;
    REQUIRE(connection.subscribe("dev-1")); // its SUBSCRIBE is sent as soon as the session is up
    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));

    REQUIRE(broker.wait_pings(1));
    CHECK(broker.last_connect().keepalive == kPingKeepalive);
    const auto connected_at = broker.last_connect().at;
    const double first      = in_intervals(first_ping_after(broker, connected_at) - connected_at);
    CHECK(first < 1.6);  // not after a whole keepalive
    CHECK(first >= 0.5); // and not at once
    CHECK(connection.is_connected());
}

TEST_CASE("pings start half a keepalive after a reconnect with subscriptions", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    OrcaMqttConnection::Config config = lan_config(broker);
    config.keepalive_seconds = kPingKeepalive;
    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribed(kReportTopic));

    broker.drop_client();
    REQUIRE(broker.wait_connects(2));
    const auto connected_at = broker.connects()[1].at;
    REQUIRE(broker.wait_pings(1));
    const double first = in_intervals(first_ping_after(broker, connected_at) - connected_at);
    CHECK(first < 1.6);
    CHECK(first >= 0.5);
}

TEST_CASE("the ping cadence does not depend on other traffic", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    OrcaMqttConnection::Config config = lan_config(broker);
    config.keepalive_seconds = 4; // a 2 second interval, to keep the test short
    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));

    // Requests in a steady stream must neither stop the pings nor bunch them up.
    std::atomic_bool busy{true};
    std::thread      traffic([&] {
        std::size_t sent = 0;
        while (busy.load()) {
            connection.send_request("dev-1", "busy");
            broker.wait_publishes(++sent);
        }
    });
    const bool pinged = broker.wait_pings(3);
    busy.store(false);
    traffic.join();
    REQUIRE(pinged);
    const auto connected_at = broker.last_connect().at;
    const auto pings        = broker.ping_times();
    // Three pings take about three intervals: not fewer, and not stopped by the traffic.
    CHECK((in_intervals(pings[2] - connected_at) * 2) >= 2.5);
    CHECK((in_intervals(pings[2] - connected_at) * 2) < 6.0);
    CHECK(broker.connect_count() == 1); // the broker answered every ping in time
}

TEST_CASE("stop closes the session and refuses further requests", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    connection.stop();
    CHECK_FALSE(connection.is_running());
    CHECK_FALSE(connection.is_connected());
    CHECK_FALSE(connection.send_request("dev-1", "late"));
    CHECK(broker.wait_no_client());
    CHECK(inbox.states().empty()); // a stop is not reported as a drop
    connection.stop();             // and stopping again is harmless
}

TEST_CASE("stop returns while the connection setup is stalled", "[OrcaMqtt]")
{
    MockBroker::Options options;
    const Stall stall = GENERATE(Stall::TlsHandshake, Stall::Upgrade, Stall::Connack);
    options.stall = stall;
    options.tls   = stall == Stall::TlsHandshake;
    MockBroker         broker(options);
    Inbox              inbox;
    OrcaMqttConnection connection;
    OrcaMqttConnection::Config config = lan_config(broker);
    config.use_tls = options.tls;

    std::promise<bool> started;
    std::thread        starter([&] { started.set_value(connection.start(config, inbox.message_handler(), inbox.state_handler())); });
    REQUIRE(broker.wait_client());
    if (stall == Stall::Connack)
        REQUIRE(broker.wait_connects(1));
    connection.stop();
    starter.join();
    CHECK_FALSE(started.get_future().get());
    CHECK_FALSE(connection.is_running());
    CHECK(broker.wait_no_client());
}

TEST_CASE("stop returns while the worker waits to retry", "[OrcaMqtt]")
{
    MockBroker         broker;
    broker.set_close_on_accept(true);
    Inbox              inbox;
    RetryLog           retries(std::chrono::hours(1)); // only stop() can end this wait
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());

    CHECK_FALSE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(retries.wait_count(1));
    connection.stop();
    CHECK_FALSE(connection.is_running());
}

TEST_CASE("stop from the message handler ends the connection without joining its own thread", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    std::atomic<int>   handled{0};
    REQUIRE(connection.start(lan_config(broker),
        [&](const std::string& dev_id, const std::string& payload) {
            inbox.message_handler()(dev_id, payload);
            ++handled;
            connection.stop();
        },
        inbox.state_handler()));
    broker.push_publish("device/dev-1/report", "one");
    broker.push_publish("device/dev-1/report", "two");
    REQUIRE(inbox.wait_messages(1));
    REQUIRE(broker.wait_no_client());
    CHECK_FALSE(connection.is_running());
    CHECK_FALSE(connection.is_connected());
    CHECK_FALSE(connection.send_request("dev-1", "late"));

    connection.stop(); // the join
    CHECK(handled.load() == 1);
    CHECK(inbox.messages().size() == 1);

    SECTION("the connection starts again")
    {
        std::atomic<int> second{0};
        REQUIRE(connection.start(lan_config(broker), [&](const std::string&, const std::string&) { ++second; }, [](bool, bool) {}));
        CHECK(broker.connect_count() == 2);
        CHECK(connection.is_connected());
    }
}

TEST_CASE("stop from the state handler ends the connection without joining its own thread", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    // Stop when told of the drop, or when told the connection is back.
    const bool         stop_on_connected = GENERATE(false, true);
    std::atomic<int>   reports{0};
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(),
        [&](bool connected, bool initial) {
            ++reports;
            if (connected == stop_on_connected)
                connection.stop();
            inbox.state_handler()(connected, initial); // recorded last: the test goes on once stop() was called
        }));

    broker.drop_client();
    REQUIRE(inbox.wait_states(stop_on_connected ? 2 : 1));
    REQUIRE(broker.wait_no_client());
    CHECK_FALSE(connection.is_running());
    CHECK_FALSE(connection.is_connected());
    connection.stop(); // the join
    CHECK(reports.load() == (stop_on_connected ? 2 : 1));
    CHECK(broker.connect_count() == (stop_on_connected ? 2 : 1));
}

TEST_CASE("stop from a handler while another thread stops the connection does not deadlock", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker),
        [&](const std::string& dev_id, const std::string& payload) {
            inbox.message_handler()(dev_id, payload);
            // Wait until the other thread's stop() is under way, then stop from here too.
            while (connection.is_running())
                std::this_thread::yield();
            connection.stop();
        },
        inbox.state_handler()));
    broker.push_publish("device/dev-1/report", "one");
    REQUIRE(inbox.wait_messages(1));

    std::thread stopper([&] { connection.stop(); });
    stopper.join(); // returns only if the handler's stop() did not wait for the stopper
    CHECK_FALSE(connection.is_running());
    CHECK(broker.wait_no_client());
}

TEST_CASE("start from a handler is refused", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    std::promise<bool> restarted;
    REQUIRE(connection.start(lan_config(broker),
        [&](const std::string&, const std::string&) {
            restarted.set_value(connection.start(lan_config(broker), [](const std::string&, const std::string&) {}, [](bool, bool) {}));
        },
        inbox.state_handler()));
    broker.push_publish("device/dev-1/report", "go");
    std::future<bool> result = restarted.get_future();
    REQUIRE(result.wait_for(kWait) == std::future_status::ready); // a deadlock fails here
    CHECK_FALSE(result.get());
    CHECK(connection.is_connected());
}

TEST_CASE("start again replaces the running connection", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    CHECK(broker.connect_count() == 2);
    CHECK(connection.is_connected());
    CHECK(inbox.states().empty());
}

TEST_CASE("many reconnect attempts leave no session behind", "[OrcaMqtt]")
{
    MockBroker::Options options;
    options.connack_rc = 3;
    MockBroker         broker(options);
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());

    CHECK_FALSE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(broker.wait_connects(80));
    CHECK(OrcaMqttConnectionTestAccess::live_sessions() <= 1);
    connection.stop();
    CHECK(OrcaMqttConnectionTestAccess::live_sessions() == 0);
}

TEST_CASE("a subscription withdrawn and made again before the worker acts on it stays subscribed", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    // From the message handler, which runs on the worker, the calls only queue work for it:
    // the SUBSCRIBE for the topic and the UNSUBSCRIBE behind it run one after the other.
    const bool         granted_before = GENERATE(true, false);
    std::atomic_bool   done{false};
    REQUIRE(connection.start(lan_config(broker),
        [&](const std::string& dev_id, const std::string& payload) {
            if (payload == "go" && !done.exchange(true)) {
                if (granted_before) {
                    connection.subscribe("dev-y");
                    connection.unsubscribe("dev-x");
                    connection.subscribe("dev-x");
                } else {
                    connection.subscribe("dev-x");
                    connection.unsubscribe("dev-x");
                    connection.subscribe("dev-x");
                }
            }
            // Recorded last: the main thread only goes on once the calls above have queued their work.
            inbox.message_handler()(dev_id, payload);
        },
        inbox.state_handler()));
    if (granted_before) {
        REQUIRE(connection.subscribe("dev-x"));
        REQUIRE(broker.wait_subscribed("device/dev-x/report"));
    }
    broker.push_publish("device/trigger/report", "go");
    REQUIRE(inbox.wait_messages(1));
    REQUIRE(connection.send_request("dev-sync", "sync")); // goes out behind everything the worker sent
    REQUIRE(broker.wait_publishes(1));

    CHECK(broker.is_subscribed("device/dev-x/report"));
    broker.push_report("dev-x", "still here");
    REQUIRE(inbox.wait_messages(2));
    CHECK(inbox.messages()[1].second == "still here");
}

TEST_CASE("unsubscribe between two sessions sends nothing and the topic is not subscribed again", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries(std::chrono::hours(1)); // the worker waits until the restart below
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(connection.subscribe("dev-2"));
    REQUIRE(broker.wait_subscribed("device/dev-2/report"));
    REQUIRE(broker.wait_subscribed(kReportTopic));

    broker.drop_client();
    REQUIRE(retries.wait_count(1)); // between the sessions
    REQUIRE(connection.unsubscribe("dev-1"));
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(broker.wait_subscribed("device/dev-2/report"));
    CHECK_FALSE(broker.is_subscribed(kReportTopic));
    CHECK(broker.unsubscribe_count() == 0);
}

TEST_CASE("start from the state handler is refused", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    std::promise<bool> restarted;
    std::atomic_bool   asked{false};
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), [&](bool, bool) {
        if (!asked.exchange(true))
            restarted.set_value(connection.start(lan_config(broker), [](const std::string&, const std::string&) {}, [](bool, bool) {}));
    }));
    broker.drop_client();
    std::future<bool> result = restarted.get_future();
    REQUIRE(result.wait_for(kWait) == std::future_status::ready); // a deadlock fails here
    CHECK_FALSE(result.get());
    CHECK(connection.is_running());
}

TEST_CASE("a handler that throws does not take the worker down", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    std::atomic_bool   thrown{false};

    SECTION("a state handler told of a drop, with a standard exception or another one")
    {
        const bool standard = GENERATE(true, false);
        REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), [&](bool connected, bool) {
            if (!connected && !thrown.exchange(true)) {
                if (standard)
                    throw std::runtime_error("state handler");
                throw 42;
            }
        }));
        broker.drop_client();
        REQUIRE(broker.wait_connects(2)); // still retrying
        CHECK(connection.is_running());
    }
    SECTION("a message handler throwing something that is not a standard exception")
    {
        REQUIRE(connection.start(lan_config(broker), [&](const std::string&, const std::string&) {
            if (!thrown.exchange(true))
                throw 42;
        }, inbox.state_handler()));
        broker.push_publish("device/dev-1/report", "boom");
        REQUIRE(broker.wait_connects(2)); // the session was dropped and retried
        CHECK(connection.is_running());
    }
}

TEST_CASE("a broker that stops answering PINGREQ is dropped and reconnected", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    OrcaMqttConnection::Config config = lan_config(broker);
    config.keepalive_seconds = 2;
    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));

    broker.set_answer_pings(false); // the socket stays open
    REQUIRE(inbox.wait_states(1));  // dropped for the missing PINGRESP
    CHECK(inbox.states().front() == std::make_pair(false, false));
    REQUIRE(broker.wait_connects(2)); // and the connection is made again
    CHECK(connection.is_running());
}

TEST_CASE("a CONNACK that lands while stop is under way is not reported", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    // The next session stalls at its CONNACK.
    broker.set_stall(Stall::Connack);
    broker.drop_client();
    REQUIRE(inbox.wait_states(1)); // the drop
    REQUIRE(broker.wait_connects(2));

    // Hold the worker at the state lock it takes in handling the CONNACK, past every check
    // that stop() could stop it at, and let stop() have its effect.
    OrcaMqttConnectionTestAccess::forget_connack_rc(connection); // the first session's 0
    auto lock = OrcaMqttConnectionTestAccess::hold_state_lock(connection);
    broker.release_connack();
    while (connection.last_connack_rc() != 0) // the worker has the CONNACK
        std::this_thread::yield();
    OrcaMqttConnectionTestAccess::mark_stopping(connection);
    lock.unlock();
    connection.stop();

    CHECK_FALSE(connection.is_connected());
    CHECK(inbox.states() == std::vector<std::pair<bool, bool>>{{false, false}}); // never told it connected
}

TEST_CASE("two connections run side by side without interfering", "[OrcaMqtt]")
{
    MockBroker         first_broker;
    MockBroker         second_broker;
    Inbox              first_inbox;
    Inbox              second_inbox;
    OrcaMqttConnection first;
    OrcaMqttConnection second;

    REQUIRE(first.start(lan_config(first_broker), first_inbox.message_handler(), first_inbox.state_handler()));
    REQUIRE(second.start(lan_config(second_broker), second_inbox.message_handler(), second_inbox.state_handler()));
    REQUIRE(first.subscribe("dev-1"));
    REQUIRE(second.subscribe("dev-2"));
    REQUIRE(first_broker.wait_subscribed(kReportTopic));
    REQUIRE(second_broker.wait_subscribed("device/dev-2/report"));
    REQUIRE(first.send_request("dev-1", "to first"));
    REQUIRE(second.send_request("dev-2", "to second"));
    first_broker.push_report("dev-1", "from first");
    second_broker.push_report("dev-2", "from second");

    REQUIRE(first_inbox.wait_messages(1));
    REQUIRE(second_inbox.wait_messages(1));
    CHECK(first_inbox.messages()[0] == std::make_pair(std::string("dev-1"), std::string("from first")));
    CHECK(second_inbox.messages()[0] == std::make_pair(std::string("dev-2"), std::string("from second")));
    REQUIRE(first_broker.wait_publishes(1));
    REQUIRE(second_broker.wait_publishes(1));
    CHECK(first_broker.received_requests() == std::vector<std::string>{"to first"});
    CHECK(second_broker.received_requests() == std::vector<std::string>{"to second"});
}

TEST_CASE("a connection stuck connecting does not hold up another one", "[OrcaMqtt]")
{
    MockBroker::Options stalled_options;
    stalled_options.stall = Stall::Connack;
    MockBroker         stalled_broker(stalled_options);
    MockBroker         broker;
    Inbox              stalled_inbox;
    Inbox              inbox;
    OrcaMqttConnection stalled;
    OrcaMqttConnection connection;

    std::promise<bool> stalled_start;
    std::thread        starter([&] { stalled_start.set_value(stalled.start(lan_config(stalled_broker), stalled_inbox.message_handler(), stalled_inbox.state_handler())); });
    REQUIRE(stalled_broker.wait_connects(1)); // its CONNACK is never going to come

    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribed(kReportTopic));
    broker.push_report("dev-1", "alive");
    REQUIRE(inbox.wait_messages(1));
    CHECK(inbox.messages()[0].second == "alive");

    stalled.stop();
    starter.join();
    CHECK_FALSE(stalled_start.get_future().get());
}

TEST_CASE("a connect that stalls after start returned is abandoned at the setup deadline and retried", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    // Every connection from now on is accepted and its CONNECT never answered.
    broker.set_stall(Stall::Connack);
    broker.drop_client();
    REQUIRE(broker.wait_connects(3)); // the stalled attempt is given up and the next one made
    const auto connects = broker.connects();
    CHECK(connects[2].at - connects[1].at >= std::chrono::seconds(10));
    CHECK(connection.is_running());
    CHECK_FALSE(connection.is_connected());
}

// A PUBLISH header whose declared length makes the whole packet `total` bytes.
std::string publish_header_of_total(std::size_t total)
{
    std::size_t remaining = total - 1 - 4; // the fixed header byte and a four-byte length
    std::string header(1, '\x30');
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(remaining % 128);
        remaining /= 128;
        if (remaining != 0)
            byte |= 0x80;
        header.push_back(static_cast<char>(byte));
    } while (remaining != 0);
    return header;
}

TEST_CASE("a packet of exactly 16 MiB is read and one byte more is refused", "[OrcaMqtt]")
{
    constexpr std::size_t kLimit = 16 * 1024 * 1024;
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    OrcaMqttConnection::Config config = lan_config(broker);

    SECTION("at the limit")
    {
        config.keepalive_seconds = 0; // no pings, whose answers would land inside the packet
        REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
        // The whole packet: the fixed header, a four-byte length, the topic and the payload.
        const std::size_t payload_size = kLimit - 1 - 4 - 2 - kReportTopic.size();
        broker.push_raw(orca_mqtt_test::mqtt_publish(kReportTopic, std::string(payload_size, 'p')));
        REQUIRE(inbox.wait_messages(1));
        CHECK(inbox.messages()[0].second.size() == payload_size);
        CHECK(broker.connect_count() == 1);
    }
    SECTION("one byte over")
    {
        config.keepalive_seconds = 0; // no pings, so a missing PINGRESP cannot end the session instead
        REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
        broker.push_raw(publish_header_of_total(kLimit + 1));
        REQUIRE(inbox.wait_states(2)); // dropped, then connected again
        CHECK(inbox.states().front() == std::make_pair(false, false));
        CHECK(broker.connect_count() == 2);
    }
}

TEST_CASE("urls are split into host, port and target, with IPv6 literals", "[OrcaMqtt]")
{
    struct Case { const char* url; const char* host; const char* port; const char* target; };
    const Case cases[] = {
        {"ws://printer.local:8280/mqtt", "printer.local", "8280", "/mqtt"},
        {"ws://10.0.0.5/mqtt", "10.0.0.5", "80", "/mqtt"},
        {"wss://api.example.com/api/v1/printers/abc/mqtt", "api.example.com", "443", "/api/v1/printers/abc/mqtt"},
        {"ws://10.0.0.5:1883", "10.0.0.5", "1883", "/"},
        {"wss://[::1]:8883/mqtt", "::1", "8883", "/mqtt"},
        {"wss://[fe80::1]/mqtt", "fe80::1", "443", "/mqtt"},
        {"ws://::1/mqtt", "::1", "80", "/mqtt"},
        {"ws://fe80::1:2/mqtt", "fe80::1:2", "80", "/mqtt"},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.url)
        {
            std::string host, port, target;
            REQUIRE(OrcaMqttConnectionTestAccess::parse_endpoint(c.url, host, port, target));
            CHECK(host == c.host);
            CHECK(port == c.port);
            CHECK(target == c.target);
        }
    }
    for (const char* url : {"http://x/y", "ws:///mqtt", "ws://[::1/mqtt", "ws://[::1]x/mqtt", "ws://[::1]:/mqtt", "printer.local/mqtt", ""}) {
        DYNAMIC_SECTION(std::string("refused ") + url)
        {
            std::string host, port, target;
            CHECK_FALSE(OrcaMqttConnectionTestAccess::parse_endpoint(url, host, port, target));
        }
    }
}

TEST_CASE("start connects to a bracketed IPv6 literal, and to a host name with a port", "[OrcaMqtt]")
{
    MockBroker::Options options;
    const bool ipv6 = GENERATE(true, false); // a bracketed literal, or localhost:port
    options.address = ipv6 ? "::1" : "127.0.0.1";
    std::unique_ptr<MockBroker> broker;
    try {
        broker = std::make_unique<MockBroker>(options);
    } catch (const std::exception&) {
        SKIP("no IPv6 loopback");
    }
    Inbox              inbox;
    OrcaMqttConnection connection;
    OrcaMqttConnection::Config config = lan_config(*broker);

    config.url = ipv6 ? broker->url() : broker->url_for("localhost");
    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    CHECK(broker->connect_count() == 1);
    if (ipv6)
        CHECK(broker->last_upgrade().header("host") == "[::1]"); // the literal is bracketed in the Host header
}

TEST_CASE("requests and an unsubscribe made just before stop reach the broker", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribed(kReportTopic));
    REQUIRE(sync_through_marker(broker, inbox, 0)); // its SUBACK has been handled

    REQUIRE(connection.send_request("dev-1", "stop-pushing"));
    REQUIRE(connection.send_request("dev-9", "other"));
    REQUIRE(connection.unsubscribe("dev-1"));
    connection.stop();

    REQUIRE(broker.wait_publishes(2));
    CHECK(broker.received_requests() == std::vector<std::string>{"stop-pushing", "other"});
    REQUIRE(broker.wait_unsubscribed(kReportTopic));
    CHECK(broker.unsubscribe_count() == 1);
}

TEST_CASE("stop returns after a short wait when the broker stops reading", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    broker.set_stop_reading(true); // what is sent from now on backs up in the socket buffers
    const std::string big(1024 * 1024, 'q');
    for (int i = 0; i < 64; ++i)
        REQUIRE(connection.send_request("dev-1", big));

    const auto started_at = std::chrono::steady_clock::now();
    connection.stop();
    CHECK_FALSE(connection.is_running());
    CHECK(std::chrono::steady_clock::now() - started_at < std::chrono::seconds(10)); // the bound is one second
    broker.set_stop_reading(false);
}

TEST_CASE("send_request refuses a device whose subscription was rejected and holds requests again once it is subscribed again", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    OrcaMqttConnection connection;
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));
    broker.set_reject_subscriptions(true);
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribes(1));
    REQUIRE(sync_through_marker(broker, inbox, 0)); // the client has handled the rejection

    CHECK_FALSE(connection.send_request("dev-1", "refused"));
    CHECK_FALSE(connection.send_request("dev-1", "refused again"));

    broker.set_reject_subscriptions(false);
    REQUIRE(connection.subscribe("dev-1"));
    CHECK(connection.send_request("dev-1", "accepted")); // held until the subscription is granted
    REQUIRE(broker.wait_publishes(1));
    CHECK(broker.received_requests() == std::vector<std::string>{"accepted"});
}

TEST_CASE("a packet that declares more than 16 MiB closes the session before it is read", "[OrcaMqtt]")
{
    MockBroker         broker;
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    REQUIRE(connection.start(lan_config(broker), inbox.message_handler(), inbox.state_handler()));

    const std::string oversized("\x30\xff\xff\xff\x7f", 5); // PUBLISH declaring 256 MiB - 1
    const int         mode = GENERATE(0, 1, 2);
    switch (mode) {
    case 0: // alone
        broker.push_raw(oversized);
        break;
    case 1: // behind a good packet in the same message: the message is refused as a whole
        broker.push_raw(orca_mqtt_test::mqtt_publish(kReportTopic, "good") + oversized);
        break;
    default: // one byte at a time
        for (const char byte : oversized)
            broker.push_raw(std::string(1, byte));
        break;
    }
    REQUIRE(inbox.wait_states(2)); // dropped, then connected again
    CHECK(inbox.states().front() == std::make_pair(false, false));
    CHECK(inbox.messages().empty());
    CHECK(broker.connect_count() == 2);
}

TEST_CASE("wss connects when the configured CA file signs the broker certificate", "[OrcaMqtt]")
{
    MockBroker         broker(tls_options());
    Inbox              inbox;
    OrcaMqttConnection connection;
    const CaFile       ca(broker.certificate_pem());
    OrcaMqttConnection::Config config = lan_config(broker);
    config.use_tls = true;
    config.ca_file = ca.path();

    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    REQUIRE(connection.subscribe("dev-1"));
    REQUIRE(broker.wait_subscribed(kReportTopic));
    broker.push_report("dev-1", "over tls");
    REQUIRE(inbox.wait_messages(1));
    CHECK(inbox.messages()[0].second == "over tls");
    REQUIRE(connection.send_request("dev-1", "pushall"));
    CHECK(broker.wait_publishes(1));
    CHECK(broker.tls_errors().empty());
}

TEST_CASE("wss sends the bearer on the upgrade and no Origin header", "[OrcaMqtt]")
{
    MockBroker         broker(tls_options());
    Inbox              inbox;
    OrcaMqttConnection connection;
    const CaFile       ca(broker.certificate_pem());
    OrcaMqttConnection::Config config;
    config.url             = broker.url();
    config.use_tls         = true;
    config.ca_file         = ca.path();
    config.bearer_provider = [] { return std::string("cloud-token"); };

    REQUIRE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    CHECK(broker.last_upgrade().header("authorization") == "Bearer cloud-token");
    CHECK_FALSE(broker.last_upgrade().has("origin"));
    CHECK_FALSE(broker.last_connect().has_username);
}

TEST_CASE("wss rejects a certificate issued for another host name", "[OrcaMqtt]")
{
    MockBroker::Options options = tls_options();
    options.tls_names = "DNS:other.example.com"; // trusted below, but not for 127.0.0.1
    MockBroker         broker(options);
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    const CaFile       ca(broker.certificate_pem());
    OrcaMqttConnection::Config config = lan_config(broker);
    config.use_tls = true;
    config.ca_file = ca.path();

    CHECK_FALSE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    REQUIRE(broker.wait_tls_errors(1));
    // The chain is trusted, so only the name check can have refused it: the client aborts
    // from its verify callback, which the server sees as an internal-error alert.
    CHECK(broker.tls_errors().front().find("internal error") != std::string::npos);
    CHECK(broker.connect_count() == 0); // the handshake failed before any MQTT
    CHECK_FALSE(connection.is_connected());
}

TEST_CASE("wss does not trust a certificate that is not signed by the configured CA", "[OrcaMqtt]")
{
    MockBroker         broker(tls_options());
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    // Trust a different self-signed certificate than the one the broker presents.
    const CaFile       ca(orca_mqtt_test::make_self_signed_identity("IP:127.0.0.1").certificate_pem);
    OrcaMqttConnection::Config config = lan_config(broker);
    config.use_tls = true;
    config.ca_file = ca.path();

    CHECK_FALSE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    REQUIRE(broker.wait_tls_errors(1));
    CHECK(broker.tls_errors().front().find("unknown ca") != std::string::npos);
    CHECK(broker.connect_count() == 0);
}

TEST_CASE("wss without a CA file does not trust a self-signed broker", "[OrcaMqtt]")
{
    MockBroker         broker(tls_options());
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    OrcaMqttConnection::Config config = lan_config(broker);
    config.use_tls = true;

    CHECK_FALSE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    REQUIRE(broker.wait_tls_errors(1));
    CHECK(broker.tls_errors().front().find("unknown ca") != std::string::npos);
    CHECK(broker.connect_count() == 0);
}

TEST_CASE("wss fails when the CA file cannot be read", "[OrcaMqtt]")
{
    MockBroker         broker(tls_options());
    Inbox              inbox;
    RetryLog           retries;
    OrcaMqttConnection connection;
    OrcaMqttConnectionTestAccess::set_retry_wait(connection, retries.hook());
    OrcaMqttConnection::Config config = lan_config(broker);
    config.use_tls = true;
    config.ca_file = "/nonexistent/orca-mqtt-test-ca.pem";

    CHECK_FALSE(connection.start(config, inbox.message_handler(), inbox.state_handler()));
    CHECK(connection.is_running());
    CHECK(broker.connect_count() == 0);
}
