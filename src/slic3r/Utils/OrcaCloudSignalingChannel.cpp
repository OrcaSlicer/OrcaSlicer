#include "OrcaCloudSignalingChannel.hpp"

#include "Http.hpp"
#include "OrcaCloudServiceAgent.hpp"
#include "ICameraSignalingChannel.hpp"
#include "PrinterCommLogger.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/verify_mode.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/log/trivial.hpp>
#include <memory>
#include <mutex>
#include <ios>
#include <exception>
#include <cstddef>
#include <nlohmann/json.hpp>

#include <openssl/ssl.h>

#include <cctype>
#include <chrono>
#include <iomanip>
#include <openssl/tls1.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Slic3r {

// The Cloud closes a camera viewer socket that sends nothing for 180 s, and the
// signalling socket is otherwise silent once ICE has finished, so the channel
// sends the OPCP keepalive at the browsers' cadence. The Cloud matches the
// text byte for byte.
static constexpr std::chrono::seconds KEEPALIVE_INTERVAL{30};
static constexpr std::string_view KEEPALIVE_MESSAGE = R"({"type":"ping"})";

OrcaCloudSignalingChannel::OrcaCloudSignalingChannel(OrcaCloudServiceAgent* cloud, std::string dev_id)
    : m_cloud(cloud)
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
    Http::Ptr request;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        conn = m_conn;
        request = m_inflight_requests;
    }
    // m_conn is installed only after the live-token request completes, so
    // cancel the request independently before waiting for the worker.
    if (request)
        request->cancel();
    if (conn) {
        // Established session: stop the keepalive and close the socket on the
        // io_context's own thread so the pending waits complete and
        // io_context.run() unwinds.
        boost::asio::post(conn->io_context, [conn] {
            conn->keepalive.cancel();
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
    // OPCP takes a viewer's candidate only as an RTCIceCandidateInit object, the
    // form browsers produce with RTCIceCandidate.toJSON(); the Cloud forwards it
    // to the printer unchanged.
    send_json(nlohmann::json{{"type", "webrtc.ice"},
                             {"candidate", {{"candidate", std::move(candidate)}, {"sdpMid", std::move(mid)}}}}
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

void OrcaCloudSignalingChannel::unavailable(CameraUnavailableReason reason, std::string detail)
{
#ifdef ORCA_PRINTER_COMM_LOG
    const char* reason_name = "error";
    switch (reason) {
    case CameraUnavailableReason::Busy: reason_name = "busy"; break;
    case CameraUnavailableReason::Error: reason_name = "error"; break;
    case CameraUnavailableReason::Disabled: reason_name = "disabled"; break;
    case CameraUnavailableReason::Closed: reason_name = "closed"; break;
    }
    PrinterCommLogger::instance().log_event("video", "signaling_unavailable",
                                            {{"dev_id", m_dev_id}, {"reason", reason_name}, {"detail", detail}});
#endif
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
        // WebRTC signaling uses fixed HTTPS/WSS endpoints on port 443;
        // custom ports and base paths are not supported by this agent.
        const std::string host = m_cloud->get_cloud_service_host();
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
        unsigned int http_code = 0;

        auto request = std::make_shared<Slic3r::Http>(Http::post(live_token_url));
        request->set_post_body(std::string("{}"))
            .header("Authorization", "Bearer " + token)
            .header("Content-Type", "application/json")
            .tls_verify(true)
            .timeout_max(30)
            .on_complete([&token_body, &http_code](std::string body, unsigned status) {
                http_code = status;
                token_body = std::move(body);
            })
            .on_error([&http_code](std::string, std::string, unsigned status) {
                http_code = status;
            });

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_inflight_requests = request;
        }

        // If close() raced with request setup, make sure this request is
        // cancelled before entering the blocking call.
        if (m_stop.load())
            request->cancel();
        try {
            request->perform_sync();
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_inflight_requests == request)
                m_inflight_requests.reset();
            throw;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_inflight_requests == request)
                m_inflight_requests.reset();
        }

        try {
            token_response = nlohmann::json::parse(token_body);
        } catch (const std::exception&) {
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
        // The Cloud omits ice_servers when it mints no TURN credentials. A peer
        // without a STUN server offers only its LAN addresses, which a printer on
        // another network cannot reach, so fall back to the public STUN servers
        // the OrcaCloud web client uses in that case.
        if (ice_servers.empty())
            ice_servers = {{"stun:stun.cloudflare.com:3478", {}, {}}, {"stun:stun.l.google.com:19302", {}, {}}};
        auto conn = std::make_shared<Connection>();
        conn->ssl_context.set_default_verify_paths();
        Http::add_platform_root_certificates(conn->ssl_context.native_handle());
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
        websocket.next_layer().set_verify_callback(boost::asio::ssl::host_name_verification(host));
        websocket.next_layer().handshake(boost::asio::ssl::stream_base::client);
        const std::string ws_target = "/api/v1/printers/" + encode_path_component(m_dev_id) +
                                      "/camera/live?token=" +
                                      encode_path_component(token_response["token"].get<std::string>());
        websocket.handshake(host, ws_target);
        BOOST_LOG_TRIVIAL(info) << "signaling: websocket handshake ok (" << ice_servers.size()
                                << " ice servers)";
        PRINTER_COMM_LOG(PrinterCommLogger::instance().log_event("video", "signaling_open",
                                                                 {{"dev_id", m_dev_id}, {"ice_servers", ice_servers.size()}}));

        if (on_ready)
            on_ready(std::move(ice_servers));
        // No camera.mode is sent. For a viewer that never declares "webrtc",
        // the Cloud closes its peer on the printer when the socket closes; this
        // client ends its peer with the socket and never reattaches to a
        // running one, so that is the behaviour it needs.
        //
        // Async read loop and keepalive, driven by the connection's own
        // io_context. run() returns once close() has shut both down, giving a
        // bounded, deadlock-free teardown from any thread.
        schedule_keepalive(conn);
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
                conn->keepalive.cancel();
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

void OrcaCloudSignalingChannel::schedule_keepalive(std::shared_ptr<Connection> conn)
{
    conn->keepalive.expires_after(KEEPALIVE_INTERVAL);
    conn->keepalive.async_wait([this, conn](boost::system::error_code ec) {
        if (ec || m_stop.load())
            return; // cancelled by close() or a failed read; io_context.run() unwinds
        // The handler runs on the io_context thread, like send_json's writes.
        boost::system::error_code write_ec;
        conn->websocket.write(boost::asio::buffer(KEEPALIVE_MESSAGE.data(), KEEPALIVE_MESSAGE.size()), write_ec);
        if (write_ec) {
            if (!m_stop.load())
                unavailable(CameraUnavailableReason::Closed, write_ec.message());
            return;
        }
        schedule_keepalive(conn);
    });
}

// Returns the string at key, or "" if absent or not a string (JSON null included).
static std::string json_string(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

#ifdef ORCA_PRINTER_COMM_LOG
// A signaling message as recorded in the capture. An SDP body is reduced to its size and an ICE
// candidate to its type (host, srflx, relay): they carry the session's ICE password and the
// host's addresses. Every other field is protocol and is kept.
static nlohmann::json signaling_record(const std::string& dev_id, const nlohmann::json& message)
{
    nlohmann::json body = message.is_object() ? message : nlohmann::json::object();
    const auto     sdp  = body.find("sdp");
    if (sdp != body.end() && sdp->is_string())
        *sdp = std::to_string(sdp->get<std::string>().size()) + " bytes";

    const auto summarize = [](nlohmann::json& candidate) {
        if (!candidate.is_string())
            return;
        const std::string text = candidate.get<std::string>();
        const size_t      typ  = text.find(" typ ");
        if (typ == std::string::npos) {
            candidate = "<candidate>";
            return;
        }
        const size_t start = typ + 5;
        const size_t end   = text.find(' ', start);
        candidate          = "typ " + text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };
    const auto candidate = body.find("candidate");
    if (candidate != body.end()) {
        const auto inner = candidate->is_object() ? candidate->find("candidate") : candidate->end();
        summarize(inner != candidate->end() ? *inner : *candidate);
    }
    return {{"dev_id", dev_id}, {"message", std::move(body)}};
}
#endif

void OrcaCloudSignalingChannel::dispatch_message(const nlohmann::json& message, const std::string& raw)
{
    const std::string type = json_string(message, "type");
    if (type == "pong")
        return; // the keepalive's reply, every 30 s; nothing to act on or log
    BOOST_LOG_TRIVIAL(info) << "signaling: recv type=" << type << " raw=" << raw.substr(0, 256);
    PRINTER_COMM_LOG(PrinterCommLogger::instance().log_event("video", "signaling_recv", signaling_record(m_dev_id, message)));
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
    PRINTER_COMM_LOG(PrinterCommLogger::instance().log_event("video", "signaling_send",
                                                             signaling_record(m_dev_id, nlohmann::json::parse(message, nullptr, false))));
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
