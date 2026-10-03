#include "Printago.hpp"

#include <cstdlib>

#include <wx/secretstore.h>
#include <wx/string.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include "slic3r/Utils/Http.hpp" // Http::url_decode for the callback token

namespace Slic3r {

// NOTE: these paths are the contract with the Printago web app (see the embedded-pages protocol
// spec). Adjust to match the real Printago routes if they change.
static const std::string ORCA_PATH   = "/orca";
static const std::string LAUNCH_PATH = "/orca/launch";
// Both "Save to Printago" and "Save & Queue to Printago" use this single route; the mode is chosen
// by an ?intent=save / ?intent=queue query parameter (see send_url()/queue_url()).
static const std::string SEND_PATH   = "/orca/send";

// wxSecretStore coordinates for the single Orca session token.
static const wxString SECRET_SERVICE = "OrcaSlicer/Printago";
static const wxString SECRET_ACCOUNT = "printago_session_token";

std::string printago_web_base()
{
    // Dev override: point the whole integration (tab + dialogs) at another stack by exporting
    // PRINTAGO_URL (e.g. http://localhost:3000). Defaults to production otherwise.
    if (const char* env = std::getenv("PRINTAGO_URL")) {
        std::string base(env);
        while (!base.empty() && base.back() == '/')
            base.pop_back();
        if (!base.empty())
            return base;
    }
    return "https://app.printago.io";
}

PrintagoEndpoints default_printago_endpoints()
{
    return PrintagoEndpoints{
        /* web_base    */ printago_web_base(),
        /* orca_path   */ ORCA_PATH,
        /* launch_path */ LAUNCH_PATH,
        /* send_path   */ SEND_PATH,
    };
}

std::string PrintagoEndpoints::login_url() const { return web_base + orca_path; }

std::string PrintagoEndpoints::launch_url(const std::string& token) const
{
    // The token rides in the URL fragment (never the query string) to keep it out of logs.
    return web_base + launch_path + "#token=" + token;
}

std::string PrintagoEndpoints::send_url() const { return web_base + send_path + "?intent=save"; }

std::string PrintagoEndpoints::queue_url() const { return web_base + send_path + "?intent=queue"; }

std::string PrintagoEndpoints::replace_url(const std::string& part_id) const
{
    // partIds are plain Firestore-style ids (alphanumeric), so no URL-encoding is needed here.
    return web_base + "/orca/replace?partId=" + part_id;
}

Printago::Printago(PrintagoEndpoints endpoints) : ep_(std::move(endpoints)) { load_session_token(); }

bool Printago::load_session_token()
{
    token_.clear();
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk()) {
        BOOST_LOG_TRIVIAL(warning) << "Printago: secret store unavailable; session not persisted";
        return false;
    }
    wxString      account;
    wxSecretValue secret;
    if (store.Load(SECRET_SERVICE, account, secret) && secret.IsOk()) {
        token_.assign(static_cast<const char*>(secret.GetData()), secret.GetSize());
    }
    return !token_.empty();
}

void Printago::save_session_token(const std::string& token)
{
    if (token.empty()) {
        clear_session_token();
        return;
    }
    token_ = token;
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk()) {
        BOOST_LOG_TRIVIAL(warning) << "Printago: secret store unavailable; token kept in memory only";
        return;
    }
    wxSecretValue secret(wxString::FromUTF8(token.c_str()));
    if (!store.Save(SECRET_SERVICE, SECRET_ACCOUNT, secret))
        BOOST_LOG_TRIVIAL(warning) << "Printago: failed to save session token to secret store";
}

void Printago::clear_session_token()
{
    token_.clear();
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk())
        store.Delete(SECRET_SERVICE);
}

std::string Printago::login_url() const { return ep_.login_url(); }

std::string Printago::send_url() const { return ep_.send_url(); }

std::string Printago::queue_url() const { return ep_.queue_url(); }

std::string Printago::relaunch_url() const
{
    return token_.empty() ? ep_.login_url() : ep_.launch_url(token_);
}

std::optional<std::string> Printago::parse_callback_token(const std::string& url)
{
    if (!boost::istarts_with(url, PRINTAGO_CALLBACK_PREFIX))
        return std::nullopt;

    const auto qpos = url.find('?');
    if (qpos == std::string::npos)
        return std::nullopt;

    // Scan the query string for token=<value>.
    const std::string query = url.substr(qpos + 1);
    size_t            i      = 0;
    while (i < query.size()) {
        const size_t amp = query.find('&', i);
        const size_t end = amp == std::string::npos ? query.size() : amp;
        const std::string pair = query.substr(i, end - i);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == "token") {
            const std::string value = pair.substr(eq + 1);
            if (value.empty())
                return std::nullopt;
            return Http::url_decode(value);
        }
        if (amp == std::string::npos)
            break;
        i = amp + 1;
    }
    return std::nullopt;
}

} // namespace Slic3r
