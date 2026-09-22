#include "CrealityPrint.hpp"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/foreach.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>

#include <curl/curl.h>
#include <wx/progdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "Bonjour.hpp"
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
using json = nlohmann::json;
using std::to_string;

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

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
            msg = format_error(body, error, status);
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
            msg = GUI::from_u8(address);
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
    // Only encode the URL path segment; keep the multipart filename and start-print path as the stored filename.
    auto url = make_url("upload/" + Http::url_encode(safe_upload_filename));

    auto  http = Http::post(url); // std::move(url));
    set_auth(http);
    if (!supports_multi_color_print())
        http.form_add("path", upload_parent_path.string());
    http.form_add_file("file", upload_data.source_path.string(), safe_upload_filename)

        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;

            if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
                wxString errormsg;
                if (!start_print(errormsg, safe_upload_filename, upload_data.extended_info)) {
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

static void ws_connect(net::io_context& ioc, websocket::stream<beast::tcp_stream>& ws,
                       const std::string& host_url, const std::string& port)
{
    std::string host = Http::get_host_from_url(host_url);

    tcp::resolver resolver{ioc};
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
    auto const results = resolver.resolve(host, port);
    beast::get_lowest_layer(ws).connect(results);
    host += ':' + std::to_string(beast::get_lowest_layer(ws).socket().remote_endpoint().port());

    ws.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) {
            req.set(http::field::user_agent,
                std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-coro");
        }));
    ws.handshake(host, "/");
    // K1-family firmware requires TEXT-mode WebSocket frames; K2-family accepts both.
    // SO_RCVTIMEO is a no-op with Boost.Asio on macOS/Linux (kqueue/epoll); use
    // expires_after() on the tcp_stream layer instead (managed per-call by the caller).
    ws.text(true);
}

static std::string ws_send_and_read(websocket::stream<beast::tcp_stream>& ws, const json& cmd, const std::string& expected_key, int max_reads = 20)
{
    ws.write(net::buffer(to_string(cmd)));

    // total caps the loop if the printer sends a continuous stream of heartbeats
    for (int reads = 0, total = 0; reads < max_reads && total < max_reads * 3; ++total) {
        beast::flat_buffer buf;
        beast::error_code ec;
        ws.read(buf, ec);
        // would_block  = SO_RCVTIMEO fired (Windows/raw sockets)
        // beast::error::timeout = expires_after() fired (Boost.Beast, macOS/Linux)
        if (ec == net::error::would_block || ec == beast::error::timeout)
            break;
        if (ec)
            throw beast::system_error{ec};
        std::string msg = beast::buffers_to_string(buf.data());
        // K1-family firmware sends periodic heartbeat pings; ack them so the
        // printer does not close the connection, then keep reading.
        if (msg.find("heart_beat") != std::string::npos) {
            beast::error_code wr_ec;
            ws.write(net::buffer(std::string("ok")), wr_ec);
            continue;
        }
        ++reads;
        if (msg.find(expected_key) != std::string::npos)
            return msg;
    }
    BOOST_LOG_TRIVIAL(warning) << "CrealityPrint: No '" << expected_key << "' response after " << max_reads << " messages";
    return {};
}

// K1-family firmware stores gcodes under /usr/data and uses a stricter WebSocket
// protocol (TEXT frames, heartbeat acks). Add verified K1-family model IDs here.
static bool is_k1_family(const std::string& model)
{
    static const std::unordered_set<std::string> k1_models = {
        "K1",
        "K1 SE",
        "K1C",
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
        {"F021", "K2"},
        {"F022", "SPARKX i7"},
        {"K1", "K1"},
        {"K1 SE", "K1 SE"},
        {"K1C", "K1C"},
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

std::string CrealityPrint::query_boxes_info() const
{
    try {
        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, "9999");

        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
        json boxs_query = {{"method", "get"}, {"params", {{"boxsInfo", 1}}}};
        std::string result = ws_send_and_read(ws, boxs_query, "boxsInfo");

        // K1 SE closes its side after responding; use the error_code overload so
        // the resulting EOF does not throw and discard the already-received result.
        beast::error_code close_ec;
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(3));
        ws.close(websocket::close_code::normal, close_ec);
        return result;
    } catch (std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Failed to query boxsInfo: " << e.what();
        return {};
    }
}

bool CrealityPrint::start_print(wxString &msg, const std::string &filename, const std::map<std::string, std::string>& extended_info) const
{
    try {
        const std::string data_root = is_k1_family(m_model) ? "/usr/data" : "/mnt/UDISK";
        const std::string gcode_path = data_root + "/printer_data/gcodes/" + filename;

        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, "9999");

        if (supports_multi_color_print()) {
            // Build colorMatch list from the mapping provided by the dialog
            bool use_spool_holder = false;
            json color_list = json::array();
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

            int enable_self_test = 0;
            {
                auto it = extended_info.find("enableSelfTest");
                if (it != extended_info.end())
                    enable_self_test = std::stoi(it->second);
            }

            if (use_spool_holder) {
                json cmd = {
                    {"method", "set"},
                    {"params", {
                        {"opGcodeFile", "printprt:" + gcode_path},
                        {"enableSelfTest", enable_self_test}
                    }}
                };
                ws.write(net::buffer(to_string(cmd)));
            } else {
                json color_match = {
                    {"method", "set"},
                    {"params", {
                        {"colorMatch", {
                            {"path", gcode_path},
                            {"list", color_list}
                        }}
                    }}
                };
                ws.write(net::buffer(to_string(color_match)));

                json multi_color_print = {
                    {"method", "set"},
                    {"params", {
                        {"multiColorPrint", {
                            {"gcode", gcode_path},
                            {"enableSelfTest", enable_self_test}
                        }}
                    }}
                };
                ws.write(net::buffer(to_string(multi_color_print)));
            }
        } else {
            json cmd = {
                {"method", "set"},
                {"params", {
                    {"opGcodeFile", "printprt:" + gcode_path}
                }}
            };
            ws.write(net::buffer(to_string(cmd)));

            // K1-family firmware closes the WebSocket right after accepting the
            // start command, so a blocking read here surfaces a benign
            // "End of file [asio.misc:2]" even though the print already started
            // (the command is delivered by write()). Read best-effort, ignore errors.
            beast::flat_buffer buffer;
            beast::error_code  read_ec;
            ws.read(buffer, read_ec);
        }

        // Same reason: the printer may have already closed the connection. A close
        // error here is not a failure — the start command was sent above.
        beast::error_code close_ec;
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(3));
        ws.close(websocket::close_code::normal, close_ec);
        return true;
    } catch(std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Error starting print: " << e.what();
        msg = wxString::FromUTF8(e.what());
        return false;
    }
}

}
