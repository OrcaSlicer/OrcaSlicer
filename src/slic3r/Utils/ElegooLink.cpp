#include "ElegooLink.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

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

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;
#define MAX_UPLOAD_PACKAGE_LENGTH 1048576 //(1024*1024)

namespace Slic3r {

    enum ElegooLinkCommand {
        //get status
        ELEGOO_GET_STATUS = 0,
        //get properties
        ELEGOO_GET_PROPERTIES = 1,
        //start print
        ELEGOO_START_PRINT = 128,
    };

    typedef enum
    {
        SDCP_PRINT_CTRL_ACK_OK = 0,  // OK
        SDCP_PRINT_CTRL_ACK_BUSY = 1 , // 设备忙 device is busy
        SDCP_PRINT_CTRL_ACK_NOT_FOUND = 2,  // 未找到目标文件 file not found
        SDCP_PRINT_CTRL_ACK_MD5_FAILED = 3,  // MD5校验失败 MD5 check failed
        SDCP_PRINT_CTRL_ACK_FILEIO_FAILED = 4,  // 文件读取失败  file I/O error
        SDCP_PRINT_CTRL_ACK_INVALID_RESOLUTION = 5, // 文件分辨率不匹配 file resolution is invalid
        SDCP_PRINT_CTRL_ACK_UNKNOW_FORMAT = 6,  // 无法识别的文件格式 file format is invalid
        SDCP_PRINT_CTRL_ACK_UNKNOW_MODEL = 7, // 文件机型不匹配 file model is invalid
    } ElegooLinkStartPrintAck;


    namespace {

        constexpr const char* ELEGOO_CC2_DEFAULT_TOKEN = "123456";
        // AppConfig section for CC2 serial numbers, keyed by normalized print_host (host/IP).
        constexpr const char* ELEGOO_DEV_SN_SECTION    = "dev_sn";

        static std::mutex                         s_sn_cache_mutex;
        static std::map<std::string, std::string> s_sn_cache;

        std::string sn_cache_key(const std::string& host_ip, const std::string& token)
        {
            return host_ip + ":" + token;
        }

        void cache_sn(const std::string& host_ip, const std::string& token, const std::string& sn)
        {
            if (host_ip.empty() || token.empty() || sn.empty())
                return;
            std::lock_guard<std::mutex> lock(s_sn_cache_mutex);
            s_sn_cache[sn_cache_key(host_ip, token)] = sn;
        }

        std::string lookup_sn(const std::string& host_ip, const std::string& token)
        {
            std::lock_guard<std::mutex> lock(s_sn_cache_mutex);
            auto it = s_sn_cache.find(sn_cache_key(host_ip, token));
            return it != s_sn_cache.end() ? it->second : std::string{};
        }

        std::string load_sn_from_config(const std::string& host_ip)
        {
            if (host_ip.empty())
                return {};
            AppConfig* app_cfg = GUI::get_app_config();
            if (app_cfg == nullptr)
                return {};
            return app_cfg->get(ELEGOO_DEV_SN_SECTION, host_ip);
        }

        void persist_sn(const std::string& host_ip, const std::string& token, const std::string& sn)
        {
            if (host_ip.empty() || sn.empty())
                return;
            cache_sn(host_ip, token, sn);
            AppConfig* app_cfg = GUI::get_app_config();
            if (app_cfg == nullptr)
                return;
            app_cfg->set_str(ELEGOO_DEV_SN_SECTION, host_ip, sn);
        }

        enum class ElegooPrinterType {
            Other,
            CC,
            CC2,
        };

        ElegooPrinterType classify_printer_model(const std::string& printer_model)
        {
            if (!boost::algorithm::starts_with(printer_model, "Elegoo Centauri"))
                return ElegooPrinterType::Other;

            const auto last_char = printer_model.find_last_not_of(" \t\r\n");
            if (last_char != std::string::npos && printer_model[last_char] == '2')
                return ElegooPrinterType::CC2;

            return ElegooPrinterType::CC;
        }

        std::string get_cc2_token(const std::string& apikey)
        {
            return apikey.empty() ? ELEGOO_CC2_DEFAULT_TOKEN : apikey;
        }

        bool parse_cc2_response(const std::string& body, std::string& error_message, std::string* serial_number = nullptr)
        {
            try {
                pt::ptree          root;
                std::istringstream is(body);
                pt::read_json(is, root);

                const int error_code = root.get<int>("error_code", -1);
                if (error_code != 0) {
                    error_message = root.get<std::string>("message", "Printer returned an error");
                    if (error_message.empty())
                        error_message = "Printer returned an error";
                    error_message += " (" + std::to_string(error_code) + ")";
                    return false;
                }

                if (serial_number != nullptr) {
                    const auto system_info = root.get_child_optional("system_info");
                    if (!system_info) {
                        error_message = "Missing system_info in response";
                        return false;
                    }

                    const auto sn = system_info->get_optional<std::string>("sn");
                    if (!sn || sn->empty()) {
                        error_message = "Missing printer serial number in response";
                        return false;
                    }
                    *serial_number = *sn;
                }

                return true;
            } catch (const std::exception&) {
                error_message = "Error parsing response";
                return false;
            }
        }

        std::string get_host_from_url(const std::string& url_in)
        {
            std::string url = url_in;
            // add http:// if there is no scheme
            size_t double_slash = url.find("//");
            if (double_slash == std::string::npos)
                url = "http://" + url;
            std::string out = url;
            CURLU* hurl = curl_url();
            if (hurl) {
                // Parse the input URL.
                CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, url.c_str(), 0);
                if (rc == CURLUE_OK) {
                    // Replace the address.
                    char* host;
                    rc = curl_url_get(hurl, CURLUPART_HOST, &host, 0);
                    if (rc == CURLUE_OK) {
                        char* port;
                        rc = curl_url_get(hurl, CURLUPART_PORT, &port, 0);
                        if (rc == CURLUE_OK && port != nullptr) {
                            out = std::string(host) + ":" + port;
                            curl_free(port);
                        } else {
                            out = host;
                            curl_free(host);
                        }
                    }
                    else
                        BOOST_LOG_TRIVIAL(error) << "ElegooLink get_host_from_url: failed to get host form URL " << url;
                }
                else
                    BOOST_LOG_TRIVIAL(error) << "ElegooLink get_host_from_url: failed to parse URL " << url;
                curl_url_cleanup(hurl);
            }
            else
                BOOST_LOG_TRIVIAL(error) << "ElegooLink get_host_from_url: failed to allocate curl_url";
            return out;
        }
    
