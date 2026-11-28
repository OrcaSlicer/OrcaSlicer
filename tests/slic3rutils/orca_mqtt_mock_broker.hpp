#pragma once

// In-process MQTT-over-WebSocket broker (ws:// and wss://) for the OrcaMqttConnection tests.
//
// It speaks just enough of MQTT 3.1.1 to drive the real client end to end without a
// network: CONNECT/CONNACK, SUBSCRIBE/SUBACK, UNSUBSCRIBE/UNSUBACK, client PUBLISH,
// PINGREQ and DISCONNECT. It records what the client sent (the WebSocket upgrade
// headers, the CONNECT fields, the QoS of every SUBSCRIBE and PUBLISH, the TLS
// handshake failures) and can refuse the CONNECT, hold or reject SUBACKs, answer the
// upgrade with another subprotocol, stall at the TLS handshake, the upgrade or the
// CONNACK, fragment what it sends into WebSocket frames, or drop the connection, so
// the tests can wait on the broker's state rather than sleep.
//
// A broker serves one client at a time; run one per connection under test.

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/beast/websocket/stream_base.hpp>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/types.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace orca_mqtt_test {

namespace net   = boost::asio;
namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace ws    = boost::beast::websocket;
using tcp       = boost::asio::ip::tcp;

// A self-signed certificate, in memory.
struct TlsIdentity {
    std::string certificate_pem;
    std::string private_key_pem;
};

inline std::string pem_from_bio(BIO* bio)
{
    char*             data = nullptr;
    const long        size = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<std::size_t>(size));
}

// `alt_names` is an OpenSSL subjectAltName list, for example "IP:127.0.0.1,DNS:localhost".
inline TlsIdentity make_self_signed_identity(const std::string& alt_names)
{
    EVP_PKEY* key  = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    X509*     cert = X509_new();
    if (!key || !cert)
        throw std::runtime_error("cannot create a test certificate");
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 24 * 3600);
    X509_set_pubkey(cert, key);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("orca-mqtt-test"), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    X509V3_CTX context;
    X509V3_set_ctx(&context, cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* names = X509V3_EXT_nconf_nid(nullptr, &context, NID_subject_alt_name, alt_names.c_str());
    X509_add_ext(cert, names, -1);
    X509_EXTENSION_free(names);
    if (X509_sign(cert, key, EVP_sha256()) == 0)
        throw std::runtime_error("cannot sign a test certificate");

    TlsIdentity identity;
    BIO*        bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    identity.certificate_pem = pem_from_bio(bio);
    BIO_free(bio);
    bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    identity.private_key_pem = pem_from_bio(bio);
    BIO_free(bio);
    X509_free(cert);
    EVP_PKEY_free(key);
    return identity;
}

// MQTT fixed header + remaining-length varint + body.
inline std::string mqtt_packet(std::uint8_t header, const std::string& body)
{
    std::string packet(1, static_cast<char>(header));
    std::size_t length = body.size();
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(length % 128);
        length /= 128;
        if (length != 0)
            byte |= 0x80;
        packet.push_back(static_cast<char>(byte));
    } while (length != 0);
    return packet + body;
}

inline std::string mqtt_string(const std::string& value)
{
    std::string out;
    out.push_back(static_cast<char>(value.size() >> 8));
    out.push_back(static_cast<char>(value.size() & 0xff));
    return out + value;
}

// A server-to-client QoS 0 PUBLISH.
inline std::string mqtt_publish(const std::string& topic, const std::string& payload)
{
    return mqtt_packet(0x30, mqtt_string(topic) + payload);
}

// A server-to-client QoS 1 PUBLISH.
inline std::string mqtt_publish_qos1(const std::string& topic, std::uint16_t packet_id, const std::string& payload)
{
    return mqtt_packet(0x32, mqtt_string(topic) + std::string{static_cast<char>(packet_id >> 8), static_cast<char>(packet_id & 0xff)} + payload);
}

