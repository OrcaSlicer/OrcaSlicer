#include <catch2/catch_test_macros.hpp>
#include <slic3r/Utils/OrcaMqttConnection.hpp>

#include "orca_mqtt_mock_broker.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Slic3r::OrcaMqttConnection;

// Offset of the CONNECT variable header: 1 (fixed header) + N remaining-length varint bytes.
static size_t mqtt_varheader_offset(const std::vector<uint8_t>& p) {
    size_t i = 1;
    while (i < p.size() && (p[i] & 0x80)) ++i;   // skip varint continuation bytes
    return i + 1;                                // + the final varint byte
}

TEST_CASE("OrcaMqtt parse_endpoint handles ws and wss", "[OrcaMqtt]") {
    OrcaMqttConnection::Endpoint ep;

    REQUIRE(OrcaMqttConnection::parse_endpoint("ws://printer.local:8280/mqtt", ep));
    CHECK(ep.host   == "printer.local");
    CHECK(ep.port   == "8280");
    CHECK(ep.target == "/mqtt");

    REQUIRE(OrcaMqttConnection::parse_endpoint("ws://10.0.0.5/mqtt", ep));
    CHECK(ep.port == "80");

    REQUIRE(OrcaMqttConnection::parse_endpoint("wss://api.example.com/api/v1/printers/abc/mqtt", ep));
    CHECK(ep.host   == "api.example.com");
    CHECK(ep.port   == "443");
    CHECK(ep.target == "/api/v1/printers/abc/mqtt");

    CHECK_FALSE(OrcaMqttConnection::parse_endpoint("http://x/y", ep));
}

TEST_CASE("OrcaMqtt CONNECT packet - no auth (cloud form)", "[OrcaMqtt]") {
    auto p = OrcaMqttConnection::make_connect_packet("OrcaSlicer", "", "", 300);
    REQUIRE(p.size() >= 12);
    CHECK(p[0] == 0x10);                              // CONNECT fixed header
    const size_t v = mqtt_varheader_offset(p);
    CHECK(p[v + 0] == 0x00); CHECK(p[v + 1] == 0x04); // protocol name length
    CHECK(p[v + 2] == 'M'); CHECK(p[v + 3] == 'Q');
    CHECK(p[v + 4] == 'T'); CHECK(p[v + 5] == 'T');
    CHECK(p[v + 6] == 0x04);                          // protocol level 3.1.1
    CHECK(p[v + 7] == 0x02);                          // connect flags: clean session only
    CHECK(((p[v + 8] << 8) | p[v + 9]) == 300);       // keepalive
}

TEST_CASE("OrcaMqtt CONNECT packet - username/password (LAN form)", "[OrcaMqtt]") {
    auto p = OrcaMqttConnection::make_connect_packet("orcaslicer-lan-x", "orcasonar", "code123", 60);
    CHECK(p[0] == 0x10);
    const size_t v = mqtt_varheader_offset(p);
    CHECK(p[v + 7] == (0x02 | 0x80 | 0x40));          // clean session + username + password flags
    const std::string blob(p.begin(), p.end());
    CHECK(blob.find("orcaslicer-lan-x") != std::string::npos);
    CHECK(blob.find("orcasonar")        != std::string::npos);
    CHECK(blob.find("code123")          != std::string::npos);
}

// Auth precedence (spec O3): when a bearer_provider is configured, connect_and_read
// passes empty CONNECT credentials, so the packet must carry clean-session only and
// no username/password flags or payload fields. (The precedence branch itself lives
// in connect_and_read; the cloud-style round trip test exercises it live.)
TEST_CASE("OrcaMqtt CONNECT omits creds when a bearer is configured", "[OrcaMqtt]") {
    auto p = OrcaMqttConnection::make_connect_packet("cid", "", "", 60);
    const size_t v = mqtt_varheader_offset(p);
    CHECK(p[v + 7] == 0x02);                       // clean session only: no 0x80 / 0x40
    const std::string blob(p.begin(), p.end());
    CHECK(blob.find("orcasonar") == std::string::npos);
}

