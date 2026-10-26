#ifndef slic3r_CrealityPrint_hpp_
#define slic3r_CrealityPrint_hpp_

#include <chrono>
#include <map>
#include <string>
#include <vector>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/asio/ip/address.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Http;
class CrealityPrint : public PrintHost
{
public:
    CrealityPrint(DynamicPrintConfig* config);
    ~CrealityPrint() override = default;

    const char* get_name() const override;
    virtual bool can_test() const override { return true; };
    std::string  get_host() const override;
    bool has_auto_discovery() const override { return true; }

    wxString                           get_test_ok_msg() const override;
    wxString                           get_test_failed_msg(wxString& msg) const override;
    virtual bool                       test(wxString& curl_msg) const override;
    PrintHostPostUploadActions         get_post_upload_actions() const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool supports_multi_color_print() const;

    // Single source of truth for the CFS-capable model table, shared with
    // LAN discovery (CrealityHostDiscovery). Model is the /info "model"
    // value: an F-code on the K2 platform, a literal name on K1-family.
    static bool model_supports_multi_color(const std::string& model);
    static std::string model_display_name(const std::string& model);
    std::string query_boxes_info() const;
    std::string model_name() const;
    // The WebSocket commands that start gcode_path, built from the send dialog's
    // extended_info (enableSelfTest and the colorMatch_<n> slot mapping).
    static std::vector<std::string> start_print_commands(bool                                      multi_color,
                                                         const std::string&                        gcode_path,
                                                         const std::map<std::string, std::string>& extended_info);

protected:
    virtual void set_auth(Http& http) const;

    struct WsSettings
    {
        std::string               port{"9999"};
        std::chrono::milliseconds connect{std::chrono::seconds(5)}; // Per resolved address tried, then again for the opening handshake
        std::chrono::milliseconds reply{std::chrono::seconds(10)};  // Commands and the replies read after them
        std::chrono::milliseconds close{std::chrono::seconds(3)};   // Closing handshake
    } ws_settings;

private:
    std::string m_host;
    std::string m_port;
    std::string m_apikey;
    std::string m_cafile;
    std::string m_web_ui;
    bool        m_ssl_revoke_best_effort;
    mutable std::string m_model;

    std::string make_url(const std::string& path) const;
    bool start_print(wxString& msg, const std::string& filename, const std::map<std::string, std::string>& extended_info) const;
    std::string safe_filename(const std::string& filename) const;
    void query_model() const;
};
} // namespace Slic3r

#endif