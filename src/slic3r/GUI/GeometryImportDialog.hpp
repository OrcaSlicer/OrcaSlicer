#pragma once

#include <string>
#include <vector>

class CheckBox;
class wxSizer;
class wxStaticText;
class wxString;
class wxWindow;

namespace Slic3r {
class AppConfig;
class Model;

namespace GUI {

// Values remain local to an import until all of its new dialogs have succeeded.
struct GeometryImportOptions {
    bool keep_names = true;
    bool sort_names = false;
    bool load_as_assembly = true;
    // AMF only: load the objects as separate objects, as OrcaSlicer did before; the assembly choice is then unused.
    bool separate_objects = false;
    // STEP only: Orca's Split compound choice; without it a multi-body compound loses its body names.
    bool split_compound = true;
    bool show_dialog = true;
    bool remember = false;
    bool dont_ask = false;
    bool chosen = false;
    double linear_deflection = 0.003;
    double angle_deflection = 0.5;
    bool single_object() const { return !separate_objects && !load_as_assembly; }
};

struct GeometryImportNamePolicy {
    bool show_dialog = true;
    bool remove = false;
    bool remember = false;
    bool dont_ask = false;
    bool used = false;
    std::string replacement = "_";
};

// STEP and STP share one preference set; an empty result means an unrelated format.
std::string geometry_import_format(const std::string& path);
// Inspect the ZIP directory only, so CAD files do not enter the project-opening path.
bool is_geometry_only_3mf(const std::string& path);
GeometryImportOptions geometry_import_options(const AppConfig& config, const std::string& format);
GeometryImportNamePolicy geometry_import_name_policy(const AppConfig& config);
bool show_geometry_import_options(wxWindow* parent, const std::string& format, GeometryImportOptions& options);
// App-styled check box with its label, shared by the import dialogs.
struct ImportCheckBox {
    ::CheckBox* box = nullptr;
    wxStaticText* label = nullptr;
    bool GetValue() const;
    void SetValue(bool value) const;
    // Greys the label too and stops it toggling the box.
    void Enable(bool enable) const;
    void SetToolTip(const wxString& tip) const;
};
// A click on the label toggles the box and sends its wxEVT_TOGGLEBUTTON, as a click on the box does.
ImportCheckBox add_import_checkbox(wxWindow* parent, wxSizer& sizer, const wxString& text, bool value, int flags, int border);
// Sorting is offered only together with Keep original names; shared by the STEP dialog.
void link_sort_to_keep(const ImportCheckBox& keep, const ImportCheckBox& sort);
bool prepare_geometry_import_names(wxWindow* parent, const std::vector<Model*>& models, GeometryImportNamePolicy& policy);
void save_geometry_import_options(AppConfig& config, const std::string& format, const GeometryImportOptions& options);
void save_geometry_import_name_policy(AppConfig& config, const GeometryImportNamePolicy& policy);

} // namespace GUI
} // namespace Slic3r
