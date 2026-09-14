#ifndef SLIC3R_GUI_PLUGINPICKERDIALOG_HPP
#define SLIC3R_GUI_PLUGINPICKERDIALOG_HPP

#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#    include <wx/wx.h>
#endif

#include <vector>
#include <string>

#include "slic3r/plugin/PluginManager.hpp"

#include "GUI_Utils.hpp"
#include "Widgets/ComboBox.hpp"

namespace Slic3r { namespace GUI {

class PluginPickerDialog : public DPIDialog
{
public:
    // Entry for capability-level selection (plugin_type non-empty path).
    struct CapabilityEntry {
        std::string plugin_key;
        std::string name;
        wxString    label;
        wxString    description;
    };

    // Existing constructor: offer all loaded plugin packages (plugin_type empty path).
    PluginPickerDialog(wxWindow* parent,
                       const wxString& plugin_type_label,
                       const std::vector<Slic3r::PluginDescriptor>& plugins);

    // New constructor: offer a list of capabilities (plugin_type non-empty path).
    PluginPickerDialog(wxWindow* parent,
                       const wxString& plugin_type_label,
                       std::vector<CapabilityEntry> capabilities);

    // Returns the plugin_key of the selected plugin package (package path).
    std::string selected_plugin_key() const;

    // Returns the {plugin_key, name} of the selected capability (capability path).
    CapabilityEntry selected_capability() const;

    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void build_ui(const wxString& plugin_type_label);
    void build_capability_ui(const wxString& plugin_type_label);
    void update_description(int selection);
    void update_capability_description(int selection);

    ComboBox*                      m_choice       { nullptr };
    wxStaticText*                  m_description  { nullptr };
    std::vector<Slic3r::PluginDescriptor> m_plugins;
    std::vector<CapabilityEntry>   m_capabilities;
};

}} // namespace Slic3r::GUI

#endif // SLIC3R_GUI_PLUGINPICKERDIALOG_HPP
