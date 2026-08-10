#ifndef slic3r_PrintHost_hpp_
#define slic3r_PrintHost_hpp_

#include <memory>
#include <set>
#include <string>
#include <functional>
#include <vector>
#include <boost/filesystem/path.hpp>

#include <wx/string.h>

#include <libslic3r/enum_bitmask.hpp>
#include "Http.hpp"
#include <map>
class wxArrayString;

namespace Slic3r {

class DynamicPrintConfig;
enum class FilamentMappingProtocol;

enum class PrintHostPostUploadAction {
    None,
    StartPrint,
    StartSimulation,
    QueuePrint
};
using PrintHostPostUploadActions = enum_bitmask<PrintHostPostUploadAction>;
ENABLE_ENUM_BITMASK_OPERATORS(PrintHostPostUploadAction);

// Orca: a caller-supplied "start_script" (see PrintHostUpload::extended_info /
// PrintHostUpload::extended() below) may embed this generic token in place of the uploaded
// file's name. A host whose upload confirms a server-side path -- which can differ from what was
// requested, e.g. on a filename collision -- substitutes that confirmed path for the token after
// the upload completes and before running the script.
constexpr const char* PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER = "{{uploaded_filename}}";

// Dispatches to the wire dialect of a device-owned mapping protocol (FilamentMappingProtocol),
// so callers of the print-host send path need no vendor knowledge -- adding a protocol only means
// adding a case here, in Utils, not editing GUI code. filename may be
// PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER when the caller doesn't yet know the uploaded name.
// Returns "" for a protocol with no start-script dialect (fmpNone, or one whose mapping is
// delivered through IPrinterAgent::send_filament_mapping() instead); callers must not read that
// as "nothing to send" without checking device_owned_mapping_protocol() first.
// ----------------------------------------------------------------------------------------------
// Send-time print options
//
// A printer declares, as DATA, which print-start options its firmware supports; the standard send
// dialog (GUI/DevicePrintOptionsDialog) renders that declaration and hands the chosen values back.
// Vendor knowledge stays here in Utils so the GUI layer never learns a printer's dialect, and both
// send paths (print-host and agent) can read the same declaration -- the print-host path has no
// agent instance to ask, which is why this is a free function keyed on the profile's protocol.
// ----------------------------------------------------------------------------------------------

enum class DevicePrintOptionKind {
    Bool,   // rendered as a checkbox; value is "1" or "0"
    Choice, // rendered as a picker over `values`; NOT rendered yet -- see DevicePrintOption::kind
};

struct DevicePrintOptionValue
{
    std::string value; ///< wire/persistence value
    std::string label; ///< L()-marked msgid, translated at render time
};

struct DevicePrintOption
{
    std::string key;     ///< wire + persistence key, e.g. "bed_leveling"
    std::string label;   ///< L()-marked msgid, translated at render time
    std::string tooltip; ///< L()-marked msgid, may be empty
    /// Bool is implemented today. Choice is declared so a vendor carrying a non-boolean option
    /// (e.g. Elegoo's bed type) can be expressed when it migrates to the standard dialog -- the
    /// renderer for it lands with that first consumer, so it ships tested rather than speculative.
    DevicePrintOptionKind kind{DevicePrintOptionKind::Bool};
    std::string default_value{"0"};              ///< used when nothing was remembered for this printer
    std::vector<DevicePrintOptionValue> values;  ///< Choice only
};

struct DevicePrintSpec
{
    /// The plate's filament->tool assignment is picked in the dialog and delivered with the job.
    bool supports_filament_mapping{false};
    std::vector<DevicePrintOption> options;

    bool empty() const { return !supports_filament_mapping && options.empty(); }
};

/// What this printer offers at print-start time. Empty spec (the default for every protocol we
/// have no dialect for) means "use the stock send dialog", so declaring nothing changes nothing.
DevicePrintSpec device_print_spec(FilamentMappingProtocol protocol);

/// Everything a start script may need about the sliced plate. Arrays are sized by LOGICAL filament
/// (one entry per project filament) EXCEPT nozzle_diameter and used_physical_tools, which are sized
/// by PHYSICAL tool -- the firmware indexes them differently and mixing the two is silently wrong
/// (see the 2026-08-10 hardware capture pinned in tests/slic3rutils/test_snapmaker_protocol.cpp).
struct DevicePrintJobInfo
{
    std::vector<int>         filament_map_1based; ///< logical filament -> 1-based physical tool
    std::vector<std::string> filament_type;       ///< logical
    std::vector<double>      nozzle_temp;         ///< logical
    std::vector<double>      flow_ratio;          ///< logical
    std::vector<double>      filament_diameter;   ///< logical
    std::vector<double>      used_g;              ///< logical
    std::vector<double>      used_mm;             ///< logical
    std::vector<double>      nozzle_diameter;     ///< PHYSICAL
    std::vector<int>         used_physical_tools; ///< PHYSICAL, deduped, first-use order
    double                   line_width{0.};
    double                   layer_height{0.};
    double                   outer_wall_speed{0.};
    /// Chosen values from the dialog, keyed by DevicePrintOption::key.
    std::map<std::string, std::string> options;