struct ConnectRecord {
    std::string client_id;
    std::string username;
    std::string password;
    bool        has_username  = false;
    bool        has_password  = false;
    bool        clean_session = false;
    int         keepalive     = -1;
    int         protocol_level = 0;
    std::chrono::steady_clock::time_point at;
};

// The WebSocket upgrade request of one connection, headers by lower-case name.
struct UpgradeRecord {
    std::map<std::string, std::string> headers;
    std::string header(const std::string& name) const
    {
        const auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
    bool has(const std::string& name) const { return headers.count(name) != 0; }
};

struct PublishRecord {
    std::string topic;
    std::string payload;
    int         qos = 0;
};

// Where the broker stops answering the client.
enum class Stall { None, TlsHandshake, Upgrade, Connack };

struct MockBrokerOptions {
    bool        tls          = false;
    int         connack_rc   = 0;     // non-zero: answer every CONNECT with this return code and close
    Stall       stall        = Stall::None;
    bool        listen       = true;  // false: the port is bound but not listening, so connecting is refused
    std::string subprotocol  = "mqtt"; // what the upgrade answers; empty answers none
    std::string tls_names    = "IP:127.0.0.1,DNS:localhost"; // subjectAltName of the certificate
    std::string address      = "127.0.0.1"; // what it listens on, "::1" for IPv6
    std::string connack_trailer;       // bytes sent in the same WebSocket message as the CONNACK
};

class MockBroker
{
    // Guard for waits; a wait only ever ends early, so this is no bound on how long
    // anything may take, only on how long a failing test hangs.
    static constexpr std::chrono::seconds kWait{30};

    template <class Fn> auto locked(Fn fn) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return fn();
    }

    template <class Pred> bool wait(Pred pred)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, kWait, pred);
    }

