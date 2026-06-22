// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// Implementierung: UltiMaker REST-API Netzwerkschicht

#include "UltimakerLink.hpp"
#include "Http.hpp"

#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>

#include <wx/string.h>

namespace Slic3r {

// ── Konstruktor ───────────────────────────────────────────────────────────────

UltimakerLink::UltimakerLink(DynamicPrintConfig *config)
{
    if (const auto *opt = config->opt<ConfigOptionString>("print_host"))
        m_host = opt->value;

    // printhost_password: Format "api_id:api_key"
    std::string stored_auth;
    if (const auto *opt = config->opt<ConfigOptionString>("printhost_password"))
        stored_auth = opt->value;

    const size_t colon = stored_auth.find(':');
    if (colon != std::string::npos) {
        m_api_id  = stored_auth.substr(0, colon);
        m_api_key = stored_auth.substr(colon + 1);
    }

    for (const auto &prefix : { "https://", "http://" })
        if (m_host.rfind(prefix, 0) == 0)
            m_host.erase(0, std::string(prefix).size());

    const size_t port_colon = m_host.find(':');
    if (port_colon != std::string::npos) {
        try { m_port = std::stoi(m_host.substr(port_colon + 1)); }
        catch (...) { m_port = 80; }
        m_host = m_host.substr(0, port_colon);
    }
}

// ── REST-Helfer ───────────────────────────────────────────────────────────────

bool UltimakerLink::rest_get(const std::string &endpoint,
                              nlohmann::json    &out,
                              std::string       &error) const
{
    const std::string url =
        (boost::format("http://%1%:%2%%3%") % m_host % m_port % endpoint).str();

    std::string response_body;
    unsigned    http_status = 0;
    std::string http_error;

    auto http = Http::get(url);
    http.header("Accept", "application/json");
    if (!m_api_id.empty())  http.header("X-Api-ID",  m_api_id);
    if (!m_api_key.empty()) http.header("X-Api-Key", m_api_key);

    http.on_complete([&](std::string body, unsigned status) {
        response_body = std::move(body);
        http_status   = status;
    });
    http.on_error([&](std::string /*body*/, std::string err, unsigned /*status*/) {
        http_error = std::move(err);
    });

    try {
        http.perform_sync();
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }

    if (!http_error.empty()) { error = http_error; return false; }
    if (http_status >= 400) {
        error = (boost::format("HTTP error %1%") % http_status).str();
        return false;
    }
    if (!response_body.empty()) {
        try { out = nlohmann::json::parse(response_body); }
        catch (...) { error = "JSON parse error in UltiMaker response."; return false; }
    }

    BOOST_LOG_TRIVIAL(debug) << "UltimakerLink GET " << endpoint << " OK";
    return true;
}

bool UltimakerLink::rest_post(const std::string &endpoint,
                               const std::string &body,
                               nlohmann::json    &out,
                               std::string       &error) const
{
    const std::string url =
        (boost::format("http://%1%:%2%%3%") % m_host % m_port % endpoint).str();

    std::string response_body;
    unsigned    http_status = 0;
    std::string http_error;

    auto http = Http::post(url);
    http.header("Content-Type", "application/json");
    http.header("Accept",       "application/json");
    if (!m_api_id.empty())  http.header("X-Api-ID",  m_api_id);
    if (!m_api_key.empty()) http.header("X-Api-Key", m_api_key);
    if (!body.empty()) http.set_post_body(body);

    http.on_complete([&](std::string resp, unsigned status) {
        response_body = std::move(resp);
        http_status   = status;
    });
    http.on_error([&](std::string /*body*/, std::string err, unsigned /*status*/) {
        http_error = std::move(err);
    });

    try {
        http.perform_sync();
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }

    if (!http_error.empty()) { error = http_error; return false; }
    if (http_status >= 400) {
        error = (boost::format("HTTP error %1%") % http_status).str();
        return false;
    }
    if (!response_body.empty()) {
        try { out = nlohmann::json::parse(response_body); }
        catch (...) { error = "JSON parse error in UltiMaker response."; return false; }
    }

    BOOST_LOG_TRIVIAL(debug) << "UltimakerLink POST " << endpoint << " OK";
    return true;
}

// ── Authentifizierung ─────────────────────────────────────────────────────────

bool UltimakerLink::check_auth(std::string &error) const
{
    if (m_host.empty()) {
        error = "No UltiMaker IP address configured.";
        return false;
    }

    // Kein Token vorhanden → Pairing anfordern
    if (m_api_id.empty() || m_api_key.empty()) {
        nlohmann::json req = {
            { "application", "OrcaSlicerFork" },
            { "user",        "NetworkUser"    }
        };
        nlohmann::json resp;
        if (!rest_post("/api/v1/authentication/request", req.dump(), resp, error))
            return false;

        if (resp.contains("id") && resp.contains("key")) {
            // Signalisiert der GUI, dass der User am Drucker bestätigen muss
            error = "PAIRING_PENDING:"
                  + resp["id"].get<std::string>() + ":"
                  + resp["key"].get<std::string>();
        } else {
            error = "Unexpected pairing response from UltiMaker.";
        }
        return false;
    }

    // Vorhandenes Token verifizieren
    nlohmann::json resp;
    if (!rest_get("/api/v1/authentication/verify", resp, error))
        return false;

    if (resp.value("message", "") == "authorized")
        return true;

    error = "UltiMaker API token invalid or revoked.";
    return false;
}

// ── PrintHost Interface ───────────────────────────────────────────────────────

wxString UltimakerLink::get_test_ok_msg() const
{
    return wxString::FromUTF8("Connected to UltiMaker printer successfully.");
}

wxString UltimakerLink::get_test_failed_msg(wxString &msg) const
{
    return msg.empty()
        ? wxString::FromUTF8("Could not connect to the UltiMaker printer.")
        : wxString::FromUTF8("Could not connect to the UltiMaker printer: ") + msg;
}

bool UltimakerLink::test(wxString &curl_info) const
{
    std::string err;
    if (check_auth(err))
        return true;
    curl_info = wxString::FromUTF8(err);
    return false;
}

bool UltimakerLink::upload(PrintHostUpload upload_data,
                            ProgressFn      prg_fn,
                            ErrorFn         err_fn,
                            InfoFn          info_fn) const
{
    // ── 1. Auth prüfen ────────────────────────────────────────────────────────
    std::string err;
    if (!check_auth(err)) {
        err_fn("UltiMaker auth failed: " + err);
        return false;
    }

    // ── 2. Datei als Multipart POST an /api/v1/print_job senden ──────────────
    const std::string url =
        (boost::format("http://%1%:%2%/api/v1/print_job") % m_host % m_port).str();

    info_fn("", "Uploading .ufp bundle to UltiMaker...");

    unsigned    http_status = 0;
    std::string upload_err;

    const boost::filesystem::path src_path(upload_data.source_path.string());

    auto http = Http::post(url);
    if (!m_api_id.empty())  http.header("X-Api-ID",  m_api_id);
    if (!m_api_key.empty()) http.header("X-Api-Key", m_api_key);
    http.form_add_file("file", src_path.string(), src_path.filename().string());

    http.on_complete([&](std::string /*body*/, unsigned status) {
        http_status = status;
    });
    http.on_error([&](std::string /*body*/, std::string msg, unsigned /*status*/) {
        upload_err = std::move(msg);
    });
    http.on_progress([&prg_fn](Http::Progress progress, bool &cancel) {
        prg_fn(progress, cancel);
    });

    try {
        http.perform_sync();
    } catch (const std::exception &e) {
        err_fn(std::string("UltiMaker upload exception: ") + e.what());
        return false;
    }

    if (!upload_err.empty()) {
        err_fn("UltiMaker upload error: " + upload_err);
        return false;
    }
    if (http_status != 200 && http_status != 201) {
        err_fn((boost::format("UltiMaker rejected the job (HTTP %1%)") % http_status).str());
        return false;
    }

    info_fn("", "Print job queued on UltiMaker successfully.");
    bool cancel = false;
    prg_fn(Http::Progress(0, 0, 100, 100, ""), cancel);
    return true;
}

} // namespace Slic3r