TEST_CASE("OrcaMqtt topic helpers", "[OrcaMqtt]") {
    CHECK(OrcaMqttConnection::request_topic("abc") == "device/abc/request");
    CHECK(OrcaMqttConnection::report_topic("abc")  == "device/abc/report");
}

TEST_CASE("OrcaMqtt PUBLISH packet QoS0", "[OrcaMqtt]") {
    auto p = OrcaMqttConnection::make_publish_packet("device/abc/request", "{\"ok\":1}");
    CHECK((p[0] & 0xf0) == 0x30);   // PUBLISH
    CHECK((p[0] & 0x06) == 0x00);   // QoS 0
    const std::string blob(p.begin(), p.end());
    CHECK(blob.find("device/abc/request") != std::string::npos);
    CHECK(blob.find("{\"ok\":1}")          != std::string::npos);
}

TEST_CASE("OrcaMqtt SUBSCRIBE packet", "[OrcaMqtt]") {
    auto p = OrcaMqttConnection::make_subscribe_packet(7, "device/abc/report", 1);
    CHECK(p[0] == 0x82);                          // SUBSCRIBE + reserved bit
    const size_t v = mqtt_varheader_offset(p);
    CHECK(((p[v] << 8) | p[v + 1]) == 7);         // packet id
    CHECK(p.back() == 1);                         // requested QoS
}

TEST_CASE("OrcaMqtt send_request refuses when not connected", "[OrcaMqtt]") {
    OrcaMqttConnection conn;
    CHECK_FALSE(conn.send_request("abc", "{\"pushing\":{\"command\":\"pushall\",\"sequence_id\":\"20001\"}}"));
}

TEST_CASE("OrcaMqtt start takes a Config", "[OrcaMqtt]") {
    OrcaMqttConnection conn;
    OrcaMqttConnection::Config cfg;
    cfg.url = "ws://127.0.0.1:1/mqtt";           // nothing listening
    cfg.keepalive_seconds = 42;
    // start() returns false (no server) but must compile with the Config overload
    const bool ok = conn.start(cfg, [](auto, auto){}, [](bool, bool){});
    CHECK_FALSE(ok);
    CHECK(conn.last_connack_rc() == -1);
    conn.stop();
}

TEST_CASE("MockBroker starts and reports a url", "[OrcaMqtt]") {
    orca_mqtt_test::MockBroker b;
    CHECK(b.ws_url().rfind("ws://127.0.0.1:", 0) == 0);
    CHECK(b.connect_count() == 0);
}

// --- End-to-end loopback: OrcaMqttConnection against the in-process MockBroker.
// These prove a LAN-style config (CONNECT username/password) and a cloud-style
// config (bearer on the WS upgrade, no CONNECT creds) drive the *same*
// OrcaMqttConnection code path with identical assertions.