public:
    using Options = MockBrokerOptions;

    explicit MockBroker(Options options = Options())
        : m_tls(options.tls)
        , m_connack_rc(options.connack_rc)
        , m_stall(options.stall)
        , m_subprotocol(options.subprotocol)
        , m_connack_trailer(options.connack_trailer)
        , m_address(options.address)
        , m_acceptor(m_io)
    {
        if (m_tls) {
            m_identity = make_self_signed_identity(options.tls_names);
            m_ssl.emplace(net::ssl::context::tls_server);
            m_ssl->use_certificate_chain(net::buffer(m_identity.certificate_pem));
            m_ssl->use_private_key(net::buffer(m_identity.private_key_pem), net::ssl::context::pem);
        }
        const tcp::endpoint endpoint(net::ip::make_address(m_address), 0);
        m_acceptor.open(endpoint.protocol());
        m_acceptor.set_option(net::socket_base::reuse_address(true));
        m_acceptor.bind(endpoint);
        m_port = m_acceptor.local_endpoint().port();
        if (!options.listen)
            return;
        m_acceptor.listen(net::socket_base::max_listen_connections);
        m_thread = std::thread([this] { run(); });
    }

    ~MockBroker()
    {
        m_stopping.store(true);
        drop_client();
        m_cv.notify_all();
        // Wake the blocking accept() with a connection of our own.
        boost::system::error_code ec;
        tcp::socket               wake(m_io);
        wake.connect(tcp::endpoint(net::ip::make_address(m_address), m_port), ec);
        if (m_thread.joinable())
            m_thread.join();
    }

    MockBroker(const MockBroker&)            = delete;
    MockBroker& operator=(const MockBroker&) = delete;

    std::string url() const { return url_for(m_address.find(':') == std::string::npos ? m_address : "[" + m_address + "]"); }
    // The url with another host in front of the broker's port, as the tests spell it.
    std::string url_for(const std::string& host) const { return std::string(m_tls ? "wss://" : "ws://") + host + ":" + std::to_string(m_port) + "/mqtt"; }
    unsigned short port() const { return m_port; }
    // The self-signed certificate a wss:// client has to trust.
    const std::string& certificate_pem() const { return m_identity.certificate_pem; }

    // --- behaviour switches, changeable while running
    void set_connack_rc(int rc) { m_connack_rc.store(rc); }
    // Return codes for the next CONNECTs, one each, before set_connack_rc() applies again.
    void script_connack_rcs(std::vector<int> rcs) { locked([&] { m_connack_script = std::move(rcs); m_connack_script_index = 0; return 0; }); }
    // Where the broker stops answering, for the connections accepted from now on.
    void set_stall(Stall stall) { m_stall.store(stall); }
    // Close every new TCP connection at once, before any handshake.
    void set_close_on_accept(bool close) { m_close_on_accept.store(close); }
    void set_hold_subacks(bool hold) { m_hold_subacks.store(hold); }
    // Send the CONNACK a Stall::Connack broker is holding back.
    void release_connack() { write(mqtt_packet(0x20, std::string{0x00, 0x00})); }
    // Stop reading what the client sends, so its writes back up.
    void set_stop_reading(bool stop)
    {
        m_stop_reading.store(stop);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cv.notify_all();
    }
    // Stop answering PINGREQs while keeping the socket open.
    void set_answer_pings(bool answer) { m_answer_pings.store(answer); }
    // At most `limit` QoS 1 publishes from queue_qos1() unacknowledged at once; 0 is no limit.
    void set_inflight_limit(std::size_t limit) { locked([&] { m_inflight_limit = limit; return 0; }); }
    // QoS 1 PUBLISHes on `topic`, sent as the in-flight limit allows and as PUBACKs come in.
    void queue_qos1(const std::string& topic, const std::vector<std::string>& payloads)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const std::string& payload : payloads)
            m_qos1_pending.emplace_back(topic, payload);
        pump_qos1_locked();
    }
    void set_reject_subscriptions(bool reject) { m_reject_subscriptions.store(reject); }
    // Frames of at most `bytes` (8 or more) bytes for everything sent; 0 sends one frame per message.
    void set_fragment_size(std::size_t bytes) { m_fragment.store(bytes); }
    // Send the SUBACKs held back while set_hold_subacks(true) was in effect: the first
    // `count` of them, in the order they were produced.
    void release_subacks(std::size_t count = static_cast<std::size_t>(-1))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (count == static_cast<std::size_t>(-1))
            m_hold_subacks.store(false);
        count = std::min(count, m_held_subacks.size());
        for (std::size_t i = 0; i < count; ++i)
            write_locked(m_held_subacks[i]);
        m_held_subacks.erase(m_held_subacks.begin(), m_held_subacks.begin() + static_cast<std::ptrdiff_t>(count));
    }

    // --- server to client
    // PUBLISH on device/<dev_id>/report, only to a client subscribed to that topic.
    void push_report(const std::string& dev_id, const std::string& payload)
    {
        const std::string topic = "device/" + dev_id + "/report";
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_subscriptions.find(topic) == m_subscriptions.end())
            return;
        write_locked(mqtt_publish(topic, payload));
    }
    // PUBLISH on any topic, regardless of subscriptions.
    void push_publish(const std::string& topic, const std::string& payload)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        write_locked(mqtt_publish(topic, payload));
    }
    // Arbitrary bytes as one binary WebSocket message. Lets a test split one MQTT packet
    // across messages or coalesce several into one.
    void push_raw(const std::string& bytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        write_locked(bytes);
    }
    // Shut down the live socket; the client sees its connection drop.
    void drop_client()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_close)
            m_close();
    }

    // --- what the client did
    int accept_count() const { return m_accept_count.load(); }
    int connect_count() const { return m_connect_count.load(); }
    std::vector<ConnectRecord> connects() const { return locked([&] { return m_connects; }); }
    ConnectRecord last_connect() const { return locked([&] { return m_connects.empty() ? ConnectRecord() : m_connects.back(); }); }
    std::vector<UpgradeRecord> upgrades() const { return locked([&] { return m_upgrades; }); }
    UpgradeRecord last_upgrade() const { return locked([&] { return m_upgrades.empty() ? UpgradeRecord() : m_upgrades.back(); }); }
    std::vector<std::string> tls_errors() const { return locked([&] { return m_tls_errors; }); }
    std::vector<PublishRecord> publishes() const { return locked([&] { return m_publishes; }); }
    // Payloads published to any topic, in the order they arrived.
    std::vector<std::string> received_requests() const
    {
        return locked([&] {
            std::vector<std::string> out;
            for (const PublishRecord& record : m_publishes)
                out.push_back(record.payload);
            return out;
        });
    }
    int  ping_count() const { return m_ping_count.load(); }
    std::vector<std::chrono::steady_clock::time_point> ping_times() const { return locked([&] { return m_ping_times; }); }
    // PUBACKs the client sent for a QoS 1 PUBLISH of ours.
    int  puback_count() const { return m_puback_count.load(); }
    // The packet identifiers of those PUBACKs, in order.
    std::vector<int> puback_ids() const { return locked([&] { return m_puback_ids; }); }
    // The QoS requested for a topic filter, or -1 when it is not subscribed.
    int  subscribed_qos(const std::string& topic) const
    {
        return locked([&] {
            const auto it = m_subscriptions.find(topic);
            return it == m_subscriptions.end() ? -1 : it->second;
        });
    }
    bool is_subscribed(const std::string& topic) const { return subscribed_qos(topic) >= 0; }
    int  suback_count() const { return m_suback_count.load(); }
    int  subscribe_count() const { return m_subscribe_count.load(); }
    int  unsubscribe_count() const { return m_unsubscribe_count.load(); }

    // --- waiting; each returns false on timeout instead of blocking forever
    bool wait_accepts(int count) { return wait([&] { return m_accept_count.load() >= count; }); }
    bool wait_upgrades(std::size_t count) { return wait([&] { return m_upgrades.size() >= count; }); }
    bool wait_connects(int count) { return wait([&] { return static_cast<int>(m_connects.size()) >= count; }); }
    bool wait_subscribed(const std::string& topic) { return wait([&] { return m_subscriptions.count(topic) != 0; }); }
    bool wait_unsubscribed(const std::string& topic) { return wait([&] { return m_subscriptions.count(topic) == 0; }); }
    bool wait_publishes(std::size_t count) { return wait([&] { return m_publishes.size() >= count; }); }
    bool wait_pubacks(std::size_t count) { return wait([&] { return m_puback_ids.size() >= count; }); }
    bool wait_pings(int count) { return wait([&] { return m_ping_count.load() >= count; }); }
    bool wait_subacks(int count) { return wait([&] { return m_suback_count.load() >= count; }); }
    bool wait_subscribes(int count) { return wait([&] { return m_subscribe_count.load() >= count; }); }
    bool wait_tls_errors(std::size_t count) { return wait([&] { return m_tls_errors.size() >= count; }); }
    // Whether a client session is open right now.
    bool wait_client() { return wait([&] { return m_client_open; }); }
    bool wait_no_client() { return wait([&] { return !m_client_open; }); }

