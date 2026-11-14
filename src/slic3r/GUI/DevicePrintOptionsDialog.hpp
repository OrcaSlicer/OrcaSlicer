#ifndef slic3r_DevicePrintOptionsDialog_hpp_
#define slic3r_DevicePrintOptionsDialog_hpp_

#include <map>
#include <string>
#include <vector>

#include "PrintHostDialogs.hpp"
#include "../Utils/PrintHost.hpp"
#include <boost/filesystem/path.hpp>
#include <wx/string.h>

class Button;

namespace Slic3r {
namespace GUI {

class FilamentMapRowsView;

// Orca: send dialog that renders a profile's declared print-start options (DevicePrintSpec) and,
// when supported, the filament->tool mapping. A subclass so PrintHostSendDialog stays untouched
// for every other vendor's dialog.
class DevicePrintOptionsDialog : public PrintHostSendDialog
{
public:
    // plate_filaments: 1-based used filament ids for the plate; empty when there is nothing to map.
    // mapping_undeliverable_reason: non-empty when the host cannot deliver a filament map; Print is
    //   then disabled and the reason shown.
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

    const std::map<std::string, std::string>& options() const { return m_options; }

    /// 1-based physical tool per project filament; empty when this printer declares no mapping.
    const std::vector<int>& filament_map() const { return m_filament_map; }

private:
    static std::string config_key(const std::string& option_key) { return "print_option_" + option_key; }

    DevicePrintSpec  m_spec;
    std::string      m_printer_preset_name;
    std::vector<int> m_plate_filaments;
    wxString         m_mapping_undeliverable_reason;

    std::map<std::string, std::string> m_options;
    std::vector<int>                   m_filament_map;

    FilamentMapRowsView* m_rows_view{nullptr};
    Button*              m_print_btn{nullptr};
};

}} // namespace Slic3r::GUI

#endif /* slic3r_DevicePrintOptionsDialog_hpp_ */
