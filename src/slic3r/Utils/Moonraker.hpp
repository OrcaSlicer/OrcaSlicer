#ifndef slic3r_Moonraker_hpp_
#define slic3r_Moonraker_hpp_

#include <string>
#include <wx/string.h>
#include <wx/arrstr.h>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"


namespace Slic3r {

class DynamicPrintConfig;
class Http;

// Orca: the storage-root filtering rule Moonraker::get_storage applies, pulled out to a free
// function so it can be pinned by a test without spinning up an HTTP mock. See get_storage's doc
// for why writability alone isn't enough to offer a root as an upload destination.
namespace MoonrakerStorage {
bool is_gcode_destination_root(const std::string &root_name);
} // namespace MoonrakerStorage

// Moonraker is the JSON / WebSocket gateway that ships in front of Klipper
// (and on Klipper-API-compatible firmwares like the Prusa-Firmware-Buddy
// Buddy-Klipper fork). REST shape differs from OctoPrint: distinct paths,
// JSON body for print/start, {"result":...}/{"error":...} envelope.
//
// Endpoints used:
//   GET  /server/info                      -- connection test, reads klippy_state
//   POST /server/files/upload (multipart)  -- upload gcode (form fields: file, root)
//   POST /printer/print/start (json)       -- {"filename":"<name>.gcode"} starts print
//
// Auth: X-Api-Key header if `printhost_apikey` is non-empty; Moonraker accepts
// unauthenticated LAN access by default, so the key is optional. HTTP Basic /
// Digest are not part of the Moonraker spec and are not sent.
class Moonraker : public PrintHost
{
public:
    Moonraker(DynamicPrintConfig *config);
    ~Moonraker() override = default;

    const char* get_name() const override;

    bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }
    bool get_storage(wxArrayString &storage_path, wxArrayString &storage_name) const override;
    const std::string& get_apikey() const { return m_apikey; }
    const std::string& get_cafile() const { return m_cafile; }

protected:
    std::string m_host;
    std::string m_apikey;
    std::string m_cafile;
    bool        m_ssl_revoke_best_effort;

    void set_auth(Http &http) const;
    std::string make_url(const std::string &path) const;
    bool start_print(wxString &error_msg, const std::string &filename) const;
    // Orca: generic capability for a caller-supplied print-start script -- POSTs a fully-rendered
    // gcode script to /printer/gcode/script instead of the plain /printer/print/start call. Any
    // vendor dialect (which macro, how the mapping is embedded) is the GUI caller's business; this
    // class only knows it's "a script to run instead of the default start". See upload()'s use of
    // upload_data.extended("start_script").
    bool start_print_with_script(wxString &error_msg, const std::string &script) const;
};

}

#endif
