#pragma once

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "Widgets/TabCtrl.hpp"

#include "libslic3r/PublishSettings.hpp"

#include <wx/wx.h>
#include <wx/colour.h>
#include <wx/scrolwin.h>
#include <wx/menu.h>
#include <utility>
#include <vector>
#include <string>

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

// One unmet dependency of an enabled mixed-filament slot: the component filament the mix uses
// would ship without its material (either the component slot is not enabled at all, or it is
// enabled with neither "Full Publish" nor the "Type" requirement checked). Slots are 0-based.
struct MixedDependencyIssue
{
    enum class Reason { Disabled, MaterialNotPublished };
    size_t mixed_slot{0};
    size_t component_slot{0};
    Reason reason{Reason::Disabled};
};

// Dialog letting a model author select which settings get embedded in a 3MF. Nested tab layout
// mirroring the Process settings (Printer / Filament / Process outer tabs, category or material
// tabs inside each). Dirty settings are pre-checked and shown bold; on OK the print rows become
// "orca_published_keys" and the material rows become "orca_published_material_keys".
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
    void on_sys_color_changed() override;

private:
    void fit_to_content();
    void refresh_mixed_tab_bitmaps();

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
        bool dirty{false};          // matches a dirty base key: pre-checked + bold
        bool matches_filter{false}; // survives the active filter (computed by apply_filter)
        wxCheckBox* check{nullptr};
        wxStaticText* value_label{nullptr};
        wxStaticText* unit_label{nullptr};
        wxStaticBitmap* color_chip{nullptr}; // Color rows only; swatch next to the value
        wxSizerItem* item{nullptr};          // sizer item of this row's h-sizer in its tab list sizer
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
        size_t group{0};        // index into m_sections / outer page
        size_t source_index{0}; // stable source page or material slot index
        wxPanel* page{nullptr};
        wxScrolledWindow* scroll{nullptr};
        wxBoxSizer* list_sizer{nullptr};
        wxStaticText* info{nullptr};
        wxPoint scroll_pos{0, 0};
        wxStaticBitmap* filament_color_chip{nullptr};
        wxStaticText* title_label{nullptr}; // material title (static text; Full Publish carries the label elsewhere)
        // "Enable": while unchecked nothing of this slot is exported and everything below the
        // header row is hidden. For physical slots the Full Publish toggle sits on a second
        // line (full_line_item) visible only when enabled; for mixed slots Enable alone implies
        // publishing the mix definition, so no Full Publish widget exists at all.
        wxCheckBox* enable_check{nullptr};
        wxSizerItem* full_line_item{nullptr}; // sizer item of the Full Publish line (physical slots only)
        // "Full Publish": while checked, the whole slot preset is serialized and its rows
        // (incl. Color/Type) are disabled.
        wxCheckBox* full_check{nullptr};
        // True for a mixed-color filament slot: no Material/Retraction rows; Enable publishes
        // the slot's gradient/ratio definition as a whole.
        bool is_mixed{false};
        // Material identity, only for Section::Material categories.
        std::string filament_type;
        std::string filament_vendor;
        std::string filament_id;
        // The author's 0-based filament slot this material section represents.
        size_t filament_slot{0};
        std::vector<Subcategory> subs;
        std::vector<size_t> rows; // flattened rows of this category
    };

    // Frozen snapshot of a mixed filament slot's definition for the read-only visualization
    // painted on the slot's page. Plain data only: the paint handler must never touch the
    // config. For gradient slots the curve is pre-sampled (t, ratio) pairs, where ratio is the
    // first component's share over model height; anchors carry the raw control points.
    struct MixedVisualSpec
    {
        bool valid{false};
        bool is_gradient{false};
        std::vector<wxColour> component_colours;   // colour per component, in config order
        std::vector<double> ratios;                // sublayer shares summing to ~1 (non-gradient)
        std::vector<double> tri_weights;           // 3-component mixes: barycentric shares
        std::vector<std::pair<double, double>> gradient_samples;
        std::vector<std::pair<double, double>> gradient_anchors;
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
        // Second tab strip, below the main one, listing only the mixed-color filament slots.
        // Present on the Material section only (null elsewhere).
        TabCtrl* mixed_tabs{nullptr};
        wxPanel* page_host{nullptr};
        wxBoxSizer* page_host_sizer{nullptr};
        int selected_inner{-1};
        // Selected mixed tab (index into mixed_categories), valid while a mixed slot page is shown.
        int selected_mixed{-1};
        std::vector<size_t> categories;      // indices into m_categories (physical slots)
        std::vector<size_t> mixed_categories; // indices into m_categories (mixed slots)
    };

    void build_option_model();
    // Frozen snapshot of a mixed slot's definition for the page visualization, resolved from
    // the full config once at dialog-build time. Gradient slots pre-sample exactly what the
    // slicer will print: the custom curve wins over the gradient_range endpoints over the
    // 0.10 -> 0.90 default (the resolution FilamentBitmapUtils::mixed_gradient_curve mirrors).
    static MixedVisualSpec make_mixed_visual_spec(const Slic3r::DynamicPrintConfig& full, size_t slot);
    void apply_filter(const wxString& filter_text);
    // Menu-only pseudo filters: show only the checked ("Filter selected") or only the
    // unchecked ("Filter non-selected") rows. The search box keeps the user's text.
    void apply_pseudo_filter(bool selected_only);
    // Recompute row matches and visibility for the active filter mode. filter is the lowered
    // search text; it is ignored by the pseudo modes.
    void refresh_filter(const wxString& filter);
    void select_all(bool value);
    void select_visible(bool value);
    void show_menu(wxMouseEvent& evt);
    void set_row_bold(Row& row, bool bold);
    // "Full Publish" toggled: disables/enables the material's rows.
    void on_full_toggle(size_t category_index);
    // "Enable" toggled on a material slot: reveals/hides everything below the header and, for a
    // mixed slot, auto-selects its component filaments' "Enable" + "Full Publish" toggles.
    void on_enable_toggle(size_t category_index);
    // Unmet dependencies of enabled mixed-filament slots, one record per (mix, component) pair:
    // "Enable" not checked on the component, or enabled with neither "Full Publish" nor the
    // "Type" requirement row checked. Colour is deliberately ignored (the receiver renders the
    // mix from its own components' colours). Sorted by mixed slot, then component slot.
    std::vector<MixedDependencyIssue> unpublished_mixed_components() const;
    // Read-only visualization of a mixed slot's definition (a stacked ratio bar, or the
    // Material Ratio vs Model Height graph for a gradient), inserted above the info hint
    // inside the category's scroll area.
    void add_mixed_visual(size_t category_index, const MixedVisualSpec& spec);
    // Return/create the fixed outer page for a Section kind.
    size_t section_group_for(Section kind);
    size_t category_index_for(const wxString& title,
                              Section section,
                              size_t group,
                              size_t source_index,
                              const PublishMaterialIdentity& identity = PublishMaterialIdentity(),
                              bool is_mixed                          = false);
    size_t subcategory_index_for(size_t category_index, const wxString& title, const wxString& icon);
    void add_row_ui(const std::string& key,
                    const wxString& label,
                    const wxString& value,
                    const wxString& unit,
                    size_t category_index,
                    size_t subcategory_index,
                    RowKind kind = RowKind::Setting);
    // The non-structural filament keys of a slot's preset, for a "Full Publish" entry.
    std::vector<std::string> full_keys_for_slot() const;
    void save_scroll_position(Category& category);
    void show_outer_page(size_t section_index);
    void show_inner_page(size_t section_index, int inner_index);
    void show_mixed_page(size_t section_index, int mixed_index);
    void on_outer_tab_changed(wxCommandEvent& event);
    void on_inner_tab_changed(size_t section_index, wxCommandEvent& event);
    void on_mixed_tab_changed(size_t section_index, wxCommandEvent& event);
    bool row_is_visible(const Row& row) const;
    void apply_visibility();
    void bind_tab_events();

    TabCtrl* m_outer_tabs{nullptr};
    wxPanel* m_outer_host{nullptr};
    wxBoxSizer* m_outer_host_sizer{nullptr};
    int m_selected_outer{-1};
    wxBoxSizer* m_fb_sizer{nullptr}; // "All"/"None" buttons sizer
    // Active filter mode: free text from the search box, or one of the menu's pseudo filters.
    enum class FilterMode { Text, SelectedOnly, UnselectedOnly };
    FilterMode m_filter_mode{FilterMode::Text};
    TextInput* m_filter_box{nullptr};
    wxTextCtrl* m_filter_ctrl{nullptr};
    wxStaticBitmap* m_menu_button{nullptr};
    // Shown while a pseudo filter is active (the search box keeps the user's text, so the chip
    // carries the visible state); clicking it returns to text filtering.
    wxStaticText* m_pseudo_chip{nullptr};
    wxString m_info_nonsel;
    wxString m_info_allsel;
    wxString m_info_empty;
    wxString m_info_mix; // body hint shown for a mixed slot (published as a whole)

    ScalableBitmap m_search;
    ScalableBitmap m_menu;

    std::vector<Row> m_rows;
    std::vector<Category> m_categories;
    std::vector<SectionGroup> m_sections;
};

}} // namespace Slic3r::GUI
