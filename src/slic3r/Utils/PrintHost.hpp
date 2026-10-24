#ifndef slic3r_PrintHost_hpp_
#define slic3r_PrintHost_hpp_

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <functional>
#include <vector>
#include <boost/filesystem/path.hpp>

#include <utility>
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

// Orca: stands for the uploaded file's name in a "start_script"; the host replaces it with the
// server-confirmed path, which can differ on a filename collision.
constexpr const char* PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER = "{{uploaded_filename}}";

// Orca: print-start options a printer's firmware supports, declared as data and rendered by
// DevicePrintOptionsDialog. Keyed on the profile's protocol since the print-host path has no agent.
// Every option is a checkbox whose value is "1" or "0".
struct DevicePrintOption
{
    std::string key;     ///< wire + persistence key, e.g. "bed_leveling"
    std::string label;   ///< L()-marked msgid, translated at render time
    std::string tooltip; ///< L()-marked msgid, may be empty
    std::string default_value{"0"}; ///< used when nothing was remembered for this printer
};

struct DevicePrintSpec
{
    bool supports_filament_mapping{false};
    std::vector<DevicePrintOption> options;

    bool empty() const { return !supports_filament_mapping && options.empty(); }
};

// Orca: how the filament->tool map reaches the printer; a reported Klipper changer overrides the vendor's.
enum class MapDelivery { none, snapmaker };
MapDelivery effective_map_delivery(FilamentMappingProtocol vendor, const std::string& reported_changer);

/// Options come from the vendor regardless of delivery. An empty spec means the stock send dialog.
DevicePrintSpec device_print_spec(FilamentMappingProtocol vendor, MapDelivery delivery);

/// Arrays are per logical filament except those marked physical, which are per physical tool.
struct DevicePrintJobInfo
{
    std::vector<int>         filament_map_1based; ///< logical filament -> 1-based physical tool
    std::vector<std::string> filament_type;       ///< logical
    std::vector<double>      nozzle_temp;         ///< logical
    std::vector<double>      flow_ratio;          ///< logical
    std::vector<double>      filament_diameter;   ///< logical
    std::vector<double>      used_g;              ///< logical
    std::vector<double>      used_mm;             ///< logical
    std::vector<double>      nozzle_diameter;     ///< physical
    std::vector<int>         used_physical_tools; ///< physical, deduped, first-use order
    double                   line_width{0.};
    double                   layer_height{0.};
    double                   outer_wall_speed{0.};
    /// Keyed by DevicePrintOption::key.
    std::map<std::string, std::string> options;

    bool option_on(const std::string& key) const
    {
        auto it = options.find(key);
        return it != options.end() && it->second == "1";
    }
};


/// Vendor prelude, map, then SD start. "" when there is no delivery or the map cannot be rendered.
std::string build_device_start_script(FilamentMappingProtocol vendor, MapDelivery delivery, const std::string& filename, const DevicePrintJobInfo& job);

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
    // Reads the "err" field of a JSON reply, 0 when absent. Returns -1 when the body is not valid JSON.
    static int get_err_code_from_body(const std::string &body);

    //Support for cloud webui login
    virtual bool is_cloud() const { return false; }
    virtual bool is_logged_in() const { return false; }
    virtual void log_out() const {}
    virtual bool get_login_url(wxString& auth_url) const { return false; }

protected:
    virtual wxString format_error(const std::string &body, const std::string &error, unsigned status) const;
    static wxString timed_out_message();
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

    // Uploads the job, firing UploadStarted and a matching UploadFinished. An exception thrown by
    // the upload is reported through error_fn and makes the upload fail.
    static bool upload_job(PrintHostJob &job, PrintHost::ProgressFn progress_fn, PrintHost::ErrorFn error_fn, PrintHost::InfoFn info_fn);

private:
    struct priv;
    std::shared_ptr<priv> p;
};



}

#endif
