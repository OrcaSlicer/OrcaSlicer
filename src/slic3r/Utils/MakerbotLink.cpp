// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// MakerbotLink.cpp
//
// Birdwing (Z18, Replicator+, Mini+, 5th Gen):
//   RAW TCP SSL on port 12309 (Boost.Asio) – NOT libcurl HTTP!
//   Protocol: JSON-RPC 2.0, newline-delimited, self-signed cert accepted.
//
// Lava/Method (Method, Method X, Method XL):
//   Plain HTTP JSON-RPC on port 2222 via libcurl (Http class).

#include "MakerbotLink.hpp"
#include "Http.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <nlohmann/json.hpp>
#include <wx/string.h>

#include <random>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>

namespace asio = boost::asio;
namespace ssl  = boost::asio::ssl;
using tcp      = boost::asio::ip::tcp;

namespace Slic3r {

// ── Birdwing Raw SSL RPC Client ───────────────────────────────────────────────
// Sends one JSON-RPC request and reads one JSON-RPC response over raw SSL TCP.
// No HTTP involved – the Z18 speaks newline-terminated JSON directly over TLS.

class BirdwingRpcClient
{
public:
    BirdwingRpcClient(const std::string& host, int port)
        : m_host(host), m_port(port)
        , m_ssl_ctx(ssl::context::tls_client)
        , m_socket(m_io, m_ssl_ctx)
    {
        // Accept self-signed certificates (MakerBot uses vendor-internal CA)
        m_ssl_ctx.set_verify_mode(ssl::verify_none);
    }

