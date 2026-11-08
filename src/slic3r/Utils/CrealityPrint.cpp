#include "CrealityPrint.hpp"

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <chrono>
#include <boost/beast/websocket/stream_base.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <map>
#include <unordered_set>
#include <sstream>
#include <exception>
#include <thread>
#include <boost/format.hpp>
#include <boost/foreach.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>

#include <curl/curl.h>
#include <utility>
#include <vector>
#include <wx/progdlg.h>

#include "PrintHost.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "AsioUtils.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/BonjourDialog.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

#include <fstream>
#include <nlohmann/json.hpp>
#include <wx/string.h>
using json = nlohmann::json;

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace Slic3r {

CrealityPrint::CrealityPrint(DynamicPrintConfig* config) : 
    m_host(config->opt_string("print_host")), 
    m_web_ui(config->opt_string("print_host_webui")),
    m_cafile(config->opt_string("printhost_cafile")),
    m_port(config->opt_string("printhost_port")),
    m_apikey(config->opt_string("printhost_apikey")),
    m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* CrealityPrint::get_name() const { return "Creality Print"; }

std::string CrealityPrint::get_host() const {
    return m_host;
}
void  CrealityPrint::set_auth(Http& http) const
{
    http.header("Authorization", "Bearer " + m_apikey);
    if (!m_cafile.empty()) {
        http.ca_file(m_cafile);
    }
}

wxString CrealityPrint::get_test_ok_msg() const { return _(L("Connected to CrealityPrint successfully!")); }

wxString CrealityPrint::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to CrealityPrint"), msg.Truncate(256));
}

bool CrealityPrint::test(wxString& msg) const
{
    return probe_info(&msg);
}

bool CrealityPrint::probe_info(wxString* msg) const
{
    bool res = true;
    const char* name = get_name();
    auto url = make_url("info");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.timeout_max(5)
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status %
                                            body;
            res = false;
            if (msg)
                *msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;
            try {
                auto info = json::parse(body);
                if (info.contains("model")) {
                    m_model = info["model"].get<std::string>();
                    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Detected model: %2%") % name % m_model;
                }
            } catch (const json::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Failed to parse /info response: %2%") % name % e.what();
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            if (msg)
                *msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();

    return res;
}

PrintHostPostUploadActions CrealityPrint::get_post_upload_actions() const {
    return PrintHostPostUploadAction::StartPrint; 
}

bool CrealityPrint::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{   
    const char* name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    wxString test_msg;
    if (!test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    bool res = true;
    const auto safe_upload_filename = safe_filename(upload_filename.string());
    const bool multi_color          = supports_multi_color_print();

    // CFS-capable firmware rejects a "path" form field, but takes a folder in the URL with its slashes encoded.
    std::string folder;
    if (multi_color) {
        if (!gcode_folder(upload_parent_path, folder)) {
            error_fn(GUI::format_wxstr(_L("The folder \"%1%\" is not inside the printer's G-code folder."),
                                       upload_parent_path.generic_string()));
            return false;
        }
        wxString folder_msg;
        if (!folder.empty() && !ensure_gcode_folder(folder, folder_msg)) {
            error_fn(std::move(folder_msg));
            return false;
        }
    }
    const std::string stored_path = folder.empty() ? safe_upload_filename : folder + "/" + safe_upload_filename;

    // Only encode the URL path segment; keep the multipart filename as the stored filename.
    auto url = make_url("upload/" + Http::url_encode(stored_path));

    auto  http = Http::post(url); // std::move(url));
    set_auth(http);
    if (!multi_color)
        http.form_add("path", upload_parent_path.string());
    http.form_add_file("file", upload_data.source_path.string(), safe_upload_filename)

        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;

            // The upload server answers OK even when it could not store the file.
            if (!folder.empty() && !gcode_file_listed(folder, safe_upload_filename)) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: The printer does not list the uploaded %2%") % name % stored_path;
                error_fn(GUI::format_wxstr(_L("The printer accepted the upload but does not list \"%1%\"."), stored_path));
                res = false;
                return;
            }

            if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
                wxString errormsg;
                if (!start_print(errormsg, stored_path, upload_data.extended_info)) {
                    error_fn(std::move(errormsg));
                    res = false;
                }
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file to %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error %
                                            status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    return res;
}

std::string CrealityPrint::make_url(const std::string &path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/') {
            return (boost::format("%1%%2%") % m_host % path).str();
        } else {
            return (boost::format("%1%/%2%") % m_host % path).str();
        }
    } else {
        return (boost::format("http://%1%/%2%") % m_host % path).str();
    }
}

