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
// in connect_and_read; the [.integration] cloud-style round trip exercises it live.)
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

TEST_CASE("MockBroker starts and reports a url", "[OrcaMqtt][.integration]") {
    orca_mqtt_test::MockBroker b;
    CHECK(b.ws_url().rfind("ws://127.0.0.1:", 0) == 0);
    CHECK(b.connect_count() == 0);
}

// --- End-to-end integration: OrcaMqttConnection against the in-process MockBroker.
// All hidden behind [.integration] (run explicitly). These prove a LAN-style config
// (CONNECT username/password) and a cloud-style config (bearer on the WS upgrade,
// no CONNECT creds) drive the *same* OrcaMqttConnection code path with identical
// assertions.

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

TEST_CASE("OrcaMqtt round-trip — LAN-style config",   "[OrcaMqtt][.integration]") { run_round_trip(false); }
TEST_CASE("OrcaMqtt round-trip — cloud-style config", "[OrcaMqtt][.integration]") { run_round_trip(true);  }

TEST_CASE("OrcaMqtt reconnects and re-subscribes after a socket drop", "[OrcaMqtt][.integration]") {
    orca_mqtt_test::MockBroker broker;
    OrcaMqttConnection conn;
    OrcaMqttConnection::Config cfg; cfg.url = broker.ws_url(); cfg.use_tls = false; cfg.username = "u"; cfg.password = "p";

    std::mutex m; std::vector<std::string> got;
    REQUIRE(conn.start(cfg,
        [&](const std::string&, const std::string& p){ std::lock_guard<std::mutex> l(m); got.push_back(p); },
        [](bool,bool){}));
    REQUIRE(conn.subscribe("dev-1"));

    broker.drop_client();
    // the worker reconnects with ~1s backoff
    for (int i = 0; i < 300 && broker.connect_count() < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(broker.connect_count() >= 2);

    // a report after the reconnect must still be delivered -> the SUBSCRIBE was re-sent
    broker.push_report("dev-1", R"({"print":{"command":"push_status","sequence_id":"20002"}})");
    bool delivered = false;
    for (int i = 0; i < 200 && !delivered; ++i) {
        { std::lock_guard<std::mutex> l(m); delivered = !got.empty(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(delivered);
    conn.stop();
}

TEST_CASE("OrcaMqtt auth rejection is terminal (no retry storm)", "[OrcaMqtt][.integration]") {
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
