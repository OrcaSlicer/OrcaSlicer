#pragma once

// Per-vendor "send to printer" dialogs.
//
// When the selected printer belongs to a vendor with its own slicer app, Print opens a dialog laid out
// like that app's send dialog, so the flow feels the same as printing from it:
//   * Elegoo     -> ElegooSlicer's print-send dialog (Elegoo cloud and LAN printers, Canvas mapping)
//   * Creality   -> Creality Print's send dialog (LAN printer, CFS mapping)
//   * Markforged -> Eiger's print panel (printer list, settings preset, print now / queue / backlog)
// Bambu Lab printers keep SelectMachineDialog, which already is Bambu Studio's dialog.
//
// The dialog only collects the user's choices. A VendorSendBackend turns them into a PrintHostJob,
// which the plater exports and runs through the regular upload queue. Backends for other vendors
// register a factory with register_vendor_send_backend().

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <wx/image.h>
#include <wx/colour.h>
#include <wx/string.h>

#include "GUI_Utils.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "slic3r/Utils/PrintHost.hpp"

class wxBoxSizer;
class wxChoice;
class wxPanel;
class wxScrolledWindow;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;
class wxWindow;

namespace Slic3r {
class DynamicPrintConfig;
namespace GUI {

enum class SendVendor { None, Bambu, Elegoo, Creality, Markforged };

// The vendor of the selected printer preset, from its vendor profile (or its name for user presets).
SendVendor current_send_vendor();

// What is being printed, shown at the top of every dialog.
struct SendJobSummary
{
    wxImage                   thumbnail;
    wxString                  name;       // file name without extension
    float                     time_s{0};
    double                    weight_g{0};
    int                       layers{0};
    std::vector<FilamentInfo> filaments; // project filaments used by the plate (id = extruder index)
};

struct SendPrinter
{
    std::string id;       // backend-specific
    wxString    name;
    wxString    detail;   // "SN: ..." or the host address
    wxString    status;   // "Idle", "Printing", ...
    wxString    group;    // "Cloud printers", "LAN printers", ...
    std::string model;
    bool        busy{false};
    bool        online{true};
};

struct SendSlot
{
    std::string id;      // backend-specific, e.g. "canvas:0:2" or "T1C"
    wxString    label;   // "1A", "2C"
    std::string type;    // "PLA"
    wxString    name;    // full filament name, for tooltips
    wxColour    color;
    bool        loaded{true};
};

struct SendSlots
{
    bool                  available{false};
    wxString              system_name; // "Canvas", "CFS"
    wxString              message;     // shown when not available
    std::vector<SendSlot> slots;
    bool                  auto_refill{false};
    bool                  supports_auto_refill{false};
};

struct SendOption
{
    enum class Kind { Toggle, Choice };
    std::string                                key;
    wxString                                   label;
    Kind                                       kind{Kind::Toggle};
    bool                                       default_on{false};
    std::vector<std::pair<std::string, wxString>> choices; // Choice: (value, label)
    std::string                                default_choice;
    wxString                                   tooltip;
    std::string                                section; // "print" (print options) or "send" (upload options)
};

struct SendRequest
{
    std::string                        printer_id;
    wxString                           file_name; // with extension
    bool                               start_print{true};
    std::map<std::string, std::string> options;   // SendOption::key -> "1"/"0" or the choice value
    std::map<int, std::string>         slot_map;  // project filament id -> SendSlot::id
};

class VendorSendBackend
{
public:
    virtual ~VendorSendBackend() = default;

    // Account handling, for cloud backends.
    virtual bool     needs_sign_in() const { return false; }
    virtual bool     signed_in() const { return true; }
    virtual bool     sign_in(wxWindow* /*parent*/) { return true; }
    virtual void     sign_out() {}
    virtual wxString account_label() const { return {}; }
    // Printers that can be listed without signing in (e.g. a LAN printer from the preset).
    virtual bool     has_local_printers() const { return false; }

    // Blocking; called on a worker thread. May throw std::exception with a user-facing message.
    virtual std::vector<SendPrinter> printers() = 0;
    virtual SendSlots                slots(const SendPrinter& /*printer*/) { return {}; }

    virtual std::vector<SendOption> options(const SendPrinter& printer) const = 0;
    // Whether the dialog offers "send only" next to "print". Eiger-style backends use a choice option instead.
    virtual bool supports_upload_only() const { return true; }

    // Turns the choices into an upload job. Returns an empty job and sets error to refuse.
    // A backend may also hand the job to another dialog (e.g. a LAN host dialog) and return an empty job without error.
    virtual PrintHostJob make_job(const SendRequest& request, const boost::filesystem::path& default_output, wxString& error) = 0;
};

// The backend keeps its own copy of the printer config: it outlives the dialog while worker threads finish.
using VendorSendBackendFactory = std::function<std::shared_ptr<VendorSendBackend>(const DynamicPrintConfig& printer_config)>;
void register_vendor_send_backend(SendVendor vendor, VendorSendBackendFactory factory);
// Registers the Elegoo and Creality backends (VendorSendBackends.cpp). Safe to call more than once.
void register_builtin_vendor_send_backends();
// The backend for the current printer, or null when its vendor has no native send dialog.
std::shared_ptr<VendorSendBackend> make_vendor_send_backend(SendVendor vendor, const DynamicPrintConfig& printer_config);

// Shows the dialog for the current printer. Returns the job to export and upload, or nothing when cancelled.
std::optional<PrintHostJob> show_vendor_send_dialog(wxWindow* parent, SendVendor vendor, std::shared_ptr<VendorSendBackend> backend,
                                                    const SendJobSummary& job, const boost::filesystem::path& default_output);

}} // namespace Slic3r::GUI
