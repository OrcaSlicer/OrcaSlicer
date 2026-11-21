#include "slic3r/Utils/MqttWebSocketClient.hpp"

#include "fake_mqtt_broker.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Mqtt = Slic3r::Mqtt;
using Bytes    = std::vector<std::uint8_t>;

TEST_CASE("CONNECT carries protocol level 4, clean session and credentials", "[MqttWebSocketClient]")
{
    const Bytes expected{0x10, 38,  0,   4,   'M', 'Q', 'T', 'T', 4,   0xc2, 0,   30,  0, 10, 'o', 'r', 'c', 'a', '1', '2',
                         '3',  '4', '5', '6', 0,   6,   'e', 'l', 'e', 'g',  'o', 'o', 0, 6,  '1', '2', '3', '4', '5', '6'};
    CHECK(Mqtt::encode_connect("orca123456", "elegoo", "123456", 30) == expected);
}

TEST_CASE("SUBSCRIBE uses reserved flags 0010 and a non zero packet id", "[MqttWebSocketClient]")
{
    CHECK(Mqtt::encode_subscribe(1, {"a/b", "c"}) == Bytes{0x82, 12, 0, 1, 0, 3, 'a', '/', 'b', 1, 0, 1, 'c', 1});
    CHECK_THROWS_AS(Mqtt::encode_subscribe(0, {"a/b"}), std::invalid_argument);
}

TEST_CASE("PUBLISH at QoS 0 has no packet identifier", "[MqttWebSocketClient]")
{
    CHECK(Mqtt::encode_publish("t", "{}") == Bytes{0x30, 5, 0, 1, 't', '{', '}'});
    CHECK_THROWS_AS(Mqtt::encode_publish(std::string(65536, 't'), "{}"), std::length_error);
}

TEST_CASE("A remaining length over 127 uses two bytes", "[MqttWebSocketClient]")
{
    const std::string payload(200, 'x');
    const Bytes bytes = Mqtt::encode_publish("t", payload);
    REQUIRE(bytes.size() == 206);
    CHECK(bytes[1] == 0xcb);
    CHECK(bytes[2] == 1);
    Mqtt::PacketDecoder decoder;
    decoder.feed(bytes.data(), 2);
    CHECK_FALSE(decoder.next());
    decoder.feed(bytes.data() + 2, bytes.size() - 2);
    const auto packet = decoder.next();
    REQUIRE(packet);
    const auto publish = Mqtt::parse_publish(*packet);
    REQUIRE(publish);
    CHECK(publish->topic == "t");
    CHECK(publish->payload == payload);
    CHECK_FALSE(publish->packet_id);
}

TEST_CASE("The decoder splits packets that arrive glued and reassembles split ones", "[MqttWebSocketClient]")
{
    Mqtt::PacketDecoder decoder;
    const Bytes replies{0x20, 2, 0, 0, 0x90, 3, 0, 1, 1};
    decoder.feed(replies.data(), replies.size());
    const auto connack = decoder.next();
    REQUIRE(connack);
    CHECK(connack->type == 2);
    CHECK(connack->body == Bytes{0, 0});
    const auto suback = decoder.next();
    REQUIRE(suback);
    CHECK(suback->type == 9);
    CHECK(suback->body == Bytes{0, 1, 1});
    CHECK_FALSE(decoder.next());

    const Bytes bytes{0x30, 5, 0, 1, 't', '{', '}'};
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        decoder.feed(&bytes[i], 1);
        CHECK_FALSE(decoder.next());
        CHECK_FALSE(decoder.failed());
    }
    decoder.feed(&bytes.back(), 1);
    const auto packet = decoder.next();
    REQUIRE(packet);
    CHECK(packet->type == 3);
    CHECK(packet->flags == 0);
    CHECK(packet->body == Bytes{0, 1, 't', '{', '}'});
    CHECK_FALSE(decoder.next());
}

TEST_CASE("A remaining length longer than four bytes fails the decoder", "[MqttWebSocketClient]")
{
    Mqtt::PacketDecoder decoder;
    const Bytes bytes{0x30, 0xff, 0xff, 0xff, 0xff, 0x01};
    decoder.feed(bytes.data(), bytes.size());
    CHECK(decoder.failed());
    CHECK_FALSE(decoder.next());
    const Bytes valid{0x20, 2, 0, 0};
    decoder.feed(valid.data(), valid.size());
    CHECK(decoder.failed());
    CHECK_FALSE(decoder.next());
}