std::string CrealityPrint::safe_filename(const std::string &filename) const
{
    std::string safe_filename = filename;
    std::replace(safe_filename.begin(), safe_filename.end(), ' ', '_');

    return safe_filename;
}

bool CrealityPrint::gcode_folder(const boost::filesystem::path& parent, std::string& folder) const
{
    folder.clear();
    // generic_string() gives '/' separators on Windows too, which the URL needs.
    std::vector<std::string> segments;
    const std::string        generic = parent.generic_string();
    boost::split(segments, generic, [](char c) { return c == '/'; });
    for (const std::string& segment : segments) {
        if (segment.empty() || segment == ".")
            continue;
        if (segment == "..")
            return false;
        // Folders get the same treatment as the filename.
        folder += (folder.empty() ? "" : "/") + safe_filename(segment);
    }
    return true;
}

std::string CrealityPrint::moonraker_url(const std::string& path) const
{
    return "http://" + Http::get_host_from_url(m_host) + ":" + moonraker_settings.port + "/" + path;
}

bool CrealityPrint::ensure_gcode_folder(const std::string& folder, wxString& msg) const
{
    const char* name = get_name();
    std::vector<std::string> segments;
    boost::split(segments, folder, [](char c) { return c == '/'; });

    // Moonraker creates one level per request, so walk the path from the root.
    std::string level;
    for (const std::string& segment : segments) {
        level += (level.empty() ? "" : "/") + segment;
        const std::string url = moonraker_url("server/files/directory?path=" + Http::url_encode("gcodes/" + level));

        // No auth header: the printer's Moonraker trusts the LAN and rejects an empty bearer token.
        bool        exists      = false;
        unsigned    list_status = 0;
        std::string list_error;
        Http::get(url)
            .timeout_connect(5)
            .timeout_max(10)
            .on_complete([&](std::string, unsigned) { exists = true; })
            .on_error([&](std::string body, std::string error, unsigned status) {
                list_status = status;
                list_error  = error.empty() ? body : error;
            })
            .perform_sync();
        if (exists)
            continue;
        // Moonraker answers 400 for a folder that does not exist.
        if (list_status != 400) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Could not check folder %2% at %3%: %4%, HTTP %5%") % name % level % url %
                                            list_error % list_status;
            msg = GUI::format_wxstr(_L("Could not check the folder \"%1%\" on the printer (Moonraker on port %2%): %3%"), level,
                                    moonraker_settings.port, list_error);
            return false;
        }

        bool        created = false;
        std::string create_error;
        Http::post(url)
            .timeout_connect(5)
            .timeout_max(10)
            .on_complete([&](std::string, unsigned) { created = true; })
            .on_error([&](std::string body, std::string error, unsigned status) {
                // Another client may have created it meanwhile.
                if (status == 400 && body.find("File exists") != std::string::npos)
                    created = true;
                else
                    create_error = error.empty() ? body : error;
            })
            .perform_sync();
        if (!created) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Could not create folder %2%: %3%") % name % level % create_error;
            msg = GUI::format_wxstr(_L("The folder \"%1%\" does not exist on the printer and could not be created: %2%"), level,
                                    create_error);
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Created folder %2%") % name % level;
    }
    return true;
}

bool CrealityPrint::gcode_file_listed(const std::string& folder, const std::string& filename) const
{
    const std::string url = moonraker_url("server/files/directory?path=" + Http::url_encode("gcodes/" + folder));
    // Moonraker notices a new file shortly after it is written.
    for (int attempt = 0; attempt < moonraker_settings.list_retries; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(moonraker_settings.list_interval);
        bool listed = false;
        Http::get(url)
            .timeout_connect(5)
            .timeout_max(10)
            .on_complete([&](std::string body, unsigned) {
                const json listing = json::parse(body, nullptr, false);
                if (!listing.is_object() || !listing.contains("result") || !listing["result"].contains("files"))
                    return;
                for (const json& file : listing["result"]["files"])
                    if (file.is_object() && file.value("filename", "") == filename)
                        listed = true;
            })
            .on_error([&](std::string, std::string, unsigned) {})
            .perform_sync();
        if (listed)
            return true;
    }
    return false;
}

