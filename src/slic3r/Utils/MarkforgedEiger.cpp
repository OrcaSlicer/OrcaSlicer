#include "MarkforgedEiger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/string.h>
#include <wx/utils.h>

#include "Http.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r_version.h"
#include "slic3r/GUI/I18N.hpp"

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace Slic3r {

namespace {

constexpr const char* API_BASE      = "https://www.eiger.io/api/v3";
constexpr const char* EIGER_WEB     = "https://www.eiger.io/";
constexpr int         SLICE_TIMEOUT = 20 * 60;
constexpr int         BUILD_WAIT    = 60;

std::string trim_quotes(std::string s)
{
    while (!s.empty() && (s.front() == '"' || s.front() == ' '))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == '"' || s.back() == ' '))
        s.pop_back();
    return s;
}

json read_settings()
{
    boost::nowide::ifstream in(Eiger::settings_file().string());
    if (!in)
        return json::object();
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    return j.is_object() ? j : json::object();
}

json items_of(const json& res)
{
    if (res.is_array())
        return res;
    if (res.is_object() && res.contains("items") && res["items"].is_array())
        return res["items"];
    return json::array();
}

// Seconds since the epoch of an ISO-8601 UTC time such as "2026-11-08T17:20:31.123Z"; 0 when unparsable.
double iso_time(const std::string& text)
{
    std::tm tm{};
    std::istringstream in(text.substr(0, 19));
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (in.fail())
        return 0;
#ifdef _WIN32
    return double(_mkgmtime(&tm));
#else
    return double(timegm(&tm));
#endif
}

std::string user_agent() { return std::string(SLIC3R_APP_KEY) + "-Markforged/" + SLIC3R_VERSION; }

json perform(Http&& http)
{
    std::string body, error;
    unsigned    status = 0;
    http.header("Accept", "application/json")
        .header("User-Agent", user_agent())
        .on_complete([&](std::string b, unsigned s) {
            body   = std::move(b);
            status = s;
        })
        .on_error([&](std::string b, std::string err, unsigned s) {
            body   = std::move(b);
            error  = err;
            status = s;
        })
        .perform_sync();
    if (status == 401 || status == 403)
        throw std::runtime_error(_u8L("Eiger rejected the API keys. Check the access key and secret key."));
    if (!error.empty())
        throw std::runtime_error("Eiger API " + std::to_string(status) + ": " + (body.empty() ? error : body.substr(0, 300)));
    return body.empty() ? json() : json::parse(body, nullptr, false);
}

} // namespace

fs::path Eiger::settings_file() { return fs::path(data_dir()) / "orca_plugins" / "markforged_eiger_data" / "settings.json"; }

EigerKeys Eiger::keys_for(const DynamicPrintConfig* cfg)
{
    EigerKeys k;
    if (cfg != nullptr) {
        auto str = [&](const char* key) { return cfg->has(key) ? trim_quotes(cfg->opt_string(key)) : std::string(); };
        k.access = str("printhost_user");
        k.secret = str("printhost_password");
        if (!k.empty())
            return k;
        const std::string key   = str("printhost_apikey");
        const auto        colon = key.find(':');
        if (colon != std::string::npos) {
            k.access = trim_quotes(key.substr(0, colon));
            k.secret = trim_quotes(key.substr(colon + 1));
            if (!k.empty())
                return k;
        }
    }
    const json s = read_settings();
    k.access     = s.value("access_key", "");
    k.secret     = s.value("secret_key", "");
    return k;
}

void Eiger::save_keys(const EigerKeys& keys)
{
    json s          = read_settings(); // keep the agent's other settings
    s["access_key"] = keys.access;
    s["secret_key"] = keys.secret;
    fs::create_directories(settings_file().parent_path());
    boost::nowide::ofstream out(settings_file().string(), std::ios::trunc);
    out << s.dump(2);
}

std::string Eiger::saved_settings_preset() { return read_settings().value("settings_preset", ""); }

json Eiger::get(const std::string& path_and_query)
{
    return perform(std::move(Http::get(std::string(API_BASE) + path_and_query).auth_basic(m_keys.access, m_keys.secret)));
}

void Eiger::post(const std::string& path, const json& body)
{
    Http http = Http::post(std::string(API_BASE) + path);
    http.auth_basic(m_keys.access, m_keys.secret).header("Content-Type", "application/json").set_post_body(body.dump());
    perform(std::move(http));
}

std::vector<json> Eiger::devices()
{
    std::vector<json> out;
    for (int page = 1;; ++page) {
        const json res = get("/devices?page%5Bnumber%5D=" + std::to_string(page) + "&page%5Bsize%5D=100");
        for (const json& d : items_of(res))
            out.push_back(d);
        if (!(res.is_object() && res.value("has_more_items", false)) || page > 50)
            return out;
    }
}

json Eiger::device(const std::string& id) { return get("/devices/" + Http::url_encode(id)); }

std::vector<std::pair<std::string, std::string>> Eiger::settings_presets()
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const json& p : items_of(get("/parts/settings_presets"))) {
        const std::string id = p.contains("id") && p["id"].is_string() ? p["id"].get<std::string>() : p.value("id", json()).dump();
        out.emplace_back(id, p.value("title", id));
    }
    return out;
}