    bool option_on(const std::string& key) const
    {
        auto it = options.find(key);
        return it != options.end() && it->second == "1";
    }
};

std::string build_device_map_start_script(FilamentMappingProtocol protocol, const std::string& filename, const std::vector<int>& filament_map_1based);

/// Full-fidelity form: renders every parameter the printer's own screen sends, including the
/// user's option choices and the plate's filament statistics.
std::string build_device_start_script(FilamentMappingProtocol protocol, const std::string& filename, const DevicePrintJobInfo& job);

struct PrintHostUpload
{
    bool use_3mf { false };
    boost::filesystem::path source_path;
    boost::filesystem::path upload_path;

    std::string group;
    std::string storage;

    PrintHostPostUploadAction post_action { PrintHostPostUploadAction::None };

    // Some extended parameters for different upload methods.
    std::map<std::string, std::string> extended_info;

    // Safe accessor for an extended_info entry; returns `def` when the key is absent.
    std::string extended(const std::string &key, const std::string &def = {}) const
    {
        auto it = extended_info.find(key);
        return it != extended_info.end() ? it->second : def;
    }
};

class PrintHost
{
public:
    virtual ~PrintHost();

    typedef Http::ProgressFn ProgressFn;
    typedef std::function<void(wxString /* error */)> ErrorFn;
    typedef std::function<void(wxString /* tag */, wxString /* status */)> InfoFn;

    virtual const char* get_name() const = 0;

    virtual bool test(wxString &curl_msg) const = 0;
    virtual wxString get_test_ok_msg () const = 0;
    virtual wxString get_test_failed_msg (wxString &msg) const = 0;
    virtual bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const = 0;
    virtual bool has_auto_discovery() const = 0;
    virtual bool can_test() const = 0;
    virtual PrintHostPostUploadActions get_post_upload_actions() const = 0;
    // A print host usually does not support multiple printers, with the exception of Repetier server.
    virtual bool supports_multiple_printers() const { return false; }
    virtual std::string get_host() const = 0;
    /**
    * Get the serial number for connecting to the printer.
    * For Elegoo CC2, the device details connection to the printer requires the serial number.
    * Other print hosts do not need to implement this interface, and it returns an empty string by default.
    */
    virtual std::string get_sn() const { return ""; }

    // Support for Repetier server multiple groups & printers. Not supported by other print hosts.
    // Returns false if not supported. May throw HostNetworkError.
    virtual bool get_groups(wxArrayString & /* groups */) const { return false; }
    virtual bool get_printers(wxArrayString & /* printers */) const { return false; }
    // Support for PrusaLink uploading to different storage. Not supported by other print hosts.
    // Returns false if not supported or fail.
    virtual bool get_storage(wxArrayString& /*storage_path*/, wxArrayString& /*storage_name*/) const { return false; }

    static PrintHost* get_print_host(DynamicPrintConfig *config);
    static std::string get_print_host_webui(DynamicPrintConfig *config);

    //Support for cloud webui login
    virtual bool is_cloud() const { return false; }
    virtual bool is_logged_in() const { return false; }
    virtual void log_out() const {}
    virtual bool get_login_url(wxString& auth_url) const { return false; }

protected:
    virtual wxString format_error(const std::string &body, const std::string &error, unsigned status) const;
};


struct PrintHostJob
{
    PrintHostUpload upload_data;
    std::unique_ptr<PrintHost> printhost;
    bool switch_to_device_tab{false};
    bool cancelled = false;

    PrintHostJob() {}
    PrintHostJob(const PrintHostJob&) = delete;
    PrintHostJob(PrintHostJob &&other)
        : upload_data(std::move(other.upload_data))
        , printhost(std::move(other.printhost))
        , switch_to_device_tab(other.switch_to_device_tab)
        , cancelled(other.cancelled)
    {}

    PrintHostJob(DynamicPrintConfig *config)
        : printhost(PrintHost::get_print_host(config))
    {}

    PrintHostJob& operator=(const PrintHostJob&) = delete;
    PrintHostJob& operator=(PrintHostJob &&other)
    {
        upload_data = std::move(other.upload_data);
        printhost   = std::move(other.printhost);
        switch_to_device_tab = other.switch_to_device_tab;
        cancelled = other.cancelled;
        return *this;
    }

    bool empty() const { return !printhost; }
    operator bool() const { return !!printhost; }
};


namespace GUI { class PrintHostQueueDialog; }

class PrintHostJobQueue
{
public:
    PrintHostJobQueue(GUI::PrintHostQueueDialog *queue_dialog);
    PrintHostJobQueue(const PrintHostJobQueue &) = delete;
    PrintHostJobQueue(PrintHostJobQueue &&other) = delete;
    ~PrintHostJobQueue();

    PrintHostJobQueue& operator=(const PrintHostJobQueue &) = delete;
    PrintHostJobQueue& operator=(PrintHostJobQueue &&other) = delete;

    void enqueue(PrintHostJob job);
    void cancel(size_t id);

private:
    struct priv;
    std::shared_ptr<priv> p;
};



}

#endif
