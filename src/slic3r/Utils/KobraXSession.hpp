#ifndef slic3r_KobraXSession_hpp_
#define slic3r_KobraXSession_hpp_

#include "KobraX.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace KobraX {

// The Anycubic Slicer client identity the printer's MQTT broker requires (CN=AnycubicSlicer), as PEM text.
struct ClientIdentity
{
    std::string cert_pem;
    std::string key_pem;
};

// <data dir>/anycubic, where the user may place the identity
std::string identity_folder();
// Looks for the identity as anycubic_slicer.pem and anycubic_slicer_key.pem in `user_folder`, then inside an
// installed Anycubic Slicer Next, then as the same two files in `bundled_folder` (a copy the caller provides, or
// empty for none), so a user copy always wins.
// Nothing is copied or written. False, with what was searched, when none is found.
bool find_client_identity(const std::string& user_folder,
                          const std::string& bundled_folder,
                          ClientIdentity&    identity,
                          std::string&       error);
// Scans one file (a PEM file or the slicer library carrying the PEM blocks) for the certificate and its key
bool read_client_identity(const std::string& path, ClientIdentity& identity);

// One MQTT session with one printer: credentials over HTTP, mutual TLS, reports to a callback. The callbacks
// run on the session's own thread.
class Session
{
public:
    struct Options
    {
        std::string    host;
        int            http_port = HTTP_PORT;
        int            mqtt_port = MQTT_PORT;
        ClientIdentity identity;
    };
    using ReportFn = std::function<void(const std::string& suffix, const nlohmann::json& payload)>;
    // Called once when an open session ends, with the reason
    using ClosedFn = std::function<void(const std::string& reason)>;
    using ProgressFn = std::function<void(int percent)>;
    using CancelFn   = std::function<bool()>;
    // A report that settles a wait: true when it does
    using MatchFn = std::function<bool(const std::string& suffix, const nlohmann::json& payload)>;

    Session(Options options, ReportFn on_report, ClosedFn on_closed);
    ~Session();
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    // Blocking: recovers the MQTT login from the printer, logs in and subscribes. Once open, reports arrive
    // until close() or a dropped connection.
    bool open(std::string& error);
    void close();
    bool is_open() const;

    // Publishes a request on the "slicer" or "web" topic (publish_topic). False when not open.
    bool send(const std::string& type, const std::string& action, const nlohmann::json& data = nlohmann::json(), const std::string& kind = "slicer");
    // Publishes a request and waits for the first report `match` accepts. False on timeout or a dropped session.
    bool request(const std::string&        type,
                 const std::string&        action,
                 const nlohmann::json&     data,
                 const MatchFn&            match,
                 std::chrono::milliseconds timeout,
                 nlohmann::json&           reply);

    // Waits for the first report `match` accepts, without publishing anything. False on timeout or a dropped session.
    bool wait(const MatchFn& match, std::chrono::milliseconds timeout, nlohmann::json& reply);

    // Streams a file to the printer's storage under `name`; upload_url is the info report's fileUploadurl.
    bool upload(const std::string& path, const std::string& name, const std::string& upload_url, const ProgressFn& progress,
                const CancelFn& cancelled, std::string& error);

    const Credentials& credentials() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p;
};

}} // namespace Slic3r::KobraX

#endif