TEST_CASE("A QoS 1 PUBLISH exposes its packet identifier", "[MqttWebSocketClient]")
{
    const Bytes bytes{0x32, 8, 0, 1, 't', 0, 7, 'a', 0, 'b'};
    Mqtt::PacketDecoder decoder;
    decoder.feed(bytes.data(), bytes.size());
    const auto packet = decoder.next();
    REQUIRE(packet);
    const auto publish = Mqtt::parse_publish(*packet);
    REQUIRE(publish);
    CHECK(publish->topic == "t");
    CHECK(publish->payload == std::string("a\0b", 3));
    CHECK(publish->packet_id == 7);
    CHECK_FALSE(Mqtt::parse_publish({3, 2, {0, 1, 't', 0}}));
    CHECK_FALSE(Mqtt::parse_publish({3, 2, {0, 1, 't', 0, 0}}));
}

TEST_CASE("A PUBLISH whose topic length exceeds the body is rejected", "[MqttWebSocketClient]")
{
    CHECK_FALSE(Mqtt::parse_publish({3, 0, {0, 2, 't'}}));
    CHECK_FALSE(Mqtt::parse_publish({3, 0, {0}}));
}

TEST_CASE("PUBACK carries the packet identifier and DISCONNECT has an empty body", "[MqttWebSocketClient]")
{
    CHECK(Mqtt::encode_puback(0x1234) == Bytes{0x40, 2, 0x12, 0x34});
    CHECK(Mqtt::encode_disconnect() == Bytes{0xe0, 0});
}

TEST_CASE("The decoder accepts one MiB of buffered data and fails above it", "[MqttWebSocketClient]")
{
    Bytes bytes(1024 * 1024);
    bytes[0] = 0x30;
    bytes[1] = 0xfc;
    bytes[2] = 0xff;
    bytes[3] = 0x3f;
    Mqtt::PacketDecoder decoder;
    decoder.feed(bytes.data(), bytes.size());
    REQUIRE_FALSE(decoder.failed());
    const auto packet = decoder.next();
    REQUIRE(packet);
    CHECK(packet->body.size() == bytes.size() - 4);

    decoder.feed(bytes.data(), bytes.size());
    decoder.feed(bytes.data(), 1);
    CHECK(decoder.failed());
    CHECK_FALSE(decoder.next());
}

namespace {
using mqtt_test::FakeMqttBroker;
using Session = FakeMqttBroker::Session;
using Client  = Slic3r::MqttWebSocketClient;
using Status  = Client::Status;
using Clock   = std::chrono::steady_clock;
using namespace std::chrono_literals;

Status connect(Client& client, const std::string& port)
{
    std::uint8_t code = 0xff;
    return client.connect("127.0.0.1", port, "test-client", "user", "password", code);
}

void accept_connect(Session& session)
{
    session.read_packet();
    session.send({0x20, 2, 0, 0});
}

bool want_all(const std::string&, const std::string&) { return true; }
} // namespace

TEST_CASE("A broker that accepts gets CONNECT, SUBSCRIBE and a published message", "[MqttWebSocketClient]")
{
    std::vector<std::uint8_t> types;
    Bytes subscribe_body;
    std::string request;
    FakeMqttBroker broker([&](Session& session) {
        types.push_back(session.read_packet().type);
        session.send({0x20, 2, 0, 0});
        const auto subscription = session.read_packet();
        types.push_back(subscription.type);
        subscribe_body = subscription.body;
        session.send({0x90, 3, 0, 1, 1});
        const auto packet = session.read_packet();
        types.push_back(packet.type);
        if (auto publish = Mqtt::parse_publish(packet))
            request = publish->payload;
        session.send(Mqtt::encode_publish("x/y", "hello"));
        types.push_back(session.read_packet().type);
        session.close();
    });
    Client client(Clock::now() + 3s);
    std::uint8_t code = 0xff;
    REQUIRE(client.connect("127.0.0.1", broker.port(), "test-client", "user", "password", code) == Status::Ok);
    CHECK(code == 0);
    REQUIRE(client.subscribe({"x/y"}) == Status::Ok);
    REQUIRE(client.publish("request", "fetch") == Status::Ok);
    std::string payload;
    REQUIRE(client.wait_for([](const auto& topic, const auto&) { return topic == "x/y"; }, payload) == Status::Ok);
    CHECK(payload == "hello");
    client.close();
    client.close();
    broker.join();
    CHECK(types == Bytes{1, 8, 3, 14});
    CHECK(subscribe_body == Bytes{0, 1, 0, 3, 'x', '/', 'y', 1});
    CHECK(broker.published_topics() == std::vector<std::string>{"request"});
    CHECK(request == "fetch");
}

