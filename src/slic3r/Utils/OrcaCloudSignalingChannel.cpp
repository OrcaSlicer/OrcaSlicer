#include "OrcaCloudSignalingChannel.hpp"

#include "Http.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <openssl/ssl.h>

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace Slic3r {

OrcaCloudSignalingChannel::OrcaCloudSignalingChannel(std::shared_ptr<ICloudServiceAgent> cloud, std::string dev_id)
    : m_cloud(std::move(cloud))
    , m_dev_id(std::move(dev_id))
{
}

OrcaCloudSignalingChannel::~OrcaCloudSignalingChannel()
{
    close();
}

void OrcaCloudSignalingChannel::open()
{
    bool expected = false;
    if (!m_open.compare_exchange_strong(expected, true))
        return;
    m_stop.store(false);
    m_thread = std::thread([this] { run(); });
}

void OrcaCloudSignalingChannel::close()
{
    m_stop.store(true);
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        conn = m_conn;
    }
    if (conn) {
        // Established session: close the socket on the io_context's own thread so
        // the pending async_read completes and io_context.run() unwinds.
        boost::asio::post(conn->io_context, [conn] {
            boost::system::error_code ec;
            boost::beast::get_lowest_layer(conn->websocket).cancel(ec);
            boost::beast::get_lowest_layer(conn->websocket).close(ec);
        });
        // Pre-run() phase (still in the synchronous connect/handshake): best-effort
        // direct interruption.
        boost::system::error_code ec;
        boost::beast::get_lowest_layer(conn->websocket).cancel(ec);
        boost::beast::get_lowest_layer(conn->websocket).close(ec);
    }
    if (m_thread.joinable())
        m_thread.join();
    m_open.store(false);
}

void OrcaCloudSignalingChannel::send_offer(std::string sdp)
{
    send_json(nlohmann::json{{"type", "webrtc.offer"}, {"sdp", std::move(sdp)}}.dump());
}

void OrcaCloudSignalingChannel::send_ice(std::string candidate, std::string mid)
{
    send_json(nlohmann::json{{"type", "webrtc.ice"},
                             {"candidate", std::move(candidate)},
                             {"sdpMid", std::move(mid)}}
                  .dump());
}

std::string OrcaCloudSignalingChannel::encode_path_component(const std::string& value)
{
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            encoded << c;
        else
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(c);
    }
    return encoded.str();
}

std::string OrcaCloudSignalingChannel::host_without_scheme(std::string value)
{
    const auto scheme = value.find("://");
    if (scheme != std::string::npos)
        value.erase(0, scheme + 3);
    const auto slash = value.find('/');
    if (slash != std::string::npos)
        value.erase(slash);
    return value;
}

void OrcaCloudSignalingChannel::unavailable(CameraUnavailableReason reason, std::string detail)
{
    if (on_unavailable)
        on_unavailable(reason, std::move(detail));
}

void OrcaCloudSignalingChannel::run()
{
    try {
        if (!m_cloud || !m_cloud->ensure_token_fresh("camera")) {
            unavailable(CameraUnavailableReason::Error, "Unable to refresh OrcaCloud credentials");
            m_open.store(false);
            return;
        }
        const std::string token = m_cloud->get_access_token();
        const std::string host = host_without_scheme(m_cloud->get_cloud_service_host());
        if (token.empty() || host.empty()) {
            unavailable(CameraUnavailableReason::Error, "OrcaCloud session is unavailable");
            m_open.store(false);
            return;
        }

        const std::string live_token_url =
            "https://" + host + "/api/v1/printers/" + encode_path_component(m_dev_id) + "/live-token";
        BOOST_LOG_TRIVIAL(info) << "signaling: POST " << live_token_url << " (dev_id=" << m_dev_id << ")";

        nlohmann::json token_response;
        std::string token_body;
        std::string token_error;
        unsigned int http_code = 0;
        auto request = Http::post(live_token_url);
        request.set_post_body(std::string("{}"))
            .header("Authorization", "Bearer " + token)
            .header("Content-Type", "application/json")
            .tls_verify(true)
            .timeout_max(30)
            .on_complete([&token_body, &http_code](std::string body, unsigned status) {
                http_code = status;
                token_body = std::move(body);
            })
            .on_error([&token_body, &token_error, &http_code](std::string body, std::string error, unsigned status) {
                http_code = status;
                token_body = std::move(body);
                token_error = std::move(error);
            })
            .perform_sync();
        BOOST_LOG_TRIVIAL(info) << "signaling: live-token HTTP " << http_code
                                << (token_error.empty() ? "" : " error=" + token_error)
                                << " body=" << token_body.substr(0, 512);
        try {
            token_response = nlohmann::json::parse(token_body);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "signaling: live-token body is not JSON: " << e.what();
        }
        if (http_code < 200 || http_code >= 300 || !token_response.contains("token")) {
            unavailable(CameraUnavailableReason::Error, "Unable to mint camera live token");
            m_open.store(false);
            return;
        }

        std::vector<CameraIceServer> ice_servers;
        if (token_response.contains("ice_servers") && token_response["ice_servers"].is_array()) {
            for (const auto& entry : token_response["ice_servers"]) {
                if (entry.is_string()) {
                    ice_servers.push_back({entry.get<std::string>(), {}, {}});
                } else if (entry.is_object()) {
                    // RTCIceServer.urls is "string | string[]" (Cloudflare
                    // Realtime returns an array). Emit one CameraIceServer per
                    // URL, sharing the credentials.
                    const std::string username = entry.value("username", std::string{});
                    const std::string credential = entry.value("credential", std::string{});
                    const auto add_url = [&](const nlohmann::json& url) {
                        if (url.is_string() && !url.get<std::string>().empty())
                            ice_servers.push_back({url.get<std::string>(), username, credential});
                    };
                    const auto urls = entry.find("urls");
                    if (urls != entry.end()) {
                        if (urls->is_array()) {
                            for (const auto& url : *urls)
                                add_url(url);
                        } else {
                            add_url(*urls);
                        }
                    }
                }
            }
        }

        auto conn = std::make_shared<Connection>();
        conn->ssl_context.set_default_verify_paths();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_conn = conn;
        }
        auto& websocket = conn->websocket;
        boost::asio::ip::tcp::resolver resolver(conn->io_context);
        const auto endpoints = resolver.resolve(host, "443");
        boost::asio::connect(boost::beast::get_lowest_layer(websocket), endpoints);
        if (!SSL_set_tlsext_host_name(websocket.next_layer().native_handle(), host.c_str()))
            throw std::runtime_error("Unable to configure TLS server name");
        websocket.next_layer().set_verify_mode(boost::asio::ssl::verify_peer);
        websocket.next_layer().handshake(boost::asio::ssl::stream_base::client);
        const std::string ws_target = "/api/v1/printers/" + encode_path_component(m_dev_id) +
                                      "/camera/live?token=" +
                                      encode_path_component(token_response["token"].get<std::string>());
        websocket.handshake(host, ws_target);
        BOOST_LOG_TRIVIAL(info) << "signaling: websocket handshake ok (" << ice_servers.size()
                                << " ice servers)";

        if (on_ready)
            on_ready(std::move(ice_servers));
        send_json(nlohmann::json{{"type", "camera.mode"}, {"mode", "webrtc"}}.dump());
        // Async read loop, driven by the connection's own io_context. run()
        // returns once close() has shut the socket down, giving a bounded,
        // deadlock-free teardown from any thread.
        do_read(conn);
        conn->io_context.run();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "signaling: run() exception: " << e.what();
        if (!m_stop.load())
            unavailable(CameraUnavailableReason::Closed, e.what());
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_conn.reset();
    }
    m_open.store(false);
}

