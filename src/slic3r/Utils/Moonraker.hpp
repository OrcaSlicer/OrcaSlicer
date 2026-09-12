#ifndef slic3r_Moonraker_hpp_
#define slic3r_Moonraker_hpp_

#include <string>
#include <vector>
#include <wx/string.h>
#include <wx/arrstr.h>

#include <boost/filesystem/path.hpp>
#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"


namespace Slic3r {

class DynamicPrintConfig;
class Http;

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
// ORCA-CFS: A single slot in a Creality Filament System (CFS) unit as reported by the
// printer's Klipper firmware over Moonraker. `slot_id` is 0-based and maps directly to the
// tool index the slicer emits at colour changes (T0..T3 for a single 4-slot unit; chained
// units extend the range). Populated by Moonraker::fetch_cfs_slots().
struct CFSSlot
{
    int         slot_id {0};
    bool        has_filament {false};
    std::string material_name;   // e.g. "PLA", "PETG"  (empty when slot is empty)
    std::string material_color;  // hex RGB without '#', e.g. "ffffff" (empty when unknown)
    int         unit_id {1};     // CFS box/unit index (T1..T4); units chain for >4 colours
    std::string label;           // human slot label as reported by firmware, e.g. "T1A".."T1D"
    int         remain {-1};     // remaining filament %, -1 if unknown (near-empty warning)
    std::string material_code;   // Creality filamentId, e.g. "103001" (empty if not reported)
};

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

    // ORCA-CFS: Detect whether the connected Klipper/Moonraker host exposes a Creality Filament
    // System object and, if so, read its slots. Returns false on transport error (msg set).
    // On success, *supports_cfs reports whether a CFS object was found; `slots` is filled only
    // when it was. Used by the send dialog to show the filament->slot mapping UI (gated on
    // *supports_cfs), mirroring the Flashforge IFS material-station flow.
    bool fetch_cfs_slots(std::vector<CFSSlot>& slots, bool* supports_cfs, wxString& msg) const;
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

    // ORCA-CFS helpers.
    // query_object(): GET /printer/objects/query?<object> -> raw JSON body.
    // detect_cfs_object(): GET /printer/objects/list and return the CFS object name if present.
    // parse_cfs_slots(): turn the queried object body into CFSSlot[] -- the ONLY place that
    //   depends on the firmware-specific field names captured by cfs-moonraker-probe.sh.
    bool query_object(const std::string& object, std::string& body_out, wxString& msg) const;
    bool detect_cfs_object(std::string& object_name_out, wxString& msg) const;
    bool parse_cfs_slots(const std::string& object_name, const std::string& body, std::vector<CFSSlot>& slots) const;
    // apply_cfs_tool_mapping(): rewrite standalone Tn tool-change lines in the sliced file so each
    //   project filament prints from its user-chosen CFS slot. effective_out receives either the
    //   original path (identity/empty mapping) or a temp file with remapped tool indices.
    bool apply_cfs_tool_mapping(const std::string& mapping,
                                const boost::filesystem::path& source,
                                boost::filesystem::path& effective_out,
                                wxString& msg) const;
};

}

#endif