TEST_CASE("A host name is resolved before connecting", "[MqttWebSocketClient]")
{
    FakeMqttBroker broker(accept_connect);
    // Windows may try ::1 first and retry its SYN for about two seconds before it reaches 127.0.0.1.
    Client client(Clock::now() + 5s);
    std::uint8_t code = 0xff;
    REQUIRE(client.connect("localhost", broker.port(), "test-client", "user", "password", code) == Status::Ok);
    CHECK(code == 0);
    broker.join();
}

TEST_CASE("A missing host or port fails connection validation", "[MqttWebSocketClient]")
{
    const bool missing_host = GENERATE(true, false);
    Client client(Clock::now() + 3s);
    std::uint8_t code = 0xff;
    CHECK(client.connect(missing_host ? "" : "localhost", missing_host ? "9001" : "", "test-client", "user", "password", code) ==
          Status::ConnectFailed);
}

TEST_CASE("The handshake offers the mqtt subprotocol", "[MqttWebSocketClient]")
{
    std::string protocol;
    FakeMqttBroker broker([&](Session& session) {
        protocol = session.subprotocol();
        accept_connect(session);
    });
    Client client(Clock::now() + 3s);
    REQUIRE(connect(client, broker.port()) == Status::Ok);
    broker.join();
    CHECK(protocol == "mqtt");
}

TEST_CASE("A rejected CONNACK reports its return code", "[MqttWebSocketClient]")
{
    FakeMqttBroker broker([](Session& session) {
        session.read_packet();
        session.send({0x20, 2, 0, 5});
    });
    Client client(Clock::now() + 3s);
    std::uint8_t code = 0xff;
    CHECK(client.connect("127.0.0.1", broker.port(), "test-client", "user", "password", code) == Status::ConnackRejected);
    CHECK(code == 5);
}

TEST_CASE("A SUBACK failure code rejects the subscription", "[MqttWebSocketClient]")
{
    FakeMqttBroker broker([](Session& session) {
        accept_connect(session);
        session.read_packet();
        session.send({0x90, 4, 0, 1, 1, 0x80});
    });
    Client client(Clock::now() + 3s);
    REQUIRE(connect(client, broker.port()) == Status::Ok);
    CHECK(client.subscribe({"first", "second"}) == Status::SubscribeRejected);
}

TEST_CASE("A QoS 1 message is acknowledged with its packet id", "[MqttWebSocketClient]")
{
    Mqtt::Packet ack{};
    FakeMqttBroker broker([&](Session& session) {
        accept_connect(session);
        session.send({0x32, 6, 0, 1, 't', 0, 9, 'x'});
        ack = session.read_packet();
    });
    Client client(Clock::now() + 3s);
    REQUIRE(connect(client, broker.port()) == Status::Ok);
    std::string payload;
    REQUIRE(client.wait_for(want_all, payload) == Status::Ok);
    client.close();
    broker.join();
    CHECK(ack.type == 4);
    CHECK(ack.flags == 0);
    CHECK(ack.body == Bytes{0, 9});
}

TEST_CASE("Messages the caller does not want are skipped", "[MqttWebSocketClient]")
{
    FakeMqttBroker broker([](Session& session) {
        accept_connect(session);
        session.send({0x30, 8, 0, 5, 'o', 't', 'h', 'e', 'r', 'a', 0x30, 9});
        session.send({0, 6, 'w', 'a', 'n', 't', 'e', 'd', 'b'});
    });
    Client client(Clock::now() + 3s);
    REQUIRE(connect(client, broker.port()) == Status::Ok);
    std::string payload;
    CHECK(client.wait_for([](const auto& topic, const auto&) { return topic == "wanted"; }, payload) == Status::Ok);
    CHECK(payload == "b");
    broker.join();
}

TEST_CASE("A silent broker makes the session fail within the deadline", "[MqttWebSocketClient]")
{
    FakeMqttBroker broker([](Session& session) {
        session.read_packet();
        session.stall();
    });
    const auto start = Clock::now();
    Client client(start + 500ms);
    CHECK(connect(client, broker.port()) == Status::Timeout);
    client.close();
    CHECK(Clock::now() - start < 1500ms);
}