        std::string get_host_from_url_no_port(const std::string& url_in)
        {
            std::string url = url_in;
            // add http:// if there is no scheme
            size_t double_slash = url.find("//");
            if (double_slash == std::string::npos)
                url = "http://" + url;
            std::string out = url;
            CURLU* hurl = curl_url();
            if (hurl) {
                // Parse the input URL.
                CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, url.c_str(), 0);
                if (rc == CURLUE_OK) {
                    // Replace the address.
                    char* host;
                    rc = curl_url_get(hurl, CURLUPART_HOST, &host, 0);
                    if (rc == CURLUE_OK) {
                        out = host;
                        curl_free(host);
                    }
                    else
                        BOOST_LOG_TRIVIAL(error) << "ElegooLink get_host_from_url: failed to get host form URL " << url;
                }
                else
                    BOOST_LOG_TRIVIAL(error) << "ElegooLink get_host_from_url: failed to parse URL " << url;
                curl_url_cleanup(hurl);
            }
            else
                BOOST_LOG_TRIVIAL(error) << "ElegooLink get_host_from_url: failed to allocate curl_url";
            return out;
        }

        std::string lookup_cc2_serial_impl(const std::string& printer_model,
                                           const std::string& print_host,
                                           const std::string& apikey)
        {
            if (classify_printer_model(printer_model) != ElegooPrinterType::CC2)
                return {};

            const std::string host_ip = get_host_from_url(print_host);
            const std::string token   = get_cc2_token(apikey);
            std::string       sn      = lookup_sn(host_ip, token);
            if (sn.empty())
                sn = load_sn_from_config(host_ip);
            return sn;
        }

        std::string lookup_cc2_serial(DynamicPrintConfig* config)
        {
            if (config == nullptr)
                return {};
            return lookup_cc2_serial_impl(config->opt_string("printer_model"),
                                          config->opt_string("print_host"),
                                          config->opt_string("printhost_apikey"));
        }

        #ifdef WIN32
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
        std::string substitute_host(const std::string& orig_addr, std::string sub_addr)
        {
            // put ipv6 into [] brackets 
            if (sub_addr.find(':') != std::string::npos && sub_addr.at(0) != '[')
                sub_addr = "[" + sub_addr + "]";

        #if 0
            //URI = scheme ":"["//"[userinfo "@"] host [":" port]] path["?" query]["#" fragment]
            std::string final_addr = orig_addr;
            //  http
            size_t double_dash = orig_addr.find("//");
            size_t host_start = (double_dash == std::string::npos ? 0 : double_dash + 2);
            // userinfo
            size_t at = orig_addr.find("@");
            host_start = (at != std::string::npos && at > host_start ? at + 1 : host_start);
            // end of host, could be port(:), subpath(/) (could be query(?) or fragment(#)?)
            // or it will be ']' if address is ipv6 )
            size_t potencial_host_end = orig_addr.find_first_of(":/", host_start); 
            // if there are more ':' it must be ipv6
            if (potencial_host_end != std::string::npos && orig_addr[potencial_host_end] == ':' && orig_addr.rfind(':') != potencial_host_end) {
                size_t ipv6_end = orig_addr.find(']', host_start);
                // DK: Uncomment and replace orig_addr.length() if we want to allow subpath after ipv6 without [] parentheses.
                potencial_host_end = (ipv6_end != std::string::npos ? ipv6_end + 1 : orig_addr.length()); //orig_addr.find('/', host_start));
            }
            size_t host_end = (potencial_host_end != std::string::npos ? potencial_host_end : orig_addr.length());
            // now host_start and host_end should mark where to put resolved addr
            // check host_start. if its nonsense, lets just use original addr (or  resolved addr?)
            if (host_start >= orig_addr.length()) {
                return final_addr;
            }
            final_addr.replace(host_start, host_end - host_start, sub_addr);
            return final_addr;
        #else
            // Using the new CURL API for handling URL. https://everything.curl.dev/libcurl/url
            // If anything fails, return the input unchanged.
            std::string out = orig_addr;
            CURLU *hurl = curl_url();
            if (hurl) {
                // Parse the input URL.
                CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, orig_addr.c_str(), 0);
                if (rc == CURLUE_OK) {
                    // Replace the address.
                    rc = curl_url_set(hurl, CURLUPART_HOST, sub_addr.c_str(), 0);
                    if (rc == CURLUE_OK) {
                        // Extract a string fromt the CURL URL handle.
                        char *url;
                        rc = curl_url_get(hurl, CURLUPART_URL, &url, 0);
                        if (rc == CURLUE_OK) {
                            out = url;
                            curl_free(url);
                        } else
                            BOOST_LOG_TRIVIAL(error) << "ElegooLink substitute_host: failed to extract the URL after substitution";
                    } else
                        BOOST_LOG_TRIVIAL(error) << "ElegooLink substitute_host: failed to substitute host " << sub_addr << " in URL " << orig_addr;
                } else
                    BOOST_LOG_TRIVIAL(error) << "ElegooLink substitute_host: failed to parse URL " << orig_addr;
                curl_url_cleanup(hurl);
            } else
                BOOST_LOG_TRIVIAL(error) << "ElegooLink substitute_host: failed to allocate curl_url";
            return out;
        #endif
        }
        #endif // WIN32
        std::string escape_string(const std::string& unescaped)
        {
            std::string ret_val;
            CURL* curl = curl_easy_init();
            if (curl) {
                char* decoded = curl_easy_escape(curl, unescaped.c_str(), unescaped.size());
                if (decoded) {
                    ret_val = std::string(decoded);
                    curl_free(decoded);
                }
                curl_easy_cleanup(curl);
            }
            return ret_val;
        }
        std::string escape_path_by_element(const boost::filesystem::path& path)
        {
            std::string ret_val = escape_string(path.filename().string());
            boost::filesystem::path parent(path.parent_path());
            while (!parent.empty() && parent.string() != "/") // "/" check is for case "/file.gcode" was inserted. Then boost takes "/" as parent_path.
            {
                ret_val = escape_string(parent.filename().string()) + "/" + ret_val;
                parent = parent.parent_path();
            }
            return ret_val;
        }

            std::string path_to_utf8(const boost::filesystem::path& path)
            {
        #ifdef WIN32
                return boost::nowide::narrow(path.wstring());
        #else
                return path.string();
        #endif
            }

            std::string filename_to_utf8(const boost::filesystem::path& path)
            {
        #ifdef WIN32
                return boost::nowide::narrow(path.filename().wstring());
        #else
                return path.filename().string();
        #endif
            }

    } //namespace


    ElegooLink::ElegooLink(DynamicPrintConfig *config):
    OctoPrint(config), m_printerModel(config->opt_string("printer_model")) {

    }

    std::string ElegooLink::get_print_host_webui(DynamicPrintConfig* config)
    {
        if (config == nullptr)
            return {};

        std::string fallback_webui = config->opt_string("print_host_webui");
        if (fallback_webui.empty())
            fallback_webui = config->opt_string("print_host");
        if (!fallback_webui.empty()) {
            const bool has_http_scheme = boost::algorithm::istarts_with(fallback_webui, "http");
            const bool has_file_scheme = boost::algorithm::istarts_with(fallback_webui, "file:");

            if (!has_http_scheme && !has_file_scheme)
                fallback_webui = "http://" + fallback_webui;
        }

        const std::string host = config->opt_string("print_host");
        if (host.empty())
            return fallback_webui;

        if (classify_printer_model(config->opt_string("printer_model")) != ElegooPrinterType::CC2)
            return fallback_webui;

        std::string web_path = resources_dir() + "/web/elegoolink/lan_service_web/index.html";
        std::replace(web_path.begin(), web_path.end(), '\\', '/');
        web_path = "file://" + web_path;

        const std::string token   = get_cc2_token(config->opt_string("printhost_apikey"));
        const std::string host_ip = get_host_from_url(host);

        // Pass sn= so the panel can subscribe to the correct MQTT topics.
        std::string sn = lookup_cc2_serial(config);
        if (sn.empty()) {
            std::string error_msg;
            auto http = Http::get("http://" + host_ip + "/system/info?X-Token=" + escape_string(token));
            http.timeout_connect(3).timeout_max(5);
            http.header("X-Token", token);
            http.header("Accept", "application/json");
            http.on_complete([&](std::string body, unsigned /*status*/) {
                parse_cc2_response(body, error_msg, &sn);
            }).perform_sync();
            if (!sn.empty())
                persist_sn(host_ip, token, sn);
        }

        web_path += "?access_code=" + token;
        web_path += "&ip=" + host_ip;
        if (!sn.empty())
            web_path += "&sn=" + sn;
        web_path += "&id=elegoo_123456";

        const std::string lang = GUI::wxGetApp().current_language_code_safe().utf8_string();
        if (!lang.empty())
            web_path += "&lang=" + lang;

        if (GUI::get_app_config()->get_bool("developer_mode"))
            web_path += "&dev=true";

        return web_path;
    }

    std::string ElegooLink::cc2_token() const
    {
        return get_cc2_token(m_apikey);
    }

    std::string ElegooLink::make_cc2_info_url() const
    {
        return make_url("system/info?X-Token=" + escape_string(cc2_token()));
    }

    std::string ElegooLink::make_cc2_upload_url() const
    {
        return make_url("upload");
    }

    const char* ElegooLink::get_name() const { return "ElegooLink"; }
    PrintHostPostUploadActions ElegooLink::get_post_upload_actions() const
    {
        if (classify_printer_model(m_printerModel) == ElegooPrinterType::CC2) {
            return PrintHostPostUploadAction::None;
        } else {
            return PrintHostPostUploadAction::StartPrint;
        }
    }

    std::string ElegooLink::get_sn() const
    {
        // Panel IPC calls this on every load with a 10s timeout. Never block on HTTP
        // here — URL sn= and dev_sn must be enough; HTTP is only for get_print_host_webui.
        return lookup_cc2_serial_impl(m_printerModel, m_host, m_apikey);
    }

    bool ElegooLink::elegoo_test(wxString& msg) const{

    const char *name = get_name();
    bool res = true;
    auto url = make_url("");
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;
            // Check if the response contains "ELEGOO" in any case.
            // This is a simple check to see if the response is from an ElegooLink server.
            std::regex re("ELEGOO", std::regex::icase);
            std::smatch match;
            if (std::regex_search(body, match, re)) {
                res = true;
            } else {
                msg = format_error(body, "ElegooLink not detected", 0);
                res = false;
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
    bool ElegooLink::test(wxString &curl_msg) const{
        switch (classify_printer_model(m_printerModel)) {
        case ElegooPrinterType::Other:
            return OctoPrint::test(curl_msg);
        case ElegooPrinterType::CC2:
            return elegoo_cc2_test(curl_msg);
        case ElegooPrinterType::CC:
            return elegoo_test(curl_msg);
        }
        return false;
    }

    bool ElegooLink::elegoo_cc2_test(wxString& msg) const
    {
        const char*  name  = get_name();
        bool         res   = true;
        const auto   token = cc2_token();
        auto         url   = make_cc2_info_url();
        auto         http  = Http::get(std::move(url));

        http.header("X-Token", token);
        http.header("Accept", "application/json");
        http.on_error([&](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting CC2 device info: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
                res = false;
                if (status == 401 || status == 403)
                    msg = format_error(body, "Invalid access code", status);
                else
                    msg = format_error(body, error.empty() ? "CC2 device not detected" : error, status);
            })
            .on_complete([&](std::string body, unsigned status) {
                BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got CC2 device info: %2%") % name % body;
                std::string error_message;
                std::string serial_number;
                if (!parse_cc2_response(body, error_message, &serial_number)) {
                    res = false;
                    msg = format_error(body, error_message.empty() ? "CC2 device not detected" : error_message, status);
                    return;
                }
                persist_sn(get_host_from_url(m_host), token, serial_number);
                res = true;
            })
#ifdef WIN32
            .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
            .on_ip_resolve([&](std::string address) {
                msg = GUI::from_u8(address);
            })
#endif // WIN32
            .perform_sync();
        return res;
    }

#ifdef WIN32
    bool ElegooLink::elegoo_test_with_resolved_ip(wxString& msg) const
    {
        // Since the request is performed synchronously here,
        // it is ok to refer to `msg` from within the closure
        const char* name = get_name();
        bool        res  = true;
        // Msg contains ip string.
        auto url = substitute_host(make_url(""), GUI::into_u8(msg));
        msg.Clear();
        std::string host = get_host_from_url(m_host);
        auto        http = Http::get(url); // std::move(url));
        // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
        // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
        // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
        // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse
        // proxy is used (issue #9734). Also when allow_ip_resolve = 0, this is not needed, but it should not break anything if it stays.
        // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
        http.header("Host", host);
        set_auth(http);
        http.on_error([&](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version at %2% : %3%, HTTP %4%, body: `%5%`") % name % url %
                                                error % status % body;
                res = false;
                msg = format_error(body, error, status);
            })
            .on_complete([&, this](std::string body, unsigned) {
                // Check if the response contains "ELEGOO" in any case.
                // This is a simple check to see if the response is from an ElegooLink server.
                std::regex  re("ELEGOO", std::regex::icase);
                std::smatch match;
                if (std::regex_search(body, match, re)) {
                    res = true;
                } else {
                    msg = format_error(body, "ElegooLink not detected", 0);
                    res = false;
                }
            })
            .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
            .perform_sync();

        return res;
    }
    bool ElegooLink::test_with_resolved_ip(wxString& msg) const
    {
        switch (classify_printer_model(m_printerModel)) {
        case ElegooPrinterType::Other:
            return OctoPrint::test_with_resolved_ip(msg);
        case ElegooPrinterType::CC2:
            return elegoo_cc2_test_with_resolved_ip(msg);
        case ElegooPrinterType::CC:
            return elegoo_test_with_resolved_ip(msg);
        }
        return false;
    }

    bool ElegooLink::elegoo_cc2_test_with_resolved_ip(wxString& msg) const
    {
        const char*  name        = get_name();
        bool         res         = true;
        const auto   token       = cc2_token();
        auto         url         = substitute_host(make_cc2_info_url(), GUI::into_u8(msg));
        std::string  host_header = get_host_from_url(m_host);
        auto         http        = Http::get(url);
        msg.Clear();

        http.header("Host", host_header);
        http.header("X-Token", token);
        http.header("Accept", "application/json");
        http.on_error([&](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting CC2 device info at %2% : %3%, HTTP %4%, body: `%5%`") % name % url %
                                                error % status % body;
                res = false;
                if (status == 401 || status == 403)
                    msg = format_error(body, "Invalid access code", status);
                else
                    msg = format_error(body, error.empty() ? "CC2 device not detected" : error, status);
            })
            .on_complete([&](std::string body, unsigned status) {
                std::string error_message;
                std::string serial_number;
                if (!parse_cc2_response(body, error_message, &serial_number)) {
                    res = false;
                    msg = format_error(body, error_message.empty() ? "CC2 device not detected" : error_message, status);
                    return;
                }
                res = true;
            })
            .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
            .perform_sync();

        return res;
    }
#endif // WIN32


    wxString ElegooLink::get_test_ok_msg() const
    {
        return _L("Connection to ElegooLink is working correctly.");
    }

    wxString ElegooLink::get_test_failed_msg(wxString& msg) const
    {
        return GUI::format_wxstr("%s: %s", _L("Could not connect to ElegooLink"), msg);
    }

    #ifdef WIN32
    bool ElegooLink::upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const
    {
        const auto printer_type = classify_printer_model(m_printerModel);

        if (printer_type == ElegooPrinterType::Other)
            return OctoPrint::upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr);

        info_fn(L"resolve", boost::nowide::widen(resolved_addr.to_string()));

        if (printer_type == ElegooPrinterType::CC2) {
            wxString cc2_msg = GUI::from_u8(resolved_addr.to_string());
            if (!elegoo_cc2_test_with_resolved_ip(cc2_msg)) {
                error_fn(std::move(cc2_msg));
                return false;
            }

            std::string url = substitute_host(make_cc2_upload_url(), resolved_addr.to_string());
            info_fn(L"resolve", boost::nowide::widen(url));
            return loopUploadCC2(url, get_host_from_url(m_host), std::move(upload_data), prorgess_fn, error_fn, info_fn);
        }

        wxString legacy_msg = GUI::from_u8(resolved_addr.to_string());
        if (!elegoo_test_with_resolved_ip(legacy_msg)) {
            error_fn(std::move(legacy_msg));
            return false;
        }

        std::string url = substitute_host(make_url("uploadFile/upload"), resolved_addr.to_string());
        info_fn(L"resolve", boost::nowide::widen(url));

        bool res = loopUpload(url, upload_data, prorgess_fn, error_fn, info_fn);
        return res;
    }
    #endif //WIN32

    bool ElegooLink::upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
    {
        const auto printer_type = classify_printer_model(m_printerModel);

        if (printer_type == ElegooPrinterType::Other)
            return OctoPrint::upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);

        if (printer_type == ElegooPrinterType::CC2) {
            wxString cc2_msg;
            if (!elegoo_cc2_test(cc2_msg)) {
                error_fn(std::move(cc2_msg));
                return false;
            }

            std::string url;
#ifdef WIN32
            if (m_host.find("https://") == 0 || cc2_msg.empty() || !GUI::get_app_config()->get_bool("allow_ip_resolve"))
#endif // _WIN32
            {
                url = make_cc2_upload_url();
            }
#ifdef WIN32
            else {
                info_fn(L"resolve", cc2_msg);
                url = substitute_host(make_cc2_upload_url(), GUI::into_u8(cc2_msg));
                BOOST_LOG_TRIVIAL(info) << "CC2 upload address after ip resolve: " << url;
            }
#endif // _WIN32

            return loopUploadCC2(url, get_host_from_url(m_host), std::move(upload_data), prorgess_fn, error_fn, info_fn);
        }

        wxString legacy_msg;
        if(!elegoo_test(legacy_msg)){
            error_fn(std::move(legacy_msg));
            return false;
        }

        std::string url;
    #ifdef WIN32
        // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
        if (m_host.find("https://") == 0 || legacy_msg.empty() || !GUI::get_app_config()->get_bool("allow_ip_resolve"))
    #endif // _WIN32
        {
            // If https is entered we assume signed ceritificate is being used
            // IP resolving will not happen - it could resolve into address not being specified in cert
            url = make_url("uploadFile/upload");
        }
    #ifdef WIN32
        else {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Curl uses easy_getinfo to get ip address of last successful transaction.
            // If it got the address use it instead of the stored in "host" variable.
            // This new address returns in "test_msg_or_host_ip" variable.
            // Solves troubles of uploades failing with name address.
            // in original address (m_host) replace host for resolved ip 
            info_fn(L"resolve", legacy_msg);
            url = substitute_host(make_url("uploadFile/upload"), GUI::into_u8(legacy_msg));
            BOOST_LOG_TRIVIAL(info) << "Upload address after ip resolve: " << url;
        }
    #endif // _WIN32

        bool res = loopUpload(url, upload_data, prorgess_fn, error_fn, info_fn);
        return res;
    }

    bool ElegooLink::validate_version_text(const boost::optional<std::string> &version_text) const
    {
        return  true;
    }
    bool ElegooLink::uploadPart(Http &http,std::string md5,std::string uuid,std::string path,
                                std::string filename,size_t filesize,size_t offset,size_t length,
                                ProgressFn  prorgess_fn,ErrorFn error_fn,InfoFn info_fn)const
    {
        const char* name   = get_name();
        bool        result = false;
        http.form_clear();
        http.form_add("Check", "1")
            .form_add("S-File-MD5", md5)
            .form_add("Offset", std::to_string(offset))
            .form_add("Uuid", uuid)
            .form_add("TotalSize", std::to_string(filesize))
            .form_add_file("File", path, filename, offset, length)
            .on_complete([&](std::string body, unsigned status) {
                BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
                if (status == 200) {
                    try {
                        pt::ptree          root;
                        std::istringstream is(body);
                        pt::read_json(is, root);
                        std::string code = root.get<std::string>("code");
                        if (code == "000000") {
                            // info_fn(L"complete", wxString());
                            result = true;
                        } else {
                            // get error messages
                            pt::ptree   messages      = root.get_child("messages");
                            std::string error_message = "ErrorCode : " + code + "\n";
                            for (pt::ptree::value_type& message : messages) {
                                std::string field = message.second.get<std::string>("field");
                                std::string msg   = message.second.get<std::string>("message");
                                error_message += field + ":" + msg + "\n";
                            }
                            error_fn(wxString::FromUTF8(error_message));
                        }
                    } catch (...) {
                        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error parsing response: %2%") % name % body;
                        error_fn(wxString::FromUTF8("Error parsing response"));
                    }
                } else {
                    error_fn(format_error(body, "upload failed", status));
                }
            })
            .on_error([&](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status %
                                                body;
                error_fn(format_error(body, error, status));
            })
            .on_progress([&](Http::Progress progress, bool& cancel) {
                // If upload is finished, do not call progress_fn
                // on_complete will be called after some time, so we do not need to call it here
                // Because some devices will call on_complete after the upload progress reaches 100%,
                // so we need to process it here, based on on_complete
                if (progress.ultotal == progress.ulnow) {
                    // Upload is finished
                    return;
                }
                prorgess_fn(std::move(progress), cancel);
                if (cancel) {
                    // Upload was canceled
                    BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                }
            })
#ifdef WIN32
            .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
            .perform_sync();
        return result;
    }

    bool ElegooLink::loopUpload(std::string url, PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
    {
        const char* name               = get_name();
        const auto  upload_filename = filename_to_utf8(upload_data.upload_path);
        std::string source_path     = path_to_utf8(upload_data.source_path);

        // calc file size
        boost::nowide::ifstream file(source_path, std::ios::binary | std::ios::ate);
        std::streamsize size = file.tellg();
        file.close();
        const std::string fileSize = std::to_string(size);

        // generate uuid
        boost::uuids::random_generator generator;
        boost::uuids::uuid             uuid        = generator();
        std::string                    uuid_string = to_string(uuid);

        std::string md5;
        bbl_calc_md5(source_path, md5);

        auto        http   = Http::post(url);
#ifdef WIN32
        // "Host" header is necessary here. In the workaround above (two mDNS..) we have got IP address from test connection and subsituted
        // it into "url" variable. And when creating Http object above, libcurl automatically includes "Host" header from address it got.
        // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back. Not changing the host would work
        // on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734). Also
        // when allow_ip_resolve = 0, this is not needed, but it should not break anything if it stays.
        // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
        std::string host = get_host_from_url(m_host);
        http.header("Host", host);
        http.header("Accept", "application/json, text/plain, */*");
#endif // _WIN32
        set_auth(http);

        bool      res          = false;
        const int packageCount = (size + MAX_UPLOAD_PACKAGE_LENGTH - 1) / MAX_UPLOAD_PACKAGE_LENGTH;

        for (size_t i = 0; i < packageCount; i++) {
            BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2%/%3%") % name % (i + 1) % packageCount;
            const size_t offset = MAX_UPLOAD_PACKAGE_LENGTH * i;
            size_t       length = MAX_UPLOAD_PACKAGE_LENGTH;
            // if it is the last package, the length is the remainder of the file size divided by MAX_UPLOAD_PACKAGE_LENGTH
            if ((i == packageCount - 1) && (size % MAX_UPLOAD_PACKAGE_LENGTH > 0)) {
                length = size % MAX_UPLOAD_PACKAGE_LENGTH;
            }
            res = uploadPart(
                http, md5, uuid_string, source_path, upload_filename, size, offset, length,
                [size, i, progress_fn](Http::Progress progress, bool& cancel) {
                    Http::Progress p(0, 0, size, i * MAX_UPLOAD_PACKAGE_LENGTH + progress.ulnow, progress.buffer);
                    progress_fn(p, cancel);
                },
                error_fn, info_fn);
            if (!res) {
                break;
            }
        }

        if (res) {
            if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
                // connect to websocket, since the upload is successful, the file will be printed
                std::string     wsUrl = get_host_from_url_no_port(m_host);
                WebSocketClient client;
                try {
                    client.connect(wsUrl, "3030", "/websocket");
                } catch (std::exception& e) {
                    const auto errorString = std::string(e.what());
                    if (errorString.find("The WebSocket handshake was declined by the remote peer") != std::string::npos) {
                        // error_fn(
                        //     _L("The number of printer connections has exceeded the limit. Please disconnect other connections, restart the "
                        //        "printer and slicer, and then try again."));
                        error_fn(_L("The file has been transferred, but some unknown errors occurred. Please check the device page for the file and try to start printing again."));
                    } else {
                        error_fn(std::string("\n") + wxString::FromUTF8(e.what()));
                    }
                    return false;
                }
                std::string timeLapse = "0";
                std::string heatedBedLeveling = "0";
                std::string bedType           = "0";
                timeLapse         = upload_data.extended_info["timeLapse"];
                heatedBedLeveling = upload_data.extended_info["heatedBedLeveling"];
                bedType           = upload_data.extended_info["bedType"];
                
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (checkResult(client, error_fn)) {
                    // send print command
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    res = print(client, timeLapse, heatedBedLeveling, bedType, upload_filename, error_fn);
                }else{
                    res = false;
                }
            }
        }
        return res;
    }

    bool ElegooLink::uploadPartCC2(Http&                           http,
                                   const std::string&             host_header,
                                   const std::string&             token,
                                   const std::string&             md5,
                                   const boost::filesystem::path& path,
                                   const std::string&             filename,
                                   size_t                         filesize,
                                   size_t                         offset,
                                   size_t                         length,
                                   ProgressFn                     prorgess_fn,
                                   ErrorFn                        error_fn) const
    {
        const char* name = get_name();

        boost::nowide::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            error_fn(_L("Failed to open file for upload."));
            return false;
        }

        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        std::string chunk(length, '\0');
        file.read(chunk.data(), static_cast<std::streamsize>(length));
        if (!file && static_cast<size_t>(file.gcount()) != length) {
            error_fn(_L("Failed to read file chunk for upload."));
            return false;
        }
        chunk.resize(static_cast<size_t>(file.gcount()));

        const size_t end_offset = offset + chunk.size() - 1;
        const auto   range      = std::string("bytes ") + std::to_string(offset) + "-" + std::to_string(end_offset) + "/" + std::to_string(filesize);

        bool result = false;
        http.headers_reset();
        if (!host_header.empty())
            http.header("Host", host_header);
        http.header("Accept", "application/json")
            .header("Content-Type", "application/octet-stream")
            .header("Content-Length", std::to_string(chunk.size()))
            .header("Content-Range", range)
            .header("X-File-Name", filename)
            .header("X-File-MD5", md5)
            .header("X-Token", token)
            .set_post_body(chunk)
            .on_complete([&](std::string body, unsigned status) {
                BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: CC2 chunk uploaded: HTTP %2%: %3%") % name % status % body;
                std::string error_message;
                if (!parse_cc2_response(body, error_message)) {
                    error_fn(format_error(body, error_message.empty() ? "CC2 upload failed" : error_message, status));
                    return;
                }
                result = true;
            })
            .on_error([&](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading CC2 chunk: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
                if (status == 401 || status == 403)
                    error_fn(format_error(body, "Invalid access code", status));
                else
                    error_fn(format_error(body, error.empty() ? "CC2 upload failed" : error, status));
            })
            .on_progress([&](Http::Progress progress, bool& cancel) {
                if (progress.ultotal == progress.ulnow)
                    return;
                prorgess_fn(std::move(progress), cancel);
                if (cancel)
                    BOOST_LOG_TRIVIAL(info) << name << ": CC2 upload canceled";
            })
