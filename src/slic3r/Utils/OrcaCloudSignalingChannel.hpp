#pragma once

#include "ICameraSignalingChannel.hpp"
#include "ICloudServiceAgent.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r {

class OrcaCloudSignalingChannel : public ICameraSignalingChannel {
public:
    OrcaCloudSignalingChannel(std::shared_ptr<ICloudServiceAgent> cloud, std::string dev_id);
    ~OrcaCloudSignalingChannel() override;

    void open() override;
    void close() override;
    void send_offer(std::string sdp) override;
    void send_ice(std::string candidate, std::string mid) override;

private:
    using WebSocket = boost::beast::websocket::stream<
        boost::beast::ssl_stream<boost::asio::ip::tcp::socket>>;

    // The io_context and ssl_context must outlive the websocket stream that
    // references them. Bundling them here with the stream declared last makes
    // the destruction order correct (stream first, then contexts), and lets a
    // single shared_ptr own the whole set.
    struct Connection {
        boost::asio::io_context io_context;
        boost::asio::ssl::context ssl_context{boost::asio::ssl::context::tls_client};
        WebSocket websocket{io_context, ssl_context};
    };

    void run();
    void do_read(std::shared_ptr<Connection> conn);
    void dispatch_message(const nlohmann::json& message, const std::string& raw);
    void send_json(const std::string& message);
    void unavailable(CameraUnavailableReason reason, std::string detail);
    static std::string encode_path_component(const std::string& value);
    static std::string host_without_scheme(std::string value);

    std::shared_ptr<ICloudServiceAgent> m_cloud;
    std::string m_dev_id;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_open{false};
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::shared_ptr<Connection> m_conn;
};

} // namespace Slic3r