TEST_CASE("A broker that never finishes the handshake times out", "[MqttWebSocketClient]")
{
    FakeMqttBroker broker([](Session&) {}, FakeMqttBroker::Handshake::Stall);
    const auto start = Clock::now();
    Client client(start + 500ms);
    CHECK(connect(client, broker.port()) == Status::Timeout);
    CHECK(Clock::now() - start < 1500ms);
}

TEST_CASE("An unreachable port fails the session before the deadline", "[MqttWebSocketClient]")
{
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::acceptor acceptor{ioc, {boost::asio::ip::make_address("127.0.0.1"), 0}};
    const auto port = std::to_string(acceptor.local_endpoint().port());
    acceptor.close();
    const auto deadline = Clock::now() + 5s;
    Client client(deadline);
    CHECK(connect(client, port) == Status::ConnectFailed);
    CHECK(Clock::now() < deadline);
}

TEST_CASE("A closed connection while waiting reports Closed", "[MqttWebSocketClient]")
{
    FakeMqttBroker broker([](Session& session) {
        accept_connect(session);
        session.read_packet();
        session.send({0x90, 3, 0, 1, 1});
        session.close();
    });
    Client client(Clock::now() + 3s);
    REQUIRE(connect(client, broker.port()) == Status::Ok);
    REQUIRE(client.subscribe({"x"}) == Status::Ok);
    std::string payload;
    CHECK(client.wait_for(want_all, payload) == Status::Closed);
}

TEST_CASE("A session rejects unexpected CONNACK types and flags", "[MqttWebSocketClient]")
{
    const auto header = GENERATE(0xf0, 0x21);
    FakeMqttBroker broker([&](Session& session) {
        session.read_packet();
        session.send({static_cast<std::uint8_t>(header), 2, 0, 0});
    });
    Client client(Clock::now() + 3s);
    CHECK(connect(client, broker.port()) == Status::ProtocolError);
}

TEST_CASE("A SUBACK for another packet identifier fails the session", "[MqttWebSocketClient]")
{
    FakeMqttBroker broker([](Session& session) {
        accept_connect(session);
        session.read_packet();
        session.send({0x90, 3, 0, 2, 1});
    });
    Client client(Clock::now() + 3s);
    REQUIRE(connect(client, broker.port()) == Status::Ok);
    CHECK(client.subscribe({"x"}) == Status::ProtocolError);
}

TEST_CASE("A broker that ignores the close handshake does not hold the client past the close bound", "[MqttWebSocketClient]")
{
    FakeMqttBroker broker([](Session& session) {
        accept_connect(session);
        session.read_packet();
        session.stall();
    });
    const auto start = Clock::now();
    Client client(start + 500ms);
    REQUIRE(connect(client, broker.port()) == Status::Ok);
    client.close();
    client.close();
    CHECK(Clock::now() - start < 1500ms);
}

TEST_CASE("Closing after DISCONNECT waits only briefly for the close handshake", "[MqttWebSocketClient]")
{
    FakeMqttBroker broker([](Session& session) {
        accept_connect(session);
        session.read_packet();
        session.stall();
    });
    Client client(Clock::now() + 3s);
    REQUIRE(connect(client, broker.port()) == Status::Ok);
    const auto start = Clock::now();
    client.close();
    // The client waits at most 250 ms for the close echo, far short of the three second deadline.
    CHECK(Clock::now() - start < 750ms);
}

TEST_CASE("A publication arriving before SUBACK is acknowledged and delivered", "[MqttWebSocketClient]")
{
    Mqtt::Packet ack{};
    FakeMqttBroker broker([&](Session& session) {
        accept_connect(session);
        session.read_packet();
        session.send({0x32, 6, 0, 1, 't', 0, 7, 'x'});
        ack = session.read_packet();
        session.send({0x90, 3, 0, 1, 1});
    });
    Client client(Clock::now() + 3s);
    REQUIRE(connect(client, broker.port()) == Status::Ok);
    REQUIRE(client.subscribe({"t"}) == Status::Ok);
    std::string payload;
    REQUIRE(client.wait_for(want_all, payload) == Status::Ok);
    CHECK(payload == "x");
    broker.join();
    CHECK(ack.type == 4);
    CHECK(ack.body == Bytes{0, 7});
}