// Run every websocket call through run_until_done(), since expires_after() does not apply to synchronous Beast calls.
static void ws_connect(net::io_context& ioc, websocket::stream<beast::tcp_stream>& ws,
                       const std::string& host_url, const std::string& port, std::chrono::milliseconds timeout)
{
    std::string host = Http::get_host_from_url(host_url);

    tcp::resolver resolver{ioc};
    beast::tcp_stream& stream = beast::get_lowest_layer(ws);
    if (const auto ec = connect_until_done(ioc, stream, resolver.resolve(host, port), timeout))
        throw beast::system_error{ec};
    host += ':' + std::to_string(stream.socket().remote_endpoint().port());

    ws.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) {
            req.set(http::field::user_agent,
                std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-coro");
        }));
    stream.expires_after(timeout);
    if (const auto ec = run_until_done(ioc, [&](auto&& handler) { ws.async_handshake(host, "/", handler); }))
        throw beast::system_error{ec};
    // K1-family firmware requires TEXT-mode WebSocket frames; K2-family accepts both.
    ws.text(true);
}

static beast::error_code ws_write(net::io_context& ioc, websocket::stream<beast::tcp_stream>& ws, const std::string& text)
{
    return run_until_done(ioc, [&](auto&& handler) { ws.async_write(net::buffer(text), handler); });
}

static void ws_send(net::io_context& ioc, websocket::stream<beast::tcp_stream>& ws, const std::string& text)
{
    if (const auto ec = ws_write(ioc, ws, text))
        throw beast::system_error{ec};
}

static void ws_send(net::io_context& ioc, websocket::stream<beast::tcp_stream>& ws, const json& cmd)
{
    ws_send(ioc, ws, to_string(cmd));
}

static beast::error_code ws_read(net::io_context& ioc, websocket::stream<beast::tcp_stream>& ws, beast::flat_buffer& buffer)
{
    return run_until_done(ioc, [&](auto&& handler) { ws.async_read(buffer, handler); });
}

// Returns a status's cfsConnect value, 0 without a CFS or 1 with one, or -1 when the message holds neither.
static int reported_cfs_connect(const std::string& msg)
{
    if (msg.find("cfsConnect") == std::string::npos)
        return -1;
    const json status = json::parse(msg, nullptr, false);
    if (!status.is_object() || !status.contains("cfsConnect"))
        return -1;
    const json& cfs_connect = status["cfsConnect"];
    if (cfs_connect == 0)
        return 0;
    return cfs_connect == 1 ? 1 : -1;
}

// Sends the CFS slot query and returns the reply, or nothing once a status reports no CFS connected, which also sets *no_cfs.
// The query gets slots_reply, or the later of slots_reply and cfs_reply once a status reports a CFS connected, since such a printer does answer.
static std::string ws_query_slots(net::io_context&                      ioc,
                                  websocket::stream<beast::tcp_stream>& ws,
                                  std::chrono::milliseconds             slots_reply,
                                  std::chrono::milliseconds             cfs_reply,
                                  bool*                                 no_cfs)
{
    const auto sent     = std::chrono::steady_clock::now();
    auto       deadline = sent + slots_reply;
    beast::get_lowest_layer(ws).expires_at(deadline);
    ws_send(ioc, ws, json{{"method", "get"}, {"params", {{"boxsInfo", 1}}}});

    // The deadline also ends the read when the printer keeps sending heartbeats or status updates.
    while (std::chrono::steady_clock::now() < deadline) {
        beast::flat_buffer buf;
        const beast::error_code ec = ws_read(ioc, ws, buf);
        if (ec == beast::error::timeout)
            break;
        if (ec)
            throw beast::system_error{ec};
        std::string msg = beast::buffers_to_string(buf.data());
        // K1-family firmware sends periodic heartbeat pings; ack them so the
        // printer does not close the connection, then keep reading.
        if (msg.find("heart_beat") != std::string::npos) {
            ws_write(ioc, ws, "ok");
            continue;
        }
        if (msg.find("boxsInfo") != std::string::npos)
            return msg;
        const int cfs_connect = reported_cfs_connect(msg);
        if (cfs_connect == 0) {
            BOOST_LOG_TRIVIAL(info) << "CrealityPrint: The printer reports no CFS connected";
            if (no_cfs)
                *no_cfs = true;
            return {};
        }
        if (cfs_connect == 1) {
            deadline = sent + std::max(slots_reply, cfs_reply);
            beast::get_lowest_layer(ws).expires_at(deadline);
        }
    }
    BOOST_LOG_TRIVIAL(warning) << "CrealityPrint: Timed out waiting for a 'boxsInfo' response";
    return {};
}

