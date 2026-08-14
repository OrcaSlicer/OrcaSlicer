#ifndef slic3r_GUI_PublishSettingsDialog_hpp_
#define slic3r_GUI_PublishSettingsDialog_hpp_

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"

#include "libslic3r/PublishSettings.hpp"

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <wx/menu.h>
#include <vector>
#include <string>
#include <functional>

// Forward declarations (all are global classes, see Widgets/TextInput.hpp,
// Widgets/StaticLine.hpp and the CollapseChevron definition in the .cpp).
class TextInput;
class StaticLine;
class CollapseChevron;

namespace Slic3r { namespace GUI {

// Dialog that lets a model author select which settings get embedded in a 3MF.
// Settings are grouped the way the tabs show them: the process (print) pages,
// the printer's per-extruder retraction settings, and one section per material
// used in the project (filament overrides). Each main category has a select-all
// tri-state header, subcategory (optgroup) headings, one row per setting
// (checkbox + grey value label), and both header levels are collapsible (chevron
// toggle). A search filter and an All/None / select-visible menu are provided.
// Modified (dirty) settings are pre-checked and shown bold. On OK, the print
// rows become the "published_keys" list and the material rows become the
// per-material "published_material_keys".
class PublishSettingsDialog : public DPIDialog
{
public:
    PublishSettingsDialog(wxWindow* parent = nullptr);
    ~PublishSettingsDialog();

    // The selected print-section setting keys (in display order). Keys may
    // contain '#'.
    std::vector<std::string> GetPublishedKeys() const;

    // The selected keys grouped per material: one entry per material section
    // with at least one checked key. Keys are base keys (no "#N" suffix).
    std::vector<Slic3r::PublishedMaterialEntry> GetPublishedMaterialKeys() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // Which part of the settings the row/category came from.
    enum class Section { Print, Printer, Material };

    // One selectable setting row: a checkbox (setting name) plus a value label
    // and a (optional) grey unit label. key is the full config key and may carry
    // a "#N" variant suffix (print/printer rows); material rows carry the base key.
    struct Row
    {
        std::string key;
        wxString category;
        wxString subcategory;
        wxString label;
        wxString value;
        wxString unit;
        wxString section_title;      // top-level group title, for filter matching
        Section section{Section::Print};
        bool dirty{false};          // matches a dirty base key: pre-checked + bold
        bool matches_filter{false}; // survives the active filter (computed by apply_filter)
        wxCheckBox* check{nullptr};
        wxStaticText* value_label{nullptr};
        wxStaticText* unit_label{nullptr};
        wxSizerItem* item{nullptr}; // sizer item of this row's h-sizer in m_list_sizer
    };

    // A subcategory (optgroup) heading. Rows store indices into m_rows.
    struct Subcategory
    {
        wxString title;
        ::StaticLine* header{nullptr}; // null when the title is empty
        bool collapsed{false};
        CollapseChevron* chevron{nullptr};
        wxSizerItem* item{nullptr}; // sizer item of the header h-sizer in m_list_sizer
        std::vector<size_t> rows;
    };

    // A main category with its select-all tri-state header.
    struct Category
    {
        wxString title;
        Section section{Section::Print};
        size_t group{0};               // index into m_sections
        std::string icon_name;         // bitmap name; empty = no icon
        ScalableBitmap icon_bmp;       // scalable bitmap for DPI changes
        wxStaticBitmap* icon{nullptr}; // 18px category icon (null when icon_name empty)
        wxCheckBox* header{nullptr};   // select-all tri-state
        // Material opt-in: the master checkbox carries the material title and
        // gates whether this material's keys may be exported.
        bool master{false};
        wxCheckBox* master_check{nullptr};
        bool collapsed{false};
        CollapseChevron* chevron{nullptr};
        wxSizerItem* item{nullptr}; // sizer item of the header h-sizer in m_list_sizer
        // Material identity, only for Section::Material categories.
        std::string filament_type;
        std::string filament_vendor;
        std::string filament_id;
        // The author's 0-based filament slot this material section represents.
        size_t filament_slot{0};
        std::vector<Subcategory> subs;
        std::vector<size_t> rows; // flattened rows, for the tri-state math
    };

    // A top-level section group mirroring the editor sidebar. Categories are
    // nested inside.
    struct SectionGroup
    {
        wxString title;                     // _L("Printer") / _L("Filament") / _L("Process")
        Section kind{Section::Print};       // maps 1:1 to the display group
        std::string icon_name;              // "printer" / "filament" / "process"
        ScalableBitmap icon_bmp;            // scalable bitmap for DPI changes
        wxStaticBitmap* icon{nullptr};      // 18px, like Category::icon
        wxCheckBox* header{nullptr};        // tri-state select-all; nullptr for the Filament group
        ::StaticLine* header_line{nullptr}; // Filament group's clickable title
        CollapseChevron* chevron{nullptr};
        wxSizerItem* item{nullptr};
        bool collapsed{false};
        std::vector<size_t> categories; // indices into m_categories
    };

    void build_option_model();
    void apply_filter(const wxString& filter_text);
    void select_all(bool value);
    void select_visible(bool value);
    void show_menu(wxMouseEvent& evt);
    void update_category_header(Category& category);
    void set_row_bold(Row& row, bool bold);
    void on_category_toggle(size_t category_index);
    // Material opt-in toggled: enables/disables the material's rows + tri-state
    // and resyncs the header.
    void on_master_toggle(size_t category_index);
    // Collapse/expand a category or subcategory; resyncs visibility + chevrons.
    void toggle_category(size_t category_index);
    void toggle_subcategory(size_t category_index, size_t subcategory_index);
    // Find-or-create the top-level section group for a Section kind (builds its
    // header row on first use).
    size_t section_group_for(Section kind);
    // Top-level section group: select-all tri-state toggled / collapse/expand /
    // header resync.
    void on_section_toggle(size_t section_index);
    void toggle_section(size_t section_index);
    void update_section_header(SectionGroup& section);
    // Single pass over categories/subs/rows: shows an item iff it is not hidden
    // by the filter and (for subs/rows) by a collapsed ancestor. Flips the
    // header chevrons. Only reads matches_filter; never re-runs filter matching.
    void apply_visibility();
    // Creates a collapse chevron with a hand cursor; clicking it (with the given
    // mouse event, LEFT_DOWN for categories / LEFT_UP for subcategories,
    // matching the header's own binding) invokes the toggle.
    CollapseChevron* create_chevron(wxWindow* parent, const wxEventTypeTag<wxMouseEvent>& event_type, std::function<void()> toggle);

    wxScrolledWindow* m_scroll{nullptr};
    wxBoxSizer* m_list_sizer{nullptr}; // vertical sizer of the scrolled window
    wxBoxSizer* m_fb_sizer{nullptr};   // "All"/"None" buttons sizer
    TextInput* m_filter_box{nullptr};
    wxTextCtrl* m_filter_ctrl{nullptr};
    wxStaticBitmap* m_menu_button{nullptr};
    wxStaticText* m_info{nullptr};
    wxString m_info_nonsel;
    wxString m_info_allsel;
    wxString m_info_empty;

    ScalableBitmap m_search;
    ScalableBitmap m_menu;

    std::vector<Row> m_rows;
    std::vector<Category> m_categories;
    // Fixed display order enforced by phase order in build_option_model():
    // Printer, then Filament, then Process.
    std::vector<SectionGroup> m_sections;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PublishSettingsDialog_hpp_