// The client's SUBSCRIBE is written asynchronously; wait until the broker records it.
static bool wait_subscribed(orca_mqtt_test::MockBroker& broker, const std::string& topic) {
    for (int i = 0; i < 200; ++i) {
        if (broker.is_subscribed(topic)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static void run_round_trip(bool use_tls_flag_only) {
    orca_mqtt_test::MockBroker broker;
    OrcaMqttConnection conn;
    OrcaMqttConnection::Config cfg;
    cfg.url = broker.ws_url();                     // plaintext regardless
    cfg.use_tls = false;                           // the mock is plaintext; the flag path is unit-tested elsewhere
    if (use_tls_flag_only) cfg.bearer_provider = []{ return std::string("tok"); };
    else { cfg.username = "orcasonar"; cfg.password = "code"; }

    // A mutex + condition_variable rather than a promise: the handler runs on the MQTT
    // worker thread and a second inbound message would throw std::future_error there.
    std::mutex              got_mutex;
    std::condition_variable got_cv;
    bool                    got_any = false;
    std::string             got_id, got_payload;

    REQUIRE(conn.start(cfg,
        [&](const std::string& id, const std::string& payload){
            {
                std::lock_guard<std::mutex> l(got_mutex);
                if (got_any) return;              // keep the first message only
                got_any = true; got_id = id; got_payload = payload;
            }
            got_cv.notify_all();
        },
        [](bool,bool){}));
    REQUIRE(conn.subscribe("dev-1"));
    REQUIRE(wait_subscribed(broker, "device/dev-1/report"));
    REQUIRE(conn.send_request("dev-1", R"({"pushing":{"command":"pushall","sequence_id":"20001"}})"));

    broker.push_report("dev-1", R"({"print":{"command":"push_status","sequence_id":"20001","result":"success"}})");
    std::string id, payload;
    {
        std::unique_lock<std::mutex> l(got_mutex);
        REQUIRE(got_cv.wait_for(l, std::chrono::seconds(3), [&]{ return got_any; }));
        id = got_id; payload = got_payload;
    }
    CHECK(id == "dev-1");
    CHECK(payload.find("push_status") != std::string::npos);

    // the client's command reached the broker on the request topic. The mock records
    // the PUBLISH on its own read-loop thread, so poll rather than check immediately.
    std::vector<std::string> reqs;
    for (int i = 0; i < 200; ++i) {
        reqs = broker.received_requests();
        if (!reqs.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(reqs.size() >= 1);
    CHECK(reqs.front().find("pushall") != std::string::npos);
    conn.stop();
}

TEST_CASE("OrcaMqtt round-trip — LAN-style config",   "[OrcaMqtt]") { run_round_trip(false); }
TEST_CASE("OrcaMqtt round-trip — cloud-style config", "[OrcaMqtt]") { run_round_trip(true);  }

TEST_CASE("OrcaMqtt keepalive runs while the connection is idle", "[OrcaMqtt]") {
    orca_mqtt_test::MockBroker broker;
    OrcaMqttConnection conn;
    OrcaMqttConnection::Config cfg;
    cfg.url = broker.ws_url();
    cfg.keepalive_seconds = 2;

    REQUIRE(conn.start(cfg, [](const std::string&, const std::string&) {}, [](bool, bool) {}));
    for (int i = 0; i < 200 && broker.ping_count() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(broker.ping_count() > 0);
    conn.stop();
}

TEST_CASE("OrcaMqtt reconnects and re-subscribes after a socket drop", "[OrcaMqtt]") {
    orca_mqtt_test::MockBroker broker;
    OrcaMqttConnection conn;
    OrcaMqttConnection::Config cfg; cfg.url = broker.ws_url(); cfg.use_tls = false; cfg.username = "u"; cfg.password = "p";

    std::mutex m; std::vector<std::string> got;
    REQUIRE(conn.start(cfg,
        [&](const std::string&, const std::string& p){ std::lock_guard<std::mutex> l(m); got.push_back(p); },
        [](bool,bool){}));
    REQUIRE(conn.subscribe("dev-1"));
    REQUIRE(wait_subscribed(broker, "device/dev-1/report"));

    broker.drop_client();
    // the worker reconnects with ~1s backoff
    for (int i = 0; i < 300 && broker.connect_count() < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(broker.connect_count() >= 2);

    // a report after the reconnect must still be delivered -> the SUBSCRIBE was re-sent
    REQUIRE(wait_subscribed(broker, "device/dev-1/report"));
    broker.push_report("dev-1", R"({"print":{"command":"push_status","sequence_id":"20002"}})");
    bool delivered = false;
    for (int i = 0; i < 200 && !delivered; ++i) {
        { std::lock_guard<std::mutex> l(m); delivered = !got.empty(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(delivered);
    conn.stop();
}

TEST_CASE("OrcaMqtt auth rejection is terminal (no retry storm)", "[OrcaMqtt]") {
    orca_mqtt_test::MockBroker broker(/*refuse_auth=*/true);
    OrcaMqttConnection conn;
    OrcaMqttConnection::Config cfg; cfg.url = broker.ws_url(); cfg.use_tls = false; cfg.username = "u"; cfg.password = "bad";

    const bool ok = conn.start(cfg, [](const std::string&, const std::string&){}, [](bool,bool){});
    CHECK_FALSE(ok);
    CHECK(conn.last_connack_rc() == 5);
    // worker must have stopped itself (rc 5 is terminal) — give it a moment
    for (int i = 0; i < 100 && conn.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_FALSE(conn.is_running());
    // and it must NOT have hammered the broker with retries
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(broker.connect_count() <= 2);
    conn.stop();
}

// --- MQTT-over-WebSocket stream reassembly. The OrcaSonar /mqtt proxy writes each
// 8 KiB TCP read as its own WebSocket message, so a packet larger than one read
// (e.g. a files.list reply) spans several messages; and several small packets can
// arrive in one. The receiver must reassemble the MQTT stream, not assume one
// packet per message.

TEST_CASE("OrcaMqtt drain_mqtt_packets reassembles across message boundaries", "[OrcaMqtt]") {
    const std::vector<std::uint8_t> a = OrcaMqttConnection::make_publish_packet("device/a/report", R"({"n":1})");
    const std::vector<std::uint8_t> b = OrcaMqttConnection::make_publish_packet("device/a/report", R"({"n":2})");
    const std::string               pa(a.begin(), a.end());
    const std::string               pb(b.begin(), b.end());

    // A partial packet yields nothing and is retained verbatim.
    std::string              stream = pa.substr(0, pa.size() / 2);
    std::vector<std::string> packets;
    OrcaMqttConnection::drain_mqtt_packets(stream, packets);
    CHECK(packets.empty());
    CHECK(stream == pa.substr(0, pa.size() / 2));

    // Completing the packet yields it and drains the stream.
    stream += pa.substr(pa.size() / 2);
    OrcaMqttConnection::drain_mqtt_packets(stream, packets);
    REQUIRE(packets.size() == 1);
    CHECK(packets[0] == pa);
    CHECK(stream.empty());

    // Two packets coalesced in one buffer are both extracted, in order.
    stream = pa + pb;
    packets.clear();
    OrcaMqttConnection::drain_mqtt_packets(stream, packets);
    REQUIRE(packets.size() == 2);
    CHECK(packets[0] == pa);
    CHECK(packets[1] == pb);
    CHECK(stream.empty());

    // A trailing partial packet is kept for the next call.
    stream = pa + pb.substr(0, 2);
    packets.clear();
    CHECK(OrcaMqttConnection::drain_mqtt_packets(stream, packets));
    REQUIRE(packets.size() == 1);
    CHECK(packets[0] == pa);
    CHECK(stream == pb.substr(0, 2));

    // A multibyte remaining length (payload > 127 bytes) is parsed correctly.
    const std::string big(300, 'x');
    const auto        big_packet = OrcaMqttConnection::make_publish_packet("device/a/report", big);
    const std::string pb_big(big_packet.begin(), big_packet.end());
    stream = pb_big;
    packets.clear();
    REQUIRE(OrcaMqttConnection::drain_mqtt_packets(stream, packets));
    REQUIRE(packets.size() == 1);
    CHECK(packets[0] == pb_big);
    CHECK(stream.empty());

    // A malformed header is reported so the caller can drop the connection.
    const std::vector<std::uint8_t> malformed_bytes{0x30, 0x80, 0x80, 0x80, 0x80};
    stream.assign(malformed_bytes.begin(), malformed_bytes.end());
    packets.clear();
    CHECK_FALSE(OrcaMqttConnection::drain_mqtt_packets(stream, packets));
    CHECK(packets.empty());
    CHECK(stream.empty());
}

TEST_CASE("OrcaMqtt delivers a report split across WebSocket messages", "[OrcaMqtt]") {
    orca_mqtt_test::MockBroker broker;
    // Callback state is declared before `conn` so it outlives the worker: if a
    // REQUIRE fails, `conn`'s destructor still runs before this state is destroyed.
    std::mutex               m;
    std::condition_variable  cv;
    std::vector<std::string> got;
    OrcaMqttConnection         conn;
    OrcaMqttConnection::Config cfg;
    cfg.url = broker.ws_url();
    cfg.use_tls = false;
    cfg.username = "u";
    cfg.password = "p";
    REQUIRE(conn.start(cfg,
        [&](const std::string&, const std::string& p) {
            { std::lock_guard<std::mutex> l(m); got.push_back(p); }
            cv.notify_all();
        },
        [](bool, bool) {}));
    REQUIRE(conn.subscribe("dev-1"));
    REQUIRE(wait_subscribed(broker, "device/dev-1/report"));

    const std::string payload = R"({"print":{"command":"push_status","sequence_id":"30001","result":"success"}})";
    const auto        packet  = OrcaMqttConnection::make_publish_packet("device/dev-1/report", payload);
    const std::string bytes(packet.begin(), packet.end());
    const std::size_t third = bytes.size() / 3;
    broker.push_raw(bytes.substr(0, third));
    broker.push_raw(bytes.substr(third, third));
    broker.push_raw(bytes.substr(2 * third));

    bool delivered = false;
    {
        std::unique_lock<std::mutex> l(m);
        delivered = cv.wait_for(l, std::chrono::seconds(3), [&] { return !got.empty(); });
    }
    REQUIRE(delivered);
    {
        std::lock_guard<std::mutex> l(m);
        REQUIRE(got.size() == 1);
        CHECK(got.front().find("push_status") != std::string::npos);
    }
    conn.stop();
}

TEST_CASE("OrcaMqtt delivers two reports coalesced into one WebSocket message", "[OrcaMqtt]") {
    orca_mqtt_test::MockBroker broker;
    // Callback state is declared before `conn` so it outlives the worker: if a
    // REQUIRE fails, `conn`'s destructor still runs before this state is destroyed.
    std::mutex               m;
    std::condition_variable  cv;
    std::vector<std::string> got;
    OrcaMqttConnection         conn;
    OrcaMqttConnection::Config cfg;
    cfg.url = broker.ws_url();
    cfg.use_tls = false;
    cfg.username = "u";
    cfg.password = "p";
    REQUIRE(conn.start(cfg,
        [&](const std::string&, const std::string& p) {
            { std::lock_guard<std::mutex> l(m); got.push_back(p); }
            cv.notify_all();
        },
        [](bool, bool) {}));
    REQUIRE(conn.subscribe("dev-1"));
    REQUIRE(wait_subscribed(broker, "device/dev-1/report"));

    const auto        p1 = OrcaMqttConnection::make_publish_packet("device/dev-1/report", R"({"print":{"command":"push_status","sequence_id":"31001"}})");
    const auto        p2 = OrcaMqttConnection::make_publish_packet("device/dev-1/report", R"({"print":{"command":"push_status","sequence_id":"31002"}})");
    const std::string both(std::string(p1.begin(), p1.end()) + std::string(p2.begin(), p2.end()));
    broker.push_raw(both);

    bool two = false;
    {
        std::unique_lock<std::mutex> l(m);
        two = cv.wait_for(l, std::chrono::seconds(3), [&] { return got.size() >= 2; });
    }
    REQUIRE(two);
    {
        std::lock_guard<std::mutex> l(m);
        CHECK(got[0].find("31001") != std::string::npos);
        CHECK(got[1].find("31002") != std::string::npos);
    }
    conn.stop();
}

TEST_CASE("OrcaMqtt stop() unblocks a stalled handshake", "[OrcaMqtt]") {
    // A peer that completes the WebSocket upgrade and CONNECT but never answers:
    // the worker blocks in the synchronous CONNACK read. stop() must shut the
    // socket down and join it promptly rather than wait out the 10s deadline.
    orca_mqtt_test::MockBroker broker(/*refuse_auth=*/false, /*stall_connack=*/true);
    OrcaMqttConnection         conn;
    OrcaMqttConnection::Config cfg;
    cfg.url = broker.ws_url();
    cfg.use_tls = false;
    cfg.username = "u";
    cfg.password = "p";

    // start() waits up to 10s for CONNACK, so run it off the test thread.
    std::thread starter([&] { conn.start(cfg, [](const std::string&, const std::string&) {}, [](bool, bool) {}); });
    struct FinalJoin {
        std::thread& thread;
        ~FinalJoin() {
            if (thread.joinable())
                thread.join();
        }
    } final_join{starter};

    // Wait until the broker has taken the CONNECT: the worker is now in the read.
    for (int i = 0; i < 300 && broker.connect_count() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(broker.connect_count() > 0);

    const auto started_at = std::chrono::steady_clock::now();
    conn.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    CHECK(elapsed < std::chrono::seconds(3));
    CHECK_FALSE(conn.is_running());
}
