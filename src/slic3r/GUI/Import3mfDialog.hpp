#ifndef slic3r_GUI_Import3mfDialog_hpp_
#define slic3r_GUI_Import3mfDialog_hpp_

#include "GUI_Utils.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>

#include <map>
#include <string>
#include <vector>

namespace Slic3r {
// Forward declaration
struct Bbs3mfProjectInfo;

namespace GUI {

// Dialog widths in DIP (device-independent pixels)
// SMALL: Basic dialog without filament mapping
// LARGE: Extended dialog with 2-column filament mapping grid
constexpr int IMPORT_DIALOG_WIDTH_SMALL = 380;
constexpr int IMPORT_DIALOG_WIDTH_LARGE = 560;

// Load type returned by determine_3mf_load_type()
enum class LoadType : unsigned char
{
    Unknown,
    OpenProject,
    LoadGeometry,
    LoadConfig
};

// Structure to hold user's 3MF import preferences.
// Populated by Import3mfDialog when user makes import choices,
// consumed by Plater::load_files() to apply those choices.
struct Import3mfSettings
{
    // User choices
    bool import_printer_settings{true};
    bool import_filament_settings{true};
    std::string reassign_printer;                       // Printer preset name if not importing printer settings
    std::map<int, std::string> filament_color_remapping; // filament_index (1-based) -> preset name
    
    // Pre-parsed project info (filled before dialog is shown)
    int project_filament_count{0};
    std::vector<std::string> project_filament_colors;   // Hex color strings
    std::string project_printer_name;
    std::vector<std::string> project_filament_preset_names;
    bool project_has_printer_settings{false};
    bool project_has_filament_settings{false};
};

// A row widget for filament mapping: [ColorSwatch] → [Slot ComboBox]
class FilamentMappingRow : public wxPanel
{
public:
    FilamentMappingRow(wxWindow* parent, int filament_index, const wxColour& color, 
                       const std::vector<std::string>& available_filaments);
    
    int get_filament_index() const { return m_filament_index; }
    std::string get_selected_filament() const;  // Returns selected preset name
    
private:
    int         m_filament_index;  // 1-based
    wxPanel*    m_color_swatch{nullptr};
    wxColour    m_color;
    ComboBox*   m_slot_dropdown{nullptr};
    std::vector<std::string> m_filament_names;  // Preset names for selectable items only
    std::vector<int> m_separator_indices;       // Indices of separator items in the dropdown
    
    void on_paint_swatch(wxPaintEvent& evt);
    void populate_slot_dropdown(const std::vector<std::string>& available_filaments);
};

class Import3mfDialog : public DPIDialog
{
private:
    // Dialog sizing
    static constexpr int WIDTH_SMALL = IMPORT_DIALOG_WIDTH_SMALL;
    static constexpr int WIDTH_LARGE = IMPORT_DIALOG_WIDTH_LARGE;
    
    wxColour          m_def_color = wxColour(255, 255, 255);
    int               m_action{1};
    bool              m_remember_choice{false};

    // Project settings import options
    bool              m_import_printer_settings{true};
    bool              m_import_filament_settings{true};
    std::string       m_reassign_printer;
    std::map<int, std::string> m_filament_color_remapping;  // filament_index -> preset name

    // 3MF color data (passed in from pre-parse)
    std::vector<std::string> m_project_filament_colors; // Hex color strings from 3MF
    
    // Available filament presets for mapping dropdowns (preset name)
    std::vector<std::string> m_available_filaments;  // All visible filament preset names
    
    // Flags for what the 3MF actually contains
    bool              m_has_printer_settings{false};
    bool              m_has_filament_settings{false};
    
    // Main sizer for size control
    wxBoxSizer*       m_main_sizer{nullptr};

    // UI components - printer settings section
    wxPanel*          m_printer_settings_panel{nullptr};
    CheckBox*         m_cb_printer_settings{nullptr};        // Styled checkbox
    wxStaticText*     m_cb_printer_label{nullptr};           // Label next to checkbox
    wxPanel*          m_printer_reassign_panel{nullptr};     // Panel for reassign row (show/hide)
    wxStaticText*     m_printer_reassign_label{nullptr};
    ComboBox*         m_printer_dropdown{nullptr};           // Grouped by manufacturer
    wxStaticText*     m_printer_warning_label{nullptr};      // Warning if printer not found

    // UI components - filament settings section  
    wxPanel*          m_filament_settings_panel{nullptr};
    CheckBox*         m_cb_filament_settings{nullptr};       // Styled checkbox
    wxStaticText*     m_cb_filament_label{nullptr};          // Label next to checkbox
    wxPanel*          m_filament_mapping_panel{nullptr};     // Panel for mapping rows (show/hide)
    wxScrolledWindow* m_filament_scroll{nullptr};            // Scrollable container for many filaments
    std::vector<FilamentMappingRow*> m_filament_rows;        // One per project filament

public:
    Import3mfDialog(const std::string& filename);
    
    // Set pre-parsed project info (call before ShowModal)
    void set_project_info(const Slic3r::Bbs3mfProjectInfo& info);

    wxPanel*      m_top_line{nullptr};
    wxStaticText* m_fname_title{nullptr};
    wxStaticText* m_fname_f{nullptr};
    wxStaticText* m_fname_s{nullptr};

    void      on_select_ok(wxCommandEvent& event);
    void      on_select_cancel(wxCommandEvent& event);

    int       get_action() const { return m_action; }
    void      set_action(int index) { m_action = index; }

    // Getters for import settings
    Import3mfSettings get_import_settings() const;

private:
    void create_printer_settings_section(wxBoxSizer* parent_sizer);
    void create_filament_settings_section(wxBoxSizer* parent_sizer);
    void populate_printer_dropdown();
    void populate_filament_mapping();
    void collect_available_filaments();
    
    void on_action_changed(int selection);
    void on_printer_checkbox_changed();
    void on_filament_checkbox_changed();
    void update_conditional_sections();
    void update_dialog_size();
    
    // Get current dialog width based on state
    int get_required_width() const;
    
    // Helper to parse hex color
    static wxColour hex_to_colour(const std::string& hex);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
};

// Main entry point: Shows import dialog and returns user's choice.
// If override_setting is provided, skips dialog and uses that setting directly.
// Stores import settings in global state accessible via get_pending_3mf_import_settings().
LoadType determine_3mf_load_type(std::string filename, std::string override_setting = "");

bool has_pending_3mf_import_settings();
Import3mfSettings get_pending_3mf_import_settings();
void clear_pending_3mf_import_settings();
const std::map<int, int>* get_pending_3mf_filament_remap();

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Import3mfDialog_hpp_
