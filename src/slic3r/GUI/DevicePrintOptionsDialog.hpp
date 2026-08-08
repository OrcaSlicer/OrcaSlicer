#ifndef slic3r_DevicePrintOptionsDialog_hpp_
#define slic3r_DevicePrintOptionsDialog_hpp_

#include <map>
#include <string>
#include <vector>

#include "PrintHostDialogs.hpp"
#include "../Utils/PrintHost.hpp"

class Button;

namespace Slic3r {
namespace GUI {

class FilamentMapRowsView;

// Orca: the standard send dialog for printers whose profile DECLARES what its screen offers at
// print start (DevicePrintSpec, see Utils/PrintHost.hpp). It renders that declaration -- today
// a checkbox per Bool option, plus the plate's filament->tool mapping when the protocol carries
// one -- instead of hard-coding any one vendor's parameters, so a new printer is added by
// declaring its options in device_print_spec(), not by writing another dialog.
//
// Deliberately a subclass rather than an extension of PrintHostSendDialog: that base is the
// parent of every vendor's send dialog, so a defect there would reach printers we cannot test.
// Plater selects this one only when the profile declares a non-empty spec; nothing else changes.
//
// Vendor dialogs (Elegoo, Creality, Flashforge) can migrate here one at a time by adding their
// declaration to device_print_spec() and deleting their subclass.
class DevicePrintOptionsDialog : public PrintHostSendDialog
{
public:
    // plate_filaments: 1-based used filament ids for the plate being sent, in plate order; empty
    //   when there is nothing to map.
    // mapping_undeliverable_reason: non-empty when this printer's host cannot carry a filament
    //   map to the printer (today: any host class other than Moonraker). "Upload and Print" is
    //   then disabled up front with this text as its tooltip, rather than being silently
    //   downgraded to an upload after the user clicks it.
    DevicePrintOptionsDialog(const boost::filesystem::path& path,
                             PrintHostPostUploadActions     post_actions,
                             const wxArrayString&           groups,
                             const wxArrayString&           storage_paths,
                             const wxArrayString&           storage_names,
                             bool                           switch_to_device_tab,
                             DevicePrintSpec                spec,
                             std::string                    printer_preset_name,
                             std::vector<int>               plate_filaments,
                             wxString                       mapping_undeliverable_reason);

    void init() override;
    void EndModal(int ret) override;

    /// Chosen option values keyed by DevicePrintOption::key, ready for DevicePrintJobInfo::options.
    const std::map<std::string, std::string>& options() const { return m_options; }

    /// 1-based physical tool per PROJECT filament, read from the embedded rows when Print is
    /// accepted; empty when this printer declares no mapping (the send path must then treat it as
    /// "do not start the print").
    const std::vector<int>& filament_map() const { return m_filament_map; }

private:
    // Per-printer persistence key for an option, so a printer's remembered choices cannot leak
    // onto another printer that happens to declare the same option key.
    static std::string config_key(const std::string& option_key) { return "print_option_" + option_key; }

    DevicePrintSpec  m_spec;
    std::string      m_printer_preset_name;
    std::vector<int> m_plate_filaments;
    wxString         m_mapping_undeliverable_reason;

    std::map<std::string, std::string> m_options;
    std::vector<int>                   m_filament_map;

    // The shared mapping UI (rows + record offer + mismatch warning + recolored plate preview),
    // embedded above the options rather than opened as a second dialog.
    FilamentMapRowsView* m_rows_view{nullptr};
    Button*              m_print_btn{nullptr};
};

}} // namespace Slic3r::GUI

#endif /* slic3r_DevicePrintOptionsDialog_hpp_ */