json Eiger::upload_stl(const fs::path& stl, const std::string& title, const std::string& preset_id)
{
    Http http = Http::post(std::string(API_BASE) + "/parts/upload_stl");
    http.auth_basic(m_keys.access, m_keys.secret).timeout_max(300).form_add("title", title);
    if (!preset_id.empty())
        http.form_add("settingsPresetId", preset_id);
    http.form_add_file("stlFile", stl, title + ".stl");
    return perform(std::move(http));
}

std::string Eiger::slice_status(const std::string& id)
{
    const json j = get("/parts/slice_status/" + Http::url_encode(id));
    return j.is_object() ? j.value("status", "") : std::string();
}

std::vector<json> Eiger::builds_titled(const std::string& title)
{
    const json        res = get("/builds?filter%5Btitle%5D%5Beq%5D=" + Http::url_encode(title) +
                         "&sort%5Bby%5D=created_at&sort%5Border%5D=desc&page%5Bsize%5D=20");
    std::vector<json> out;
    for (const json& b : items_of(res))
        out.push_back(b);
    return out;
}

// ---------------------------------------------------------------------------------------------

bool MarkforgedEigerHost::test(wxString& msg) const
{
    try {
        Eiger(m_keys).devices();
        return true;
    } catch (const std::exception& e) {
        msg = wxString::FromUTF8(e.what());
        return false;
    }
}

wxString MarkforgedEigerHost::get_test_ok_msg() const { return _L("Connected to Eiger."); }

wxString MarkforgedEigerHost::get_test_failed_msg(wxString& msg) const
{
    return wxString::Format("%s: %s", _L("Could not connect to Eiger"), msg);
}

bool MarkforgedEigerHost::upload(PrintHostUpload u, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    const std::string dummy;
    auto step = [&](int pct, const std::string& status) {
        if (!status.empty() && info_fn)
            info_fn(L"set_status", wxString::FromUTF8(status));
        bool cancel = false;
        if (progress_fn)
            progress_fn(Http::Progress(0, 0, 100, size_t(pct), dummy), cancel);
        if (cancel)
            throw std::runtime_error(_u8L("Cancelled"));
    };
    const std::string device = u.extended("eiger_device");
    const std::string action = u.extended("eiger_action", "now");
    const std::string preset = u.extended("eiger_preset");
    const std::string title  = u.extended("eiger_title", u.upload_path.stem().string());
    try {
        Eiger api(m_keys);
        step(10, _u8L("Uploading to Eiger…"));
        const double started = double(std::time(nullptr));
        const json   up      = api.upload_stl(u.source_path, title, preset);
        std::string  job     = up.is_object() && up.contains("slice_job_id") ? up["slice_job_id"].is_string() ? up["slice_job_id"].get<std::string>() :
                                                                                                                   up["slice_job_id"].dump() :
                                                                             std::string();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(SLICE_TIMEOUT);
        while (!job.empty()) {
            std::string st = api.slice_status(job);
            for (char& c : st)
                c = char(toupper(c));
            if (st == "COMPLETED")
                break;
            if (st == "ERROR" || st == "CANCELED")
                throw std::runtime_error(_u8L("Eiger could not slice the part."));
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error(_u8L("Eiger is still slicing after 20 minutes."));
            step(40, _u8L("Eiger is slicing…"));
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }

        // The API cannot create a build: use the one Eiger made for this part, if it made one.
        step(80, _u8L("Looking for the Eiger build…"));
        std::string build_id;
        const auto  wait_until = std::chrono::steady_clock::now() + std::chrono::seconds(BUILD_WAIT);
        while (build_id.empty() && std::chrono::steady_clock::now() < wait_until) {
            for (const json& b : api.builds_titled(title)) {
                const double created = iso_time(b.value("created_at", ""));
                if (b.value("sliced", false) && created > 0 && created >= started - 120) {
                    build_id = b["id"].is_string() ? b["id"].get<std::string>() : b["id"].dump();
                    break;
                }
            }
            if (build_id.empty()) {
                step(80, std::string());
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
        if (build_id.empty()) {
            wxLaunchDefaultBrowser(EIGER_WEB);
            throw std::runtime_error(_u8L("Eiger sliced the part but made no build for it. Add the part to a build in Eiger to print it."));
        }

        step(90, _u8L("Sending the build to the printer…"));
        if (action == "backlog") {
            api.post("/backlog/" + Http::url_encode(build_id), json::object());
        } else {
            const std::string state = action == "now" ? api.device(device).value("state", "") : std::string();
            if (action == "now" && state == "Ready")
                api.post("/devices/" + Http::url_encode(device), {{"build", build_id}});
            else // queue, or the printer is busy
                api.post("/devices/" + Http::url_encode(device) + "/queue", {{"build", build_id}});
        }
        step(100, std::string());
        boost::system::error_code ec;
        fs::remove(u.source_path, ec);
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Eiger: " << e.what();
        if (error_fn)
            error_fn(wxString::FromUTF8(e.what()));
        return false;
    }
}

} // namespace Slic3r