#ifdef WIN32
            .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
            .perform_sync();

        return result;
    }

    bool ElegooLink::loopUploadCC2(std::string           url,
                                   const std::string&    host_header,
                                   PrintHostUpload       upload_data,
                                   ProgressFn            progress_fn,
                                   ErrorFn               error_fn,
                                   InfoFn                info_fn) const
    {
        BOOST_LOG_TRIVIAL(info) << get_name() << ": Uploading file to Elegoo CC2";
        const auto  upload_filename = filename_to_utf8(upload_data.upload_path);
        std::string source_path     = path_to_utf8(upload_data.source_path);
        std::string md5;
        bbl_calc_md5(source_path, md5);
        std::transform(md5.begin(), md5.end(), md5.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        boost::nowide::ifstream file(source_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            error_fn(_L("Failed to open file for upload."));
            return false;
        }

        const std::streamsize size = file.tellg();
        file.close();
        if (size <= 0) {
            error_fn(_L("The file is empty or could not be read."));
            return false;
        }

        if (md5.empty()) {
            error_fn(_L("Failed to calculate file checksum."));
            return false;
        }

        const std::string token        = cc2_token();
        const int         packageCount = static_cast<int>((size + MAX_UPLOAD_PACKAGE_LENGTH - 1) / MAX_UPLOAD_PACKAGE_LENGTH);
        auto              http         = Http::put2(url);
        http.timeout_connect(30)
            .timeout_max(180);

        bool              res          = false;
        for (int i = 0; i < packageCount; ++i) {
            const size_t offset = MAX_UPLOAD_PACKAGE_LENGTH * static_cast<size_t>(i);
            size_t       length = MAX_UPLOAD_PACKAGE_LENGTH;
            if (i == packageCount - 1 && size % MAX_UPLOAD_PACKAGE_LENGTH > 0)
                length = static_cast<size_t>(size % MAX_UPLOAD_PACKAGE_LENGTH);

            res = uploadPartCC2(
                http, host_header, token, md5, source_path, upload_filename, static_cast<size_t>(size), offset, length,
                [size, i, progress_fn](Http::Progress progress, bool& cancel) {
                    const size_t uploaded = static_cast<size_t>(i) * MAX_UPLOAD_PACKAGE_LENGTH + progress.ulnow;
                    Http::Progress merged(0, 0, static_cast<size_t>(size), std::min(static_cast<size_t>(size), uploaded), progress.buffer,
                                          progress.upload_spd);
                    progress_fn(merged, cancel);
                },
                error_fn);
            if (!res)
                break;
        }

        if (res && upload_data.post_action == PrintHostPostUploadAction::StartPrint)
            BOOST_LOG_TRIVIAL(info) << get_name() << ": CC2 upload completed; start print is not supported.";

        (void) info_fn;
        return res;
    }

    bool ElegooLink::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
    {
    #ifndef WIN32
        return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
    #else
        std::string host = get_host_from_url(m_host);

        // decide what to do based on m_host - resolve hostname or upload to ip
        std::vector<boost::asio::ip::address> resolved_addr;
        boost::system::error_code ec;
        boost::asio::ip::address host_ip = boost::asio::ip::make_address(host, ec);
        if (!ec) {
            resolved_addr.push_back(host_ip);
        } else if ( GUI::get_app_config()->get_bool("allow_ip_resolve") && boost::algorithm::ends_with(host, ".local")){
            Bonjour("octoprint")
                .set_hostname(host)
                .set_retries(5) // number of rounds of queries send
                .set_timeout(1) // after each timeout, if there is any answer, the resolving will stop
                .on_resolve([&ra = resolved_addr](const std::vector<BonjourReply>& replies) {
                    for (const auto & rpl : replies) {
                        boost::asio::ip::address ip(rpl.ip);
                        ra.emplace_back(ip);
                        BOOST_LOG_TRIVIAL(info) << "Resolved IP address: " << rpl.ip;
                    }
                })
                .resolve_sync();
        }
        if (resolved_addr.empty()) {
            // no resolved addresses - try system resolving
            BOOST_LOG_TRIVIAL(error) << "ElegooLink failed to resolve hostname " << m_host << " into the IP address. Starting upload with system resolving.";
            return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
        } else if (resolved_addr.size() == 1) {
            // one address resolved - upload there
            return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr.front());
        }  else if (resolved_addr.size() == 2 && resolved_addr[0].is_v4() != resolved_addr[1].is_v4()) {
            // there are just 2 addresses and 1 is ip_v4 and other is ip_v6
            // try sending to both. (Then if both fail, show both error msg after second try)
            wxString error_message;
            if (!upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn
                , [&msg = error_message, resolved_addr](wxString error) { msg = GUI::format_wxstr("%1%: %2%", resolved_addr.front(), error); }
                , info_fn, resolved_addr.front())
                &&
                !upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn
                , [&msg = error_message, resolved_addr](wxString error) { msg += GUI::format_wxstr("\n%1%: %2%", resolved_addr.back(), error); }
                , info_fn, resolved_addr.back())
                ) {

                error_fn(error_message);
                return false;
            }
            return true;
        } else {
            // There are multiple addresses - user needs to choose which to use.
            size_t selected_index = resolved_addr.size(); 
            IPListDialog dialog(nullptr, boost::nowide::widen(m_host), resolved_addr, selected_index);
            if (dialog.ShowModal() == wxID_OK && selected_index < resolved_addr.size()) {    
                return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr[selected_index]);
            }
        }
        return false;
    #endif // WIN32
    }

    bool ElegooLink::print(WebSocketClient&  client,
                           std::string       timeLapse,
                           std::string       heatedBedLeveling,
                           std::string       bedType,
                           const std::string filename,
                           ErrorFn           error_fn) const
    {

        // convert bedType to 0 or 1, 0 is PTE, 1 is PC
        if (bedType == std::to_string((int)BedType::btPC)){
            bedType = "1";
        }else{
            bedType = "0";
        }
        bool res = false;
        // create a random UUID generator
        boost::uuids::random_generator generator;
        // generate a UUID
        boost::uuids::uuid uuid = generator();
        std::string uuid_string = to_string(uuid);
        try {
            std::string requestID = uuid_string; 
            auto now = std::chrono::system_clock::now();
            auto duration = now.time_since_epoch();
            auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
            std::string timestamp = std::to_string(milliseconds);     
            std::string jsonString = R"({
                                        "Id":"",
                                        "Data":{
                                            "Cmd":)"+std::to_string(ElegooLinkCommand::ELEGOO_START_PRINT)+R"(,
                                            "Data":{
                                                "Filename":"/local/)" + filename + R"(",
                                                "StartLayer":0,
                                                "Calibration_switch":)" + heatedBedLeveling + R"(,
                                                "PrintPlatformType":)" + bedType + R"(,
                                                "Tlp_Switch":)" + timeLapse + R"(
                                            },
                                            "RequestID":")"+ uuid_string + R"(",
                                            "MainboardID":"",
                                            "TimeStamp":)" + timestamp + R"(,
                                            "From":1
                                        }
                                    })";
                    std::cout << "send: " << jsonString << std::endl;
                    BOOST_LOG_TRIVIAL(info) << "start print, param: " << jsonString;
                    client.send(jsonString);
                    // wait 30s
                    auto start_time = std::chrono::steady_clock::now();
                    do{
                        std::string response = client.receive();
                        std::cout << "Received: " << response << std::endl;
                        BOOST_LOG_TRIVIAL(info) << "Received: " << response;

                        //sample response
                        // {"Id":"979d4C788A4a78bC777A870F1A02867A","Data":{"Cmd":128,"Data":{"Ack":1},"RequestID":"5223de52cc7642ae8d7924f9dd46f6ad","MainboardID":"1c7319d30105041800009c0000000000","TimeStamp":1735032553},"Topic":"sdcp/response/1c7319d30105041800009c0000000000"}  
                        pt::ptree root;
                        std::istringstream is(response);
                        pt::read_json(is, root);

                        auto data = root.get_child_optional("Data");
                        if(!data){
                            BOOST_LOG_TRIVIAL(info) << "wait for start print response";
                            continue;
                        }
                        auto cmd = data->get_optional<int>("Cmd");
                        if(!cmd){
                            BOOST_LOG_TRIVIAL(info) << "wait for start print response";
                            continue;
                        }
                        if(*cmd == ElegooLinkCommand::ELEGOO_START_PRINT){
                            auto _ack = data->get_optional<int>("Data.Ack");
                            if(!_ack){
                                BOOST_LOG_TRIVIAL(error) << "start print failed, ack not found";
                                error_fn(_L("Error code not found"));
                                break;
                            }
                            auto ack = static_cast<ElegooLinkStartPrintAck>(*_ack);
                            if(ack == ElegooLinkStartPrintAck::SDCP_PRINT_CTRL_ACK_OK){
                                res = true;
                            }else{
                                res = false;
                                BOOST_LOG_TRIVIAL(error) << "start print failed: ack: " << ack;
                                wxString error_message = "";
                                switch(ack){
                                    case ElegooLinkStartPrintAck::SDCP_PRINT_CTRL_ACK_BUSY:
                                    {   
                                        error_message =_L("The printer is busy, Please check the device page for the file and try to start printing again.");
                                        break;
                                    }
                                    case ElegooLinkStartPrintAck::SDCP_PRINT_CTRL_ACK_NOT_FOUND:
                                    {
                                        error_message =_(L("The file is lost, please check and try again."));
                                        break;
                                    }
                                    case ElegooLinkStartPrintAck::SDCP_PRINT_CTRL_ACK_MD5_FAILED:
                                    {
                                        error_message =_(L("The file is corrupted, please check and try again."));
                                        break;
                                    }
                                    case ElegooLinkStartPrintAck::SDCP_PRINT_CTRL_ACK_FILEIO_FAILED:
                                    case ElegooLinkStartPrintAck::SDCP_PRINT_CTRL_ACK_INVALID_RESOLUTION:
                                    case ElegooLinkStartPrintAck::SDCP_PRINT_CTRL_ACK_UNKNOW_FORMAT:
                                    {
                                        error_message =_(L("Transmission abnormality, please check and try again."));
                                        break;
                                    }
                                    case ElegooLinkStartPrintAck::SDCP_PRINT_CTRL_ACK_UNKNOW_MODEL:
                                    {
                                        error_message =_(L("The file does not match the printer, please check and try again."));
                                        break;
                                    }
                                    default:
                                    {
                                        error_message =_L("Unknown error");
                                        break;
                                    }
                                }

                                error_message += " " + wxString::Format(_L("Error code: %d"),ack);
                                error_fn(error_message);
                            }
                            break;
                        }
                    } while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(30));
                    if (std::chrono::steady_clock::now() - start_time >= std::chrono::seconds(30)) {
                        res = false;
                        error_fn(_L("Start print timeout"));
                    }
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
                BOOST_LOG_TRIVIAL(error) << "start print error: " << e.what();
                error_fn(_L("Start print failed") +"\n" +GUI::from_u8(e.what()));
                res=false;
            }
        return res;
    }

    bool ElegooLink::checkResult(WebSocketClient& client, ErrorFn error_fn) const
    {
        bool res = true;
        // create a random UUID generator
        boost::uuids::random_generator generator;
        // generate a UUID
        boost::uuids::uuid uuid        = generator();
        std::string        uuid_string = to_string(uuid);
        try {
            std::string requestID    = uuid_string;
            auto        now          = std::chrono::system_clock::now();
            auto        duration     = now.time_since_epoch();
            auto        milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
            std::string timestamp    = std::to_string(milliseconds);
            std::string jsonString   = R"({
                                        "Id":"",
                                        "Data":{
                                            "Cmd":)" +
                                     std::to_string(ElegooLinkCommand::ELEGOO_GET_STATUS) + R"(,
                                            "Data":{},
                                            "RequestID":")" +
                                     uuid_string + R"(",
                                            "MainboardID":"",
                                            "TimeStamp":)" +
                                     timestamp + R"(,
                                            "From":1
                                        }
                                    })";
            std::cout << "send: " << jsonString << std::endl;
            BOOST_LOG_TRIVIAL(info) << "start print, param: " << jsonString;
            bool needWrite = true;
            // wait 60s
            auto start_time = std::chrono::steady_clock::now();
            do {
                if (needWrite) {
                    client.send(jsonString);
                    needWrite = false;
                }
                std::string response = client.receive();
                std::cout << "Received: " << response << std::endl;
                BOOST_LOG_TRIVIAL(info) << "Received: " << response;
                pt::ptree          root;
                std::istringstream is(response);
                pt::read_json(is, root);
                auto status = root.get_child_optional("Status");
                if (status) {
                    auto currentStatus = status->get_child_optional("CurrentStatus");
                    if (currentStatus) {
                        std::vector<int> status;
                        for (auto& item : *currentStatus) {
                            status.push_back(item.second.get_value<int>());
                        }
                        if (std::find(status.begin(), status.end(), 8) != status.end()) {
                            // 8 is check file status, need to wait
                            needWrite = true;
                            // sleep 1s
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }
            } while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(60));
            if (std::chrono::steady_clock::now() - start_time >= std::chrono::seconds(60)) {
                res = false;
                error_fn(_L("Start print timeout"));
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            BOOST_LOG_TRIVIAL(error) << "start print error: " << e.what();
            error_fn(_L("Start print failed") + "\n" + GUI::from_u8(e.what()));
            res = false;
        }
        return res;
    }
    }
