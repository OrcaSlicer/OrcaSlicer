#pragma once

// Native client for the Elegoo cloud (the "cloud" printers of an Elegoo account, e.g. Centauri Carbon 2).
//
// The protocol follows Elegoo's open-source elegoo-link SDK:
//   * REST on matrix.elegoo.com with a Bearer token (account, device list, object storage slot);
//   * the sliced file is PUT to object storage, then the printer is told over Agora RTM 2.x
//     (user-channel messages to "<agoraUserId><serial>") to download it (1057, progress 6006)
//     and to start the print (1020). Canvas (multi-material) status is method 2005.
//
// Agora RTM is a proprietary runtime. It is never bundled: it is loaded at run time from an
// ElegooSlicer install or downloaded from the elegoo-link repository, like the Bambu network plugin.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include "PrintHost.hpp"

namespace Slic3r {

class ElegooCloudError : public std::runtime_error
{
public:
    explicit ElegooCloudError(const std::string& what) : std::runtime_error(what) {}
};

struct ElegooCloudAccount
{
    std::string region{"global"}; // "global" or "CN"
    std::string access_token;
    std::string refresh_token;
    int64_t     access_expire_ms{0};
    std::string user_id;
    std::string nickname;
    std::string email;
    std::string avatar;
};

struct ElegooCloudPrinter
{
    std::string serial;
    std::string model; // Elegoo product code, e.g. "CC2"
    std::string name;
    int         state{-1}; // machine_status.status from the cloud report: 1 idle, 2 printing, -1 unknown
};

struct ElegooCanvasTray
{
    int         canvas_id{0};
    int         tray_id{0};
    std::string filament_type;
    std::string filament_name;
    std::string brand;
    std::string color; // "#RRGGBB"
    int         status{0}; // 0 empty, 1 loaded, 2 unknown, 3 loaded (RFID)
    bool        loaded() const { return status == 1 || status == 3; }
};

struct ElegooCanvasInfo
{
    bool                          connected{false};
    bool                          auto_refill{false};
    std::vector<ElegooCanvasTray> trays;
};

struct ElegooSlotMapEntry
{
    int t{0};         // project filament index
    int canvas_id{0};
    int tray_id{0};
};

struct ElegooPrintOptions
{
    bool                            start_print{true};
    bool                            bed_leveling{true};
    bool                            timelapse{false};
    bool                            auto_refill{false};
    bool                            set_auto_refill{false}; // send method 2004 before printing
    char                            plate_side{'A'};        // "print_layout": A = textured PEI, B = smooth PTE
    std::vector<ElegooSlotMapEntry> slot_map;
};

// Reports progress 0..100 and a status line; returns true to cancel.
using ElegooProgressFn = std::function<bool(int percent, const std::string& status)>;

class ElegooCloud
{
public:
    static ElegooCloud& instance();

    bool               signed_in() const;
    ElegooCloudAccount account() const;
    std::string        account_label() const;
    void               set_account(const ElegooCloudAccount& account);
    void               sign_out();
    std::string        region() const;
    void               set_region(const std::string& region);

    // Login page of the Elegoo account site; it reports the tokens back over the WebView bridge.
    std::string login_url(bool dark) const;
    // Fills the account from the tokens a login page reported ("report.userInfo" params).
    bool accept_login_report(const nlohmann::json& params, std::string& error);

#ifdef _WIN32
    // Copies the sign-in ElegooSlicer saved (%APPDATA%\ElegooSlicer\user\user_info, DPAPI).
    bool import_from_elegooslicer(std::string& error);
#endif

    // All throw ElegooCloudError. Blocking: call them off the UI thread.
    std::vector<ElegooCloudPrinter> printers();
    int                             printer_state(const std::string& serial);
    ElegooCanvasInfo                canvas_info(const std::string& serial);
    void send(const boost::filesystem::path& file, const std::string& display_name, const std::string& serial,
              const ElegooPrintOptions& options, const ElegooProgressFn& progress);

    static std::string user_agent();
    static boost::filesystem::path data_folder();

private:
    ElegooCloud();
    std::string    api_base() const;
    nlohmann::json request(const std::string& method, const std::string& path, const nlohmann::json* body, bool auth);
    nlohmann::json call(const std::string& method, const std::string& path, const nlohmann::json* body = nullptr);
    void           ensure_token();
    void           refresh();
    void           load();
    void           save() const;
    std::string    upload_to_storage(const std::string& server_name, const boost::filesystem::path& file,
                                     const std::string& md5_base64, const ElegooProgressFn& progress);
    nlohmann::json file_detail(const std::string& serial, const std::string& filename);

    mutable std::recursive_mutex m_mutex;
    ElegooCloudAccount           m_account;
    // One RTM session per account at a time: the cloud allows a single messaging login.
    std::mutex m_rtm_mutex;
};

// PrintHost that sends through ElegooCloud, so cloud jobs use the regular upload queue.
// The target printer and the print options travel in PrintHostUpload::extended_info
// (see ElegooCloudHost::to_extended_info).
class ElegooCloudHost : public PrintHost
{
public:
    ElegooCloudHost() = default;
    ~ElegooCloudHost() override = default;

    const char*                get_name() const override { return "Elegoo Cloud"; }
    bool                       test(wxString& curl_msg) const override;
    wxString                   get_test_ok_msg() const override;
    wxString                   get_test_failed_msg(wxString& msg) const override;
    bool                       upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                       has_auto_discovery() const override { return false; }
    bool                       can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string                get_host() const override { return "matrix.elegoo.com"; }
    bool                       is_cloud() const override { return true; }
    bool                       is_logged_in() const override { return ElegooCloud::instance().signed_in(); }

    static void               to_extended_info(const std::string& serial, const ElegooPrintOptions& options,
                                               std::map<std::string, std::string>& info);
    static ElegooPrintOptions from_extended_info(const PrintHostUpload& upload, std::string& serial);
};

} // namespace Slic3r