private:
    void write_locked(const std::string& bytes)
    {
        if (m_write)
            m_write(bytes);
    }

    void run()
    {
        while (!m_stopping.load()) {
            tcp::socket               socket(m_io);
            boost::system::error_code ec;
            m_acceptor.accept(socket, ec);
            if (m_stopping.load() || ec)
                return;
            ++m_accept_count;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_cv.notify_all();
            }
            if (m_close_on_accept.load())
                continue; // the socket closes as it goes out of scope
            try {
                if (m_tls) {
                    ws::stream<beast::ssl_stream<tcp::socket>> stream(std::move(socket), *m_ssl);
                    if (m_stall == Stall::TlsHandshake) {
                        hold(stream);
                    } else {
                        boost::system::error_code handshake_error;
                        stream.next_layer().handshake(net::ssl::stream_base::server, handshake_error);
                        if (handshake_error) {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            m_tls_errors.push_back(handshake_error.message());
                            m_cv.notify_all();
                        } else {
                            serve(stream);
                        }
                    }
                } else {
                    ws::stream<tcp::socket> stream(std::move(socket));
                    serve(stream);
                }
            } catch (...) {
                // a client dying mid-session must not take the broker down
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_write = nullptr;
            m_close = nullptr;
            m_subscriptions.clear(); // clean session: filters die with the connection
            m_held_subacks.clear();
            m_qos1_pending.clear();
            m_inflight = 0;
            m_client_open = false;
            m_cv.notify_all();
        }
    }

    // Keep the connection open and silent until the client goes away or the broker drops it.
    template <class Stream> void hold(Stream& stream)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_client_open = true;
            m_close       = [&stream] {
                boost::system::error_code close_error;
                beast::get_lowest_layer(stream).shutdown(tcp::socket::shutdown_both, close_error);
            };
            m_cv.notify_all();
        }
        char                      scratch[256];
        boost::system::error_code ec;
        while (!ec && !m_stopping.load())
            beast::get_lowest_layer(stream).read_some(net::buffer(scratch), ec);
    }

    template <class Stream> void serve(Stream& stream)
    {
        beast::flat_buffer                 buffer;
        http::request<http::string_body>   request;
        boost::system::error_code          ec;
        http::read(stream.next_layer(), buffer, request, ec);
        if (ec)
            return;
        UpgradeRecord upgrade;
        for (const auto& field : request) {
            std::string name(field.name_string());
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            upgrade.headers[name] = std::string(field.value());
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_upgrades.push_back(std::move(upgrade));
            m_cv.notify_all();
        }
        if (m_stall == Stall::Upgrade) {
            hold(stream);
            return;
        }
        const std::string subprotocol = m_subprotocol;
        stream.set_option(ws::stream_base::decorator([subprotocol](ws::response_type& response) {
            response.erase("Sec-WebSocket-Protocol");
            if (!subprotocol.empty())
                response.set("Sec-WebSocket-Protocol", subprotocol);
        }));
        stream.accept(request, ec);
        if (ec)
            return;
        stream.binary(true);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_client_open = true;
            m_write       = [this, &stream](const std::string& bytes) {
                boost::system::error_code write_error;
                const std::size_t         fragment = m_fragment.load();
                stream.binary(true);
                stream.auto_fragment(fragment != 0);
                if (fragment != 0)
                    stream.write_buffer_bytes(fragment);
                stream.write(net::buffer(bytes), write_error); // a vanished client is not a test failure
            };
            m_close = [&stream] {
                boost::system::error_code close_error;
                beast::get_lowest_layer(stream).shutdown(tcp::socket::shutdown_both, close_error);
            };
            m_cv.notify_all();
        }

        std::string stream_bytes;
        while (!m_stopping.load()) {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return !m_stop_reading.load() || m_stopping.load(); });
            }
            beast::flat_buffer message;
            stream.read(message, ec);
            if (ec)
                return;
            stream_bytes += beast::buffers_to_string(message.data());
            std::size_t consumed = 0;
            for (;;) {
                const std::size_t total = packet_length(stream_bytes, consumed);
                if (total == 0 || consumed + total > stream_bytes.size())
                    break;
                const bool keep_open = handle_packet(stream_bytes.substr(consumed, total));
                consumed += total;
                if (!keep_open)
                    return;
            }
            stream_bytes.erase(0, consumed);
        }
    }

    // Length of the packet starting at `offset`, or 0 when its header is incomplete.
    static std::size_t packet_length(const std::string& bytes, std::size_t offset)
    {
        std::size_t length = 0;
        std::size_t scale  = 1;
        std::size_t index  = offset + 1;
        for (int i = 0; i < 4; ++i, ++index) {
            if (index >= bytes.size())
                return 0;
            const std::uint8_t byte = static_cast<std::uint8_t>(bytes[index]);
            length += static_cast<std::size_t>(byte & 0x7f) * scale;
            scale *= 128;
            if ((byte & 0x80) == 0)
                return index + 1 - offset + length;
        }
        return 0;
    }

    struct Body {
        std::string data;
        std::size_t pos = 0;
        std::uint8_t u8() { return static_cast<std::uint8_t>(data.at(pos++)); }
        int          u16() { const int hi = u8(); return (hi << 8) | u8(); }
        std::string  str() { const std::size_t n = static_cast<std::size_t>(u16()); std::string s = data.substr(pos, n); pos += n; return s; }
        bool         done() const { return pos >= data.size(); }
    };

    static Body body_of(const std::string& packet)
    {
        std::size_t index = 1;
        while (static_cast<std::uint8_t>(packet[index]) & 0x80)
            ++index;
        return Body{packet.substr(index + 1), 0};
    }

    int next_connack_rc()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_connack_script_index < m_connack_script.size())
            return m_connack_script[m_connack_script_index++];
        return m_connack_rc.load();
    }

    // Returns false when the session must be closed.
    bool handle_packet(const std::string& packet)
    {
        const std::uint8_t header = static_cast<std::uint8_t>(packet[0]);
        Body               body   = body_of(packet);
        switch (header & 0xf0) {
        case 0x10: { // CONNECT
            ConnectRecord record;
            body.str(); // "MQTT"
            record.protocol_level = body.u8();
            const std::uint8_t flags = body.u8();
            record.clean_session = (flags & 0x02) != 0;
            record.has_username  = (flags & 0x80) != 0;
            record.has_password  = (flags & 0x40) != 0;
            record.keepalive     = body.u16();
            record.client_id     = body.str();
            if (flags & 0x04) { body.str(); body.str(); } // will topic and message
            if (record.has_username) record.username = body.str();
            if (record.has_password) record.password = body.str();
            record.at = std::chrono::steady_clock::now();
            const int rc = next_connack_rc();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_connects.push_back(record);
                ++m_connect_count;
                m_cv.notify_all();
            }
            if (rc != 0) {
                write(mqtt_packet(0x20, std::string{0x00, static_cast<char>(rc)}));
                return false;
            }
            if (m_stall != Stall::Connack)
                write(mqtt_packet(0x20, std::string{0x00, 0x00}) + m_connack_trailer);
            return true;
        }
        case 0x80: { // SUBSCRIBE
            const int id = body.u16();
            std::string results;
            // One step, so a waiter that sees the topic also sees the counts.
            std::lock_guard<std::mutex> lock(m_mutex);
            while (!body.done()) {
                const std::string topic = body.str();
                const int         qos   = body.u8();
                if (m_reject_subscriptions.load()) {
                    results.push_back(static_cast<char>(0x80));
                    continue;
                }
                results.push_back(static_cast<char>(std::min(qos, 1)));
                m_subscriptions[topic] = qos;
            }
            const std::string suback = mqtt_packet(0x90, std::string{static_cast<char>(id >> 8), static_cast<char>(id & 0xff)} + results);
            if (m_hold_subacks.load())
                m_held_subacks.push_back(suback);
            else
                write_locked(suback);
            ++m_suback_count;
            ++m_subscribe_count;
            m_cv.notify_all();
            return true;
        }
        case 0xa0: { // UNSUBSCRIBE
            const int id = body.u16();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                while (!body.done())
                    m_subscriptions.erase(body.str());
                ++m_unsubscribe_count;
                m_cv.notify_all();
            }
            write(mqtt_packet(0xb0, std::string{static_cast<char>(id >> 8), static_cast<char>(id & 0xff)}));
            return true;
        }
        case 0x30: { // PUBLISH
            PublishRecord publish;
            publish.qos   = (header >> 1) & 0x03;
            publish.topic = body.str();
            int id = 0;
            if (publish.qos > 0)
                id = body.u16();
            publish.payload = body.data.substr(body.pos);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_publishes.push_back(publish);
                m_cv.notify_all();
            }
            if (publish.qos == 1)
                write(mqtt_packet(0x40, std::string{static_cast<char>(id >> 8), static_cast<char>(id & 0xff)}));
            return true;
        }
        case 0x40: { // PUBACK
            const int id = body.u16();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_puback_ids.push_back(id);
            ++m_puback_count;
            if (m_inflight > 0)
                --m_inflight;
            pump_qos1_locked();
            m_cv.notify_all();
            return true;
        }
        case 0xc0: // PINGREQ
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_ping_times.push_back(std::chrono::steady_clock::now());
                ++m_ping_count;
                m_cv.notify_all();
            }
            if (m_answer_pings.load())
                write(mqtt_packet(0xd0, {}));
            return true;
        case 0xe0: // DISCONNECT
            return false;
        default:
            return true;
        }
    }

    void pump_qos1_locked()
    {
        while (!m_qos1_pending.empty() && (m_inflight_limit == 0 || m_inflight < m_inflight_limit)) {
            write_locked(mqtt_publish_qos1(m_qos1_pending.front().first, ++m_next_qos1_id, m_qos1_pending.front().second));
            m_qos1_pending.pop_front();
            ++m_inflight;
        }
    }

    void write(const std::string& bytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        write_locked(bytes);
    }

    const bool                        m_tls;
    std::atomic<int>                  m_connack_rc;
    std::atomic<Stall>                m_stall;
    const std::string                 m_subprotocol;
    const std::string                 m_connack_trailer;
    const std::string                 m_address;
    std::atomic_bool                  m_stop_reading{false};
    std::atomic_bool                  m_close_on_accept{false};
    std::atomic_bool                  m_hold_subacks{false};
    std::atomic_bool                  m_answer_pings{true};
    std::atomic_bool                  m_reject_subscriptions{false};
    std::atomic<std::size_t>          m_fragment{0};
    TlsIdentity                       m_identity;
    std::optional<net::ssl::context>  m_ssl;
    net::io_context                   m_io;
    tcp::acceptor                     m_acceptor;
    unsigned short                    m_port = 0;
    std::thread                       m_thread;
    std::atomic_bool                  m_stopping{false};
    std::atomic<int>                  m_accept_count{0};
    std::atomic<int>                  m_connect_count{0};
    std::atomic<int>                  m_ping_count{0};
    std::atomic<int>                  m_puback_count{0};
    std::atomic<int>                  m_suback_count{0};
    std::atomic<int>                  m_subscribe_count{0};
    std::atomic<int>                  m_unsubscribe_count{0};
    mutable std::mutex                m_mutex;
    std::condition_variable           m_cv;
    std::function<void(const std::string&)> m_write; // guarded by m_mutex; empty without a client
    std::function<void()>             m_close;       // guarded by m_mutex
    bool                              m_client_open = false;
    std::vector<ConnectRecord>        m_connects;
    std::vector<UpgradeRecord>        m_upgrades;
    std::vector<std::string>          m_tls_errors;
    std::vector<PublishRecord>        m_publishes;
    std::vector<std::chrono::steady_clock::time_point> m_ping_times;
    std::map<std::string, int>        m_subscriptions;
    std::vector<std::string>          m_held_subacks;
    std::vector<int>                  m_puback_ids;
    std::deque<std::pair<std::string, std::string>> m_qos1_pending;
    std::size_t                       m_inflight_limit = 0;
    std::size_t                       m_inflight = 0;
    std::uint16_t                     m_next_qos1_id = 0;
    std::vector<int>                  m_connack_script;
    std::size_t                       m_connack_script_index = 0;
};

} // namespace orca_mqtt_test
