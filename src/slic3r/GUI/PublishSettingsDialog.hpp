#ifndef slic3r_GUI_PublishSettingsDialog_hpp_
#define slic3r_GUI_PublishSettingsDialog_hpp_

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "Widgets/TabCtrl.hpp"

#include "libslic3r/PublishSettings.hpp"

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <wx/menu.h>
#include <vector>
#include <string>
#include <functional>

// Forward declarations (all are global classes, see Widgets/TextInput.hpp and
// Widgets/StaticLine.hpp).
class TextInput;
class StaticLine;

namespace Slic3r { namespace GUI {

struct PublishMaterialIdentity
{
    std::string type;
    std::string vendor;
    std::string id;
};

// Dialog letting a model author select which settings get embedded in a 3MF. Nested tab layout
// mirroring the Process settings (Printer / Filament / Process outer tabs, category or material
// tabs inside each). Dirty settings are pre-checked and shown bold; on OK the print rows become
// "published_keys" and the material rows become "published_material_keys".
class PublishSettingsDialog : public DPIDialog
{
public:
    PublishSettingsDialog(wxWindow* parent = nullptr);
    ~PublishSettingsDialog();

    // The selected print/printer setting keys (in display order); printer keys carry a '#N'
    // per-extruder suffix.
    std::vector<std::string> GetPublishedKeys() const;

    // The selected keys grouped per material section (base keys, no '#N' suffix).
    std::vector<Slic3r::PublishedMaterialEntry> GetPublishedMaterialKeys() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // Which part of the settings the row/category came from.
    enum class Section { Print, Printer, Material };

    // One selectable setting row: a checkbox (setting name) plus a value label and an optional
    // grey unit label. key is the full config key, possibly with a "#N" variant suffix
    // (print/printer rows); material rows carry the base key.
    enum class RowKind {
        Setting, // a regular setting key
        Color,   // material colour requirement (filament_colour)
        Type,    // material type requirement (read-only text)
    };
    struct Row
    {
        std::string key;
        wxString category;
        wxString subcategory;
        wxString label;
        wxString value;
        wxString unit;
        wxString section_title; // outer tab title, for filter matching
        Section section{Section::Print};
        RowKind kind{RowKind::Setting};
        size_t outer_index{0};
        size_t inner_index{0};
        size_t subcategory_index{0};
        bool dirty{false};          // matches a dirty base key: pre-checked + bold
        bool matches_filter{false}; // survives the active filter (computed by apply_filter)
        wxCheckBox* check{nullptr};
        wxStaticText* value_label{nullptr};
        wxStaticText* unit_label{nullptr};
        wxStaticBitmap* color_chip{nullptr}; // Color rows only; swatch next to the value
        wxSizerItem* item{nullptr}; // sizer item of this row's h-sizer in its tab list sizer
    };

    // An optgroup heading. Rows store indices into m_rows.
    struct Subcategory
    {
        wxString title;
        ::StaticLine* header{nullptr}; // null when the title is empty
        wxSizerItem* item{nullptr};
        std::vector<size_t> rows;
    };

    // An inner TabCtrl page with its material controls and content.
    struct Category
    {
        wxString title;
        Section section{Section::Print};
        size_t group{0};               // index into m_sections / outer page
        size_t source_index{0};        // stable source page or material slot index
        std::string source_title;
        std::string icon_name;
        wxPanel* page{nullptr};
        wxScrolledWindow* scroll{nullptr};
        wxBoxSizer* list_sizer{nullptr};
        wxStaticText* info{nullptr};
        wxPoint scroll_pos{0, 0};
        ScalableBitmap icon_bmp;       // scalable bitmap for DPI changes
        wxStaticBitmap* icon{nullptr};
        wxStaticBitmap* filament_color_chip{nullptr};
        wxStaticText* title_label{nullptr}; // material title (static text; Full Publish carries the label elsewhere)
        // "Full Publish": while checked, the whole slot preset is serialized and its rows
        // (incl. Color/Type) are disabled.
        bool full{false};
        wxCheckBox* full_check{nullptr};
        // Material identity, only for Section::Material categories.
        std::string filament_type;
        std::string filament_vendor;
        std::string filament_id;
        // The author's 0-based filament slot this material section represents.
        size_t filament_slot{0};
        std::vector<Subcategory> subs;
        std::vector<size_t> rows; // flattened rows of this category
    };

    // One outer TabCtrl page. Category entries are its inner tabs.
    struct SectionGroup
    {
        wxString title;               // _L("Printer") / _L("Filament") / _L("Process")
        Section kind{Section::Print}; // maps 1:1 to the display group
        std::string icon_name;        // "printer" / "filament" / "process"
        ScalableBitmap icon_bmp;      // tab icon next to the title; rescaled on DPI change
        wxPanel* page{nullptr};
        TabCtrl* tabs{nullptr};
        wxPanel* page_host{nullptr};
        wxBoxSizer* page_host_sizer{nullptr};
        int selected_inner{-1};
        std::vector<size_t> categories; // indices into m_categories
    };

    void build_option_model();
    void apply_filter(const wxString& filter_text);
    void select_all(bool value);
    void select_visible(bool value);
    void show_menu(wxMouseEvent& evt);
    void set_row_bold(Row& row, bool bold);
    // "Full Publish" toggled: disables/enables the material's rows.
    void on_full_toggle(size_t category_index);
    // Return/create the fixed outer page for a Section kind.
    size_t section_group_for(Section kind);
    size_t category_index_for(const wxString& title, Section section, const std::string& icon_name, size_t group,
                              size_t source_index, const PublishMaterialIdentity& identity = PublishMaterialIdentity());
    size_t subcategory_index_for(size_t category_index, const wxString& title, const wxString& icon);
    void add_row_ui(const std::string& key, const wxString& label, const wxString& value, const wxString& unit,
                    size_t category_index, size_t subcategory_index, RowKind kind = RowKind::Setting);
    // The non-structural filament keys of a slot's preset, for a "Full Publish" entry.
    std::vector<std::string> full_keys_for_slot() const;
    void save_scroll_position(Category& category);
    void show_outer_page(size_t section_index);
    void show_inner_page(size_t section_index, int inner_index);
    void on_outer_tab_changed(wxCommandEvent& event);
    void on_inner_tab_changed(size_t section_index, wxCommandEvent& event);
    bool row_is_visible(const Row& row) const;
    void apply_visibility();
    void bind_tab_events();

    TabCtrl* m_outer_tabs{nullptr};
    wxPanel* m_outer_host{nullptr};
    wxBoxSizer* m_outer_host_sizer{nullptr};
    int m_selected_outer{-1};
    wxBoxSizer* m_fb_sizer{nullptr}; // "All"/"None" buttons sizer
    TextInput* m_filter_box{nullptr};
    wxTextCtrl* m_filter_ctrl{nullptr};
    wxStaticBitmap* m_menu_button{nullptr};
    wxString m_info_nonsel;
    wxString m_info_allsel;
    wxString m_info_empty;

    ScalableBitmap m_search;
    ScalableBitmap m_menu;

    std::vector<Row> m_rows;
    std::vector<Category> m_categories;
    std::vector<SectionGroup> m_sections;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PublishSettingsDialog_hpp_