void OrcaCloudSignalingChannel::do_read(std::shared_ptr<Connection> conn)
{
    auto buffer = std::make_shared<boost::beast::flat_buffer>();
    conn->websocket.async_read(
        *buffer, [this, conn, buffer](boost::system::error_code ec, std::size_t) {
            if (ec) {
                if (!m_stop.load())
                    unavailable(CameraUnavailableReason::Closed, ec.message());
                return; // do not re-arm; io_context.run() unwinds
            }
            const std::string raw = boost::beast::buffers_to_string(buffer->data());
            // A malformed or unexpectedly-shaped message must not tear down the
            // session: parse/dispatch is guarded.
            try {
                dispatch_message(nlohmann::json::parse(raw), raw);
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << "signaling: ignoring malformed message: " << e.what()
                                           << " raw=" << raw.substr(0, 256);
            }
            if (!m_stop.load())
                do_read(conn);
        });
}

// Returns the string at key, or "" if absent or not a string (JSON null included).
static std::string json_string(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

void OrcaCloudSignalingChannel::dispatch_message(const nlohmann::json& message, const std::string& raw)
{
    const std::string type = json_string(message, "type");
    BOOST_LOG_TRIVIAL(info) << "signaling: recv type=" << type << " raw=" << raw.substr(0, 256);
    if (type == "webrtc.answer") {
        const std::string sdp = json_string(message, "sdp");
        if (on_answer && !sdp.empty())
            on_answer(sdp);
    } else if (type == "webrtc.ice" && message.contains("candidate")) {
        // The peer may send "candidate" as a flat string or as a nested
        // RTCIceCandidateInit object { candidate, sdpMid, sdpMLineIndex }.
        const nlohmann::json& candidate = message["candidate"];
        std::string sdp_candidate;
        std::string mid = json_string(message, "sdpMid");
        if (candidate.is_string()) {
            sdp_candidate = candidate.get<std::string>();
        } else if (candidate.is_object()) {
            sdp_candidate = json_string(candidate, "candidate");
            std::string nested_mid = json_string(candidate, "sdpMid");
            if (!nested_mid.empty())
                mid = std::move(nested_mid);
        }
        if (on_ice && !sdp_candidate.empty())
            on_ice(sdp_candidate, mid);
    } else if (type == "webrtc.unavailable") {
        const std::string reason = json_string(message, "reason");
        unavailable(reason == "busy" ? CameraUnavailableReason::Busy
                                     : reason == "disabled" ? CameraUnavailableReason::Disabled
                                                            : CameraUnavailableReason::Error,
                    reason.empty() ? "error" : reason);
    }
}

void OrcaCloudSignalingChannel::send_json(const std::string& message)
{
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        conn = m_conn;
    }
    if (!conn || m_stop.load())
        return;
    // Serialize the write onto the io_context thread (same thread that runs
    // async_read), so reads and writes never touch the stream concurrently.
    auto payload = std::make_shared<std::string>(message);
    boost::asio::post(conn->io_context, [this, conn, payload] {
        if (m_stop.load())
            return;
        boost::system::error_code ec;
        conn->websocket.write(boost::asio::buffer(*payload), ec);
        if (ec && !m_stop.load())
            unavailable(CameraUnavailableReason::Closed, ec.message());
    });
}

} // namespace Slic3r