    bool connect(std::string& error) {
        try {
            tcp::resolver resolver(m_io);
            auto eps = resolver.resolve(m_host, std::to_string(m_port));
            asio::connect(m_socket.lowest_layer(), eps);
            m_socket.lowest_layer().set_option(tcp::no_delay(true));
            m_socket.handshake(ssl::stream_base::client);
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    // Send one JSON-RPC request (adds \r\n), read one JSON response.
    // Send multiple RPCs on the same connection
    bool call_persistent(const nlohmann::json& request,
                          nlohmann::json&       response,
                          std::string&          error)
    {
        return call(request, response, error, 130); // long timeout for authorize
    }

    bool call(const nlohmann::json& request,
              nlohmann::json&       response,
              std::string&          error,
              int                   timeout_s = 10)
    {
        try {
            const std::string msg = request.dump() + "\r\n";
            asio::write(m_socket, asio::buffer(msg));

            // Set timeout via a deadline on the io_context
            asio::streambuf buf;
            boost::system::error_code ec;
            asio::read_until(m_socket, buf, '\n', ec);
            if (ec && ec != asio::error::eof) {
                error = ec.message();
                return false;
            }

            std::istream is(&buf);
            std::string line;
            std::getline(is, line);
            boost::algorithm::trim(line);
            if (line.empty()) {
                error = "Empty response from MakerBot";
                return false;
            }

            response = nlohmann::json::parse(line);
            if (response.contains("error")) {
                error = response["error"].value("message", "RPC error");
                return false;
            }
            return true;

        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    void close() {
        boost::system::error_code ec;
        m_socket.lowest_layer().close(ec);
    }

private:
    std::string       m_host;
    int               m_port;
    asio::io_context  m_io;
    ssl::context      m_ssl_ctx;
    ssl::stream<tcp::socket> m_socket;
};


// ── KaitenSession: persistent plaintext RPC on port 9999 ─────────────────────
// Confirmed via packet capture (Z18, MakerBot Desktop 4.10.1, 2026-06): the
// real command/telemetry channel is plain TCP on port 9999, newline-
// delimited JSON-RPC 2.0, no TLS at all. handshake -> authenticate(token) ->
// repeated calls, all on the SAME connection - unlike BirdwingRpcClient
// above (SSL/12309), which reconnects per call.

struct KaitenSession::Impl
{
    std::string      host;
    asio::io_context io;
    tcp::socket      socket{io};
    bool             connected = false;
    int              next_id   = 2; // 0/1 are used up by handshake+authenticate in open()
};

KaitenSession::KaitenSession() : m_impl(std::make_unique<Impl>()) {}
KaitenSession::~KaitenSession() { close(); }

bool KaitenSession::is_open() const { return m_impl && m_impl->connected; }

void KaitenSession::close()
{
    if (!m_impl || !m_impl->connected) return;
    boost::system::error_code ec;
    m_impl->socket.close(ec);
    m_impl->connected = false;
}

bool KaitenSession::call(const std::string& method, const nlohmann::json& params,
                          nlohmann::json& out, std::string& error, int timeout_s)
{
    if (!is_open()) { error = "KaitenSession is not connected."; return false; }

    const nlohmann::json request = {
        {"jsonrpc", "2.0"}, {"method", method}, {"params", params}, {"id", m_impl->next_id++}
    };

    try {
        const std::string msg = request.dump() + "\r\n";
        asio::write(m_impl->socket, asio::buffer(msg));

        asio::streambuf buf;
        boost::system::error_code ec;
        m_impl->socket.non_blocking(false);
        asio::read_until(m_impl->socket, buf, '\n', ec);
        if (ec) { error = ec.message(); close(); return false; }

        std::istream is(&buf);
        std::string line;
        std::getline(is, line);
        boost::algorithm::trim(line);
        if (line.empty()) { error = "Empty response from MakerBot (port 9999)"; return false; }

        out = nlohmann::json::parse(line);
        if (out.contains("error")) {
            error = out["error"].value("message", "RPC error");
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        close();
        return false;
    }
}

bool KaitenSession::open(const std::string& host, const std::string& access_token, std::string& error)
{
    close();
    m_impl = std::make_unique<Impl>();
    m_impl->host = host;

    try {
        tcp::resolver resolver(m_impl->io);
        auto eps = resolver.resolve(host, std::to_string(MakerbotLink::KAITEN_PLAINTEXT_PORT));
        asio::connect(m_impl->socket, eps);
        m_impl->socket.set_option(tcp::no_delay(true));
        m_impl->connected = true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }

    nlohmann::json resp;
    if (!call("handshake", nlohmann::json::object(), resp, error, 10)) {
        close();
        return false;
    }

    if (access_token.empty()) {
        // Re-Authentifizierung ohne vorherige Erstauthentifizierung (SSL/
        // 12309) ergibt keinen Sinn in diesem Protokoll - klar scheitern
        // lassen statt eine vermutlich vom Z18 ohnehin abgelehnte,
        // unauthentifizierte Session als "offen" zu melden.
        error = "No access_token available - printer must be paired (Erstauthentifizierung) first.";
        close();
        return false;
    }

    const nlohmann::json auth_params = {{"access_token", access_token}};
    if (!call("authenticate", auth_params, resp, error, 10)) {
        close();
        return false;
    }

    return true;
}


// ── Konstruktor ───────────────────────────────────────────────────────────────

MakerbotLink::MakerbotLink(DynamicPrintConfig* config)
{
    if (const auto* opt = config->opt<ConfigOptionString>("print_host"))
        m_host = opt->value;

    std::string stored_auth;
    if (const auto* opt = config->opt<ConfigOptionString>("printhost_password"))
        stored_auth = opt->value;

    const size_t colon = stored_auth.find(':');
    if (colon != std::string::npos) {
        m_client_id    = stored_auth.substr(0, colon);
        m_access_token = stored_auth.substr(colon + 1);
    }

    for (const auto& prefix : { "https://", "http://" })
        if (m_host.rfind(prefix, 0) == 0)
            m_host.erase(0, std::string(prefix).size());

    const size_t port_colon = m_host.find(':');
    if (port_colon != std::string::npos) {
        try { m_port = std::stoi(m_host.substr(port_colon + 1)); }
        catch (...) {}
        m_host = m_host.substr(0, port_colon);
    }

    // Flavor detection
    if (const auto* gcf = config->opt<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor"))
        m_is_birdwing = (gcf->value == gcfMakerBotBirdwing);

    // Default: Birdwing SSL on port 12309
    // Override: if user entered :2222 → Lava/Method HTTP
    if (m_port == LAVA_PORT && !m_is_birdwing) {
        // No explicit port → assume Birdwing
        m_port        = SSL_PORT_BIRDWING;
        m_is_birdwing = true;
    }
    if (m_port == SSL_PORT_BIRDWING) m_is_birdwing = true;
    if (m_port == LAVA_PORT)         m_is_birdwing = false;

    BOOST_LOG_TRIVIAL(debug) << "MakerbotLink: host=" << m_host
                             << " port=" << m_port
                             << " birdwing=" << m_is_birdwing;
}


// ── Birdwing: Raw SSL RPC ─────────────────────────────────────────────────────

bool MakerbotLink::birdwing_rpc(const std::string&    method,
                                 const nlohmann::json& params,
                                 nlohmann::json&       out,
                                 std::string&          error,
                                 int                   timeout_s) const
{
    if (m_host.empty()) { error = "No IP address configured."; return false; }

    BirdwingRpcClient client(m_host, m_port);
    if (!client.connect(error))
        return false;

    const nlohmann::json request = {
        {"jsonrpc", "2.0"},
        {"method",  method},
        {"params",  params},
        {"id",      1}
    };

    const bool ok = client.call(request, out, error, timeout_s);
    client.close();

    if (ok) BOOST_LOG_TRIVIAL(debug) << "MakerbotLink Birdwing RPC " << method << " OK";
    return ok;
}


// ── Plaintext kaiten session (port 9999) ─────────────────────────────────────

std::shared_ptr<KaitenSession> MakerbotLink::open_kaiten_session(std::string& error) const
{
    if (m_host.empty()) { error = "No IP address configured."; return nullptr; }

    // Re-Authentifizierung über Port 9999 setzt eine erfolgte Erstauthenti-
    // fizierung über SSL/12309 voraus (siehe birdwing_authorize() /
    // BirdwingHandshakeDialog). Ohne gespeicherten Token gar nicht erst
    // versuchen - das spart eine verwirrende RPC-Fehlermeldung weiter unten.
    if (m_access_token.empty()) {
        error = "Printer not paired yet. Pair it first in Printer Settings "
                "(confirm the one-time handshake on the printer's handwheel), "
                "then reopen the Device tab.";
        return nullptr;
    }

    auto session = std::make_shared<KaitenSession>();
    if (!session->open(m_host, m_access_token, error))
        return nullptr;

    BOOST_LOG_TRIVIAL(info) << "MakerbotLink: opened plaintext kaiten session on "
        << m_host << ":" << KAITEN_PLAINTEXT_PORT;
    return session;
}


// ── Smart Extruder Detection ─────────────────────────────────────────────────
// Query the printer's kaiten RPC to detect which Smart Extruder is attached.
// Called after successful handshake.
// Maps kaiten "type_name" to our smart_extruder_type strings.
std::string MakerbotLink::get_toolhead_type(std::string& error) const
{
    nlohmann::json resp;
    // Use a short timeout – this is a quick info query
    if (!birdwing_rpc("get_system_information", nlohmann::json::object(), resp, error, 10))
        return "";

    // Response: {"result": {"toolheads": [{"type_name": "mk13", ...}], ...}}
    try {
        const auto& result = resp["result"];
        if (result.contains("toolheads") && !result["toolheads"].empty()) {
            const auto& th = result["toolheads"][0];
            if (th.contains("type_name")) {
                const std::string type_name = th["type_name"].get<std::string>();
                BOOST_LOG_TRIVIAL(info)
                    << "MakerbotLink: detected Smart Extruder type: " << type_name;
                // Normalize to our known types
                if (type_name == "mk13_impla")       return "mk13_impla";
                if (type_name == "mk13_experimental")return "mk13_experimental";
                if (type_name == "mk12")             return "mk12";
                if (type_name.rfind("mk13", 0) == 0) return "mk13"; // mk13, mk13_plus, etc.
                return type_name; // pass through unknown types
            }
        }
        // Older firmware: check "machine_info" or similar
        if (result.contains("machine_info")) {
            const auto& mi = result["machine_info"];
            if (mi.contains("toolhead_model"))
                return mi["toolhead_model"].get<std::string>();
        }
    } catch (const std::exception& e) {
        error = std::string("toolhead parse error: ") + e.what();
    }
    return "mk13"; // safe default for Birdwing printers
}

// ── Birdwing Auth Flow ────────────────────────────────────────────────────────

MakerbotLink::BirdwingAuthResult
MakerbotLink::birdwing_authorize(std::string& error_or_token,
                                  int          timeout_s) const
{
    // COMPLETE REWRITE (2026-06-18): Port 12309 and the old "authorize" RPC
    // do NOT exist in MakerBot's actual conveyor-3.10.1 source code.
    // The real protocol (confirmed from birdwing.py in the conveyor Python egg):
    //
    //   Step 1: HTTPS GET https://<printer>:443/auth?response_type=code
    //              &client_id=MakerWare&client_secret=<random>
    //              &username=OrcaSlicer&thingiverse_token=
    //           → printer blinks yellow, waits for button
    //           → {"status":"ok","answer_code":"<answer_code>"}
    //
    //   Step 2: Poll HTTPS GET https://<printer>:443/auth?response_type=answer
    //              &client_id=MakerWare&client_secret=<random>
    //              &answer_code=<answer_code>
    //           → {"answer":"pending"}  (while waiting)
    //           → {"answer":"accepted","code":"<birdwing_code>"}  (after press)
    //
    //   Step 3: HTTPS GET https://<printer>:443/auth?response_type=token
    //              &client_id=MakerWare&client_secret=<random>
    //              &context=jsonrpc&auth_code=<birdwing_code>
    //           → {"status":"success","access_token":"<token>"}
    //
    // Re-authentication reuses the stored access_token on port 9999 (KaitenSession).

    if (m_host.empty()) { error_or_token = "No IP address configured."; return BirdwingAuthResult::ConnectionFailed; }

    // Generate a client_secret once per pairing session (matches conveyor pattern)
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 35);
    const std::string chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string client_secret = "orca_";
    for (int i = 0; i < 16; ++i) client_secret += chars[dist(rng)];

    const std::string base_url = "https://" + m_host + ":443/auth?";
    const std::string common = "client_id=MakerWare&client_secret=" + client_secret;

    auto https_get = [&](const std::string& params, std::string& body, std::string& err) -> bool {
        const std::string url = base_url + params + "&" + common;
        bool ok = false;
        Http::get(url)
            .timeout_connect(10)
            .timeout_max(15)
            .tls_verify(false) // Z18 uses self-signed cert (same as conveyor's _ssl_kwargs)
            .on_complete([&](std::string resp_body, unsigned) {
                body = std::move(resp_body);
                ok = true;
            })
            .on_error([&](std::string /*body*/, std::string error, unsigned) {
                err = error;
            })
            .perform_sync();
        return ok;
    };

    // ── Step 1: Request a code (triggers yellow blink on Z18) ──────────────
    std::string body, err;
    const std::string code_params =
        "response_type=code&username=OrcaSlicer&thingiverse_token=";
    if (!https_get(code_params, body, err)) {
        error_or_token = "Could not reach Z18 at https://" + m_host + ":443 — " + err;
        BOOST_LOG_TRIVIAL(warning) << "MakerbotLink birdwing_authorize step1 failed: " << err;
        return BirdwingAuthResult::ConnectionFailed;
    }

    nlohmann::json j1;
    try { j1 = nlohmann::json::parse(body); } catch (...) {
        error_or_token = "Unexpected response from printer (step1): " + body;
        return BirdwingAuthResult::ConnectionFailed;
    }
    if (j1.value("status", "") != "ok" || !j1.contains("answer_code")) {
        error_or_token = "Printer rejected code request: " + body;
        return BirdwingAuthResult::ConnectionFailed;
    }
    const std::string answer_code = j1["answer_code"].get<std::string>();
    BOOST_LOG_TRIVIAL(info) << "MakerbotLink birdwing_authorize: got answer_code, waiting for button press...";

    // ── Step 2: Poll until button pressed (or timeout) ──────────────────────
    const std::string answer_params =
        "response_type=answer&answer_code=" + Http::url_encode(answer_code);

    std::string birdwing_code;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string abody, aerr;
        if (!https_get(answer_params, abody, aerr)) {
            BOOST_LOG_TRIVIAL(warning) << "MakerbotLink birdwing_authorize step2 poll failed: " << aerr;
            continue;
        }
        nlohmann::json j2;
        try { j2 = nlohmann::json::parse(abody); } catch (...) { continue; }
        const std::string answer = j2.value("answer", "");
        if (answer == "accepted") {
            birdwing_code = j2.value("code", "");
            break;
        } else if (answer == "rejected") {
            error_or_token = "Button press was rejected by the printer.";
            return BirdwingAuthResult::ConnectionFailed;
        }
        // answer == "pending" → keep polling
    }

    if (birdwing_code.empty()) {
        error_or_token = "Timed out waiting for button press on the printer.";
        return BirdwingAuthResult::Timeout;
    }

    // ── Step 3: Exchange birdwing_code for access_token ─────────────────────
    const std::string token_params =
        "response_type=token&context=jsonrpc&auth_code=" + Http::url_encode(birdwing_code);
    std::string tbody, terr;
    if (!https_get(token_params, tbody, terr)) {
        error_or_token = "Could not fetch access token from printer — " + terr;
        return BirdwingAuthResult::ConnectionFailed;
    }

    nlohmann::json j3;
    try { j3 = nlohmann::json::parse(tbody); } catch (...) {
        error_or_token = "Unexpected token response from printer: " + tbody;
        return BirdwingAuthResult::ConnectionFailed;
    }
    if (j3.value("status", "") == "success" && j3.contains("access_token")) {
        error_or_token = j3["access_token"].get<std::string>();
        BOOST_LOG_TRIVIAL(info) << "MakerbotLink birdwing_authorize: paired successfully, token obtained.";
        return BirdwingAuthResult::Success;
    }

    error_or_token = "Token request failed: " + tbody;
    return BirdwingAuthResult::ConnectionFailed;
}




// ── Lava/Method: JSON-RPC over HTTP ──────────────────────────────────────────

bool MakerbotLink::lava_rpc(const std::string&    method,
                             const nlohmann::json& params,
                             nlohmann::json&       out,
                             std::string&          error) const
{
    if (m_host.empty()) { error = "No IP address configured."; return false; }

    const std::string url = (boost::format("http://%1%:%2%/rpc") % m_host % m_port).str();
    const nlohmann::json payload = {
        {"jsonrpc", "2.0"}, {"method", method}, {"params", params}, {"id", 1}
    };

    std::string resp_body;
    unsigned    http_status = 0;
    std::string http_error;

    auto http = Http::post(url);
    http.header("Content-Type", "application/json");
    http.header("Accept",       "application/json");
    if (!m_access_token.empty())
        http.header("Authorization", "Bearer " + m_access_token);
    http.set_post_body(payload.dump());
    http.on_complete([&](std::string body, unsigned status) {
        resp_body   = std::move(body);
        http_status = status;
    });
    http.on_error([&](std::string, std::string err, unsigned) { http_error = std::move(err); });

    try { http.perform_sync(); } catch (const std::exception& e) { error = e.what(); return false; }

    if (!http_error.empty()) { error = http_error; return false; }
    if (resp_body.empty()) {
        error = http_status >= 400
            ? (boost::format("HTTP %1%") % http_status).str()
            : "No response from MakerBot port 2222";
        return false;
    }

    try { out = nlohmann::json::parse(resp_body); }
    catch (...) { error = "JSON parse error"; return false; }

    if (out.contains("error")) { error = out["error"].value("message", "RPC error"); return false; }
    BOOST_LOG_TRIVIAL(debug) << "MakerbotLink Lava RPC " << method << " OK";
    return true;
}


// ── PrintHost Interface ───────────────────────────────────────────────────────

wxString MakerbotLink::get_test_ok_msg() const
{
    return m_is_birdwing
        ? wxString::FromUTF8("Connected to MakerBot Birdwing (SSL port 12309). Press the handwheel to authorize.")
        : wxString::FromUTF8("Connected to MakerBot Lava/Method (HTTP port 2222).");
}

wxString MakerbotLink::get_test_failed_msg(wxString& msg) const
{
    const std::string hint = m_is_birdwing
        ? "Port 12309 SSL. For Lava/Method printers add :2222 to the IP."
        : "Port 2222 HTTP. For Birdwing printers (Z18/Replicator+) remove :2222.";
    return msg.empty()
        ? wxString::FromUTF8(hint)
        : msg + wxString::FromUTF8(" — ") + wxString::FromUTF8(hint);
}

bool MakerbotLink::test(wxString& curl_info) const
{
    std::string err;

    if (m_is_birdwing) {
        // Handshake only – immediate response, no button press needed
        nlohmann::json resp;
        if (birdwing_rpc("handshake", nlohmann::json::object(), resp, err, 10))
            return true;
    } else {
        nlohmann::json resp;
        if (lava_rpc("auth.check", nlohmann::json::object(), resp, err))
            return true;
    }

    curl_info = wxString::FromUTF8(err);
    return false;
}

bool MakerbotLink::upload(PrintHostUpload upload_data,
                           ProgressFn      prg_fn,
                           ErrorFn         err_fn,
                           InfoFn          info_fn) const
{
    std::string err;

    if (m_is_birdwing) {
        // For Birdwing uploads, the one_time_token must already be stored
        // (obtained via the Handshake Dialog in PhysicalPrinterDialog).
        // Here we just verify and proceed.
        info_fn("", "Verifying MakerBot Birdwing connection...");
        nlohmann::json resp;
        if (!birdwing_rpc("handshake", nlohmann::json::object(), resp, err, 10)) {
            err_fn("MakerBot Birdwing connection failed: " + err);
            return false;
        }
    } else {
        info_fn("", "Authenticating with MakerBot Lava/Method...");
        nlohmann::json resp;
        if (!lava_rpc("auth.check", nlohmann::json::object(), resp, err)) {
            err_fn("MakerBot auth failed: " + err);
            return false;
        }
    }

    // Upload token
    info_fn("", "Requesting upload slot...");
    const boost::filesystem::path src(upload_data.source_path.string());
    const nlohmann::json tok_params = {
        {"filename", src.filename().string()},
        {"length",   (uint64_t)boost::filesystem::file_size(src)}
    };

    nlohmann::json tok_resp;
    const bool tok_ok = m_is_birdwing
        ? birdwing_rpc("upload.request_token", tok_params, tok_resp, err, 30)
        : lava_rpc("upload.request_token", tok_params, tok_resp, err);

    if (!tok_ok) { err_fn("Upload token failed: " + err); return false; }

    const std::string uid = tok_resp["result"]["upload_id"].get<std::string>();
    const std::string schema = m_is_birdwing ? "https" : "http";
    const std::string upload_url =
        (boost::format("%1%://%2%:%3%/upload/%4%") % schema % m_host % m_port % uid).str();

    info_fn("", "Streaming .makerbot archive...");

    unsigned    http_status = 0;
    std::string upload_err;
    auto http = Http::put(upload_url);
    if (m_is_birdwing) http.tls_verify(false);
    if (!m_access_token.empty()) http.header("Authorization", "Bearer " + m_access_token);
    http.set_put_body(src);
    http.on_complete([&](std::string, unsigned s) { http_status = s; });
    http.on_error([&](std::string, std::string e, unsigned) { upload_err = std::move(e); });
    http.on_progress([&prg_fn](Http::Progress p, bool& c) { prg_fn(p, c); });

    try { http.perform_sync(); } catch (const std::exception& e) {
        err_fn(std::string("Upload error: ") + e.what()); return false;
    }
    if (!upload_err.empty()) { err_fn("Upload error: " + upload_err); return false; }
    if (http_status != 200 && http_status != 201) {
        err_fn((boost::format("HTTP %1%") % http_status).str()); return false;
    }

    info_fn("", "Upload complete.");
    bool cancel = false;
    prg_fn(Http::Progress(0, 0, 100, 100, ""), cancel);
    return true;
}

} // namespace Slic3r