// The K1 SE closes its side after a reply and K1-family printers after a start command, so a close error is not a failure.
static void ws_close(net::io_context& ioc, websocket::stream<beast::tcp_stream>& ws, std::chrono::milliseconds timeout)
{
    beast::get_lowest_layer(ws).expires_after(timeout);
    run_until_done(ioc, [&](auto&& handler) { ws.async_close(websocket::close_code::normal, handler); });
}

// K1-family firmware stores gcodes under /usr/data and uses a stricter WebSocket
// protocol (TEXT frames, heartbeat acks). The /info model is the literal name on
// K1-family, including the *_CFS-C bundle variants. Add verified model IDs here.
static bool is_k1_family(const std::string& model)
{
    static const std::unordered_set<std::string> k1_models = {
        "K1",
        "K1 Max",
        "K1 Max_CFS-C",
        "K1 SE",
        "K1 SE_CFS-C",
        "K1C",
        "K1C_CFS-C",
        "K1_CFS-C",
    };
    return k1_models.count(model) > 0;
}

void CrealityPrint::query_model() const
{
    if (!m_model.empty())
        return;

    wxString msg;
    test(msg);
}

// CFS-capable models. One table for capability checks, display names and
// LAN-discovery labelling -- keep additions here only.
static const std::map<std::string, std::string>& cfs_capable_models()
{
    static const std::map<std::string, std::string> models = {
        {"F008", "K2 Plus"},
        {"F012", "K2 Pro"},
        {"F016", "K2 SE"},
        {"F021", "K2"},
        {"F022", "SPARKX i7"},
        {"K1", "K1"},
        {"K1 Max", "K1 Max"},
        {"K1 Max_CFS-C", "K1 Max_CFS-C"},
        {"K1 SE", "K1 SE"},
        {"K1 SE_CFS-C", "K1 SE_CFS-C"},
        {"K1C", "K1C"},
        {"K1C_CFS-C", "K1C_CFS-C"},
        {"K1_CFS-C", "K1_CFS-C"},
    };
    return models;
}

bool CrealityPrint::model_supports_multi_color(const std::string& model)
{
    return cfs_capable_models().count(model) > 0;
}

std::string CrealityPrint::model_display_name(const std::string& model)
{
    auto& names = cfs_capable_models();
    auto it = names.find(model);
    return it != names.end() ? it->second : std::string{};
}

bool CrealityPrint::supports_multi_color_print() const
{
    query_model();
    return model_supports_multi_color(m_model);
}

std::string CrealityPrint::model_name() const
{
    query_model();
    if (m_model.empty())
        return "unreachable";
    std::string name = model_display_name(m_model);
    return !name.empty() ? name : "unknown (" + m_model + ")";
}

std::string CrealityPrint::query_boxes_info(bool* no_cfs) const
{
    if (no_cfs)
        *no_cfs = false;
    try {
        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, ws_settings.port, ws_settings.connect);

        std::string result = ws_query_slots(ioc, ws, ws_settings.slots_reply, ws_settings.reply, no_cfs);

        ws_close(ioc, ws, ws_settings.close);
        return result;
    } catch (std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Failed to query boxsInfo: " << e.what();
        return {};
    }
}

CrealityPrinterInfo CrealityPrint::read_printer_info() const
{
    CrealityPrinterInfo info;
    info.reachable   = probe_info(nullptr);
    info.multi_color = info.reachable && model_supports_multi_color(m_model);
    // model_name() probes /info again when the reply names no model.
    if (info.reachable)
        info.model_name = m_model.empty() ? std::string("unknown") : model_name();
    if (info.multi_color)
        info.boxes_json = query_boxes_info(&info.no_cfs);
    return info;
}

std::vector<std::string> CrealityPrint::start_print_commands(bool                                      multi_color,
                                                             const std::string&                        gcode_path,
                                                             const std::map<std::string, std::string>& extended_info)
{
    // Build colorMatch list from the mapping provided by the dialog
    bool use_spool_holder = false;
    json color_list       = json::array();
    int  enable_self_test = 0;
    if (multi_color) {
        for (int i = 0; ; i++) {
            auto it = extended_info.find("colorMatch_" + std::to_string(i));
            if (it == extended_info.end())
                break;
            // Value format: "toolId\ttype\tcolor\tboxId\tmaterialId"
            auto val = it->second;
            std::vector<std::string> parts;
            std::istringstream iss(val);
            std::string part;
            while (std::getline(iss, part, '\t'))
                parts.push_back(part);
            if (parts.size() >= 5) {
                int box_id = std::stoi(parts[3]);
                if (box_id == 0)
                    use_spool_holder = true;
                color_list.push_back({
                    {"id", parts[0]},
                    {"type", parts[1]},
                    {"color", parts[2]},
                    {"boxId", box_id},
                    {"materialId", std::stoi(parts[4])}
                });
            }
        }
        auto it = extended_info.find("enableSelfTest");
        if (it != extended_info.end())
            enable_self_test = std::stoi(it->second);
    }

    std::vector<std::string> commands;
    // Only a mapping onto CFS slots starts a multi-color job. No mapping means the
    // printer listed no slots (no CFS attached, or boxsInfo unanswered): start from
    // the spool holder, as Creality Print does, instead of sending an empty
    // colorMatch, which makes the firmware flag "no material" for the whole job.
    if (multi_color && !use_spool_holder && !color_list.empty()) {
        json color_match = {
            {"method", "set"},
            {"params", {
                {"colorMatch", {
                    {"path", gcode_path},
                    {"list", color_list}
                }}
            }}
        };
        commands.push_back(to_string(color_match));

        json multi_color_print = {
            {"method", "set"},
            {"params", {
                {"multiColorPrint", {
                    {"gcode", gcode_path},
                    {"enableSelfTest", enable_self_test}
                }}
            }}
        };
        commands.push_back(to_string(multi_color_print));
    } else {
        json params = {{"opGcodeFile", "printprt:" + gcode_path}};
        if (multi_color)
            params["enableSelfTest"] = enable_self_test;
        json cmd = {{"method", "set"}, {"params", params}};
        commands.push_back(to_string(cmd));
    }
    return commands;
}

bool CrealityPrint::start_print(wxString &msg, const std::string &filename, const std::map<std::string, std::string>& extended_info) const
{
    try {
        // Runs before the WebSocket opens and its reply deadline starts, since it can query the
        // printer over HTTP. It also fills m_model.
        const bool        multi_color = supports_multi_color_print();
        const std::string data_root   = is_k1_family(m_model) ? "/usr/data" : "/mnt/UDISK";
        const std::string gcode_path  = data_root + "/printer_data/gcodes/" + filename;

        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, ws_settings.port, ws_settings.connect);
        beast::get_lowest_layer(ws).expires_after(ws_settings.reply);

        const std::vector<std::string> commands = start_print_commands(multi_color, gcode_path, extended_info);
        for (const std::string& cmd : commands)
            ws_send(ioc, ws, cmd);

        if (commands.size() == 1) {
            // A plain start. K1-family firmware closes the WebSocket right after accepting it,
            // so a read here surfaces a benign "End of file [asio.misc:2]" even though the print
            // already started (ws_send() delivered the command). Read best-effort, ignore errors:
            // it also keeps the close frame from following the command in the same burst.
            beast::flat_buffer buffer;
            ws_read(ioc, ws, buffer);
        }

        ws_close(ioc, ws, ws_settings.close);
        return true;
    } catch(std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Error starting print: " << e.what();
        // Shows a websocket error without Boost's source location, and a timeout the same way as an HTTP one
        if (const auto* system_error = dynamic_cast<const beast::system_error*>(&e))
            msg = system_error->code() == beast::error::timeout ? timed_out_message() : wxString::FromUTF8(system_error->code().message());
        else
            msg = wxString::FromUTF8(e.what());
        return false;
    }
}

}
