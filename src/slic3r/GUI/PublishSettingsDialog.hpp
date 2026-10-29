#pragma once

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"

#include "libslic3r/PublishSelection.hpp"
#include "libslic3r/PublishSettings.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/panel.h>
#include <wx/gdicmn.h>
#include <wx/event.h>
#include <wx/wx.h>
#include <wx/colour.h>
#include <wx/scrolwin.h>
#include <wx/menu.h>
#include <utility>
#include <vector>
#include <string>

class TabCtrl;
class wxBoxSizer;
class wxCommandEvent;
class wxMouseEvent;
class wxPanel;
class wxScrolledWindow;
class wxSizerItem;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;
class wxWindow;
namespace Slic3r { class DynamicPrintConfig; }

// Forward declarations (all are global classes, see Widgets/TextInput.hpp and
// Widgets/StaticLine.hpp).
class TextInput;
class StaticLine;
class CheckBox;
class MultiSwitchButton;

namespace Slic3r { namespace GUI {

class ConfigOptionsGroup;
class Line;
class Page;

struct PublishMaterialIdentity
{
    std::string type;
    std::string vendor;
    std::string id;
};

// Dialog letting a model author select which settings get embedded in a 3MF. Nested tab layout
// mirroring the Process settings (Printer / Filament / Process outer tabs, category or material
// tabs inside each). Dirty settings are pre-checked and shown bold; on OK the print rows become
// "orca_published_keys" and the material rows become "orca_published_material_keys".
class PublishSettingsDialog : public DPIDialog
{
public:
    // Optional published selection (Feature A/B): when the caller supplies one (either a
    // remembered session selection or the payload of a freshly loaded published 3MF) the dialog
    // is seeded from it, overriding the dirty-default pre-check. A non-null pointer to an empty
    // selection means "publish nothing" (an intentional empty state); a null pointer means "no
    // remembered selection" (keep the dirty defaults). legacy_process_selection marks a selection
    // read from a file without per-extruder process layout: its "key#N" process entries then select
    // that setting on every extruder.
    PublishSettingsDialog(wxWindow* parent = nullptr,
                          const std::vector<std::string>* published_keys = nullptr,
                          const std::vector<Slic3r::PublishedMaterialEntry>* material_keys = nullptr,
                          bool legacy_process_selection = false);
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

    // Which part of the settings the row came from.
    enum class Section { Print, Printer, Material };

    // One option inside a row. A row is one source Line, so a multi-option row (Overhang speed,
    // Recommended nozzle temperature) stays one publish unit with one toggle.
    struct RowOption
    {
        std::string     key;        // base config key
        int             index{-1};  // vector index the value field loads: slot, file column, or -1
        ConfigOptionDef def;        // the source line's definition, including its per-line label
    };

    // One selectable setting. This list is the dialog's durable state: it drives the filter, All
    // and None, seeding and the payload, whether or not the row's widgets have been built.
    struct Row
    {
        Slic3r::PublishRowId  id;                  // publish-ready keys, owner, column, section, kind
        std::vector<RowOption> options;            // widget mapping, one per published key
        wxString              label;               // the source Line's label, for the filter
        wxString              label_en;            // untranslated label, for the cloned Line
        wxString              label_tooltip;       // the source Line's tooltip, copied verbatim
        wxString              label_path;          // the source Line's wiki path
        wxString              category;
        wxString              subcategory;
        wxString              value;               // first option's value as shown, for the filter
        wxString              unit;
        wxString              section_title;       // outer tab title, for filter matching
        size_t                outer_index{0};
        size_t                inner_index{0};
        size_t                sub_index{0}; // the Subcategory that draws this row
        bool                  publishable{true};      // false for a row whose control is an action
        bool                  dirty{false};          // matches a dirty base key: pre-checked
        bool                  matches_filter{false}; // survives the active filter
    };

    // An optgroup heading and the group that draws its rows. Rows store indices into m_rows.
    struct Subcategory
    {
        wxString            title;
        wxString            icon;           // the source optgroup's icon id
        ConfigOptionsGroup* group{nullptr}; // built when the page's rows are created
        wxSizerItem*        item{nullptr};
        std::vector<size_t> rows;
        bool                built{false}; // its group exists
        // The source optgroup's / page's layout inputs, copied so the group renders like the Tab.
        int      label_width{20};
        bool     split_multi_line{false};
        bool     option_label_at_right{false};
        wxString page_title;
        int      preset_type{-1};
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
        // One sub-tab per Filament tab page, on material pages only. Their title is the source
        // page's, so a page reads the same in the dialog as in the sidebar.
        TabCtrl*             page_tabs{nullptr};
        wxString             selected_page;
        std::vector<wxString> page_order;
        wxStaticText* info{nullptr};
        wxPoint scroll_pos{0, 0};
        wxStaticBitmap* filament_color_chip{nullptr};
        wxStaticText* title_label{nullptr}; // material title (static text; Full Publish carries the label elsewhere)
        // "Enable": while unchecked nothing of this slot is exported and everything below the
        // header row is hidden. For physical slots the Full Publish toggle sits on a second
        // line (full_line_item) visible only when enabled; for mixed slots Enable alone implies
        // publishing the mix definition, so no Full Publish widget exists at all.
        ::CheckBox* enable_check{nullptr};
        wxStaticText* enable_label{nullptr};
        wxSizerItem* full_line_item{nullptr}; // sizer item of the Full Publish line (physical slots only)
        // "Full Publish": while checked, the whole slot preset is serialized and its rows
        // (incl. Color/Type) are disabled.
        ::CheckBox* full_check{nullptr};
        wxStaticText* full_label{nullptr};
        // True for a mixed-color filament slot: no Material/Retraction rows; Enable publishes
        // the slot's gradient/ratio definition as a whole.
        bool is_mixed{false};
        // The file column whose variant rows are shown; -1 when the owner has no variant layout.
        // The variant switch sets it.
        int column{-1};
        // File columns the owner's preset defines, in preset order (empty without a layout).
        std::vector<size_t> columns;
        // The owner's variant switch, when it has more than one variant.
        MultiSwitchButton* variant_switch{nullptr};
        // The Q1 notice: this printer needs a variant the filament does not define.
        wxStaticText* variant_notice{nullptr};

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
        // Holds every category in order and stays the selection model. Hidden on the Process section,
        // whose page tabs are shown in `page_tabs` instead.
        TabCtrl* tabs{nullptr};
        // Several extruders: the extruder switch, the same MultiSwitchButton (and option names) as the
        // printer tab's Extruder page, with one option per entry of `variant_inners`.
        MultiSwitchButton* variant_switch{nullptr};
        // Process section: the visible strip of page tabs, shown below the extruder switch.
        TabCtrl* page_tabs{nullptr};
        std::vector<size_t> variant_inners; // positions in `categories` of the extruder categories
        std::vector<size_t> page_inners;    // positions in `categories` of the categories shown in `page_tabs`
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
    // Seed the dialog from a published selection (print/printer keys + per-slot material keys):
    // the supplied selection is authoritative - it is applied after the dirty pre-check and
    // overrides it, so deselected dirty keys stay off. Rows/slots not present in the selection
    // are left unselected. Unknown or out-of-range entries are skipped gracefully.
    void apply_selection(const std::vector<std::string>& published_keys,
                         const std::vector<Slic3r::PublishedMaterialEntry>& material_keys);
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
    // "Full Publish" toggled: disables/enables the material's rows.
    void on_full_toggle(size_t category_index);
    // "Enable" toggled on a material slot: reveals/hides everything below the header and, for a
    // mixed slot, auto-selects its component filaments' "Enable" + "Full Publish" toggles.
    void on_enable_toggle(size_t category_index);
    // Whether a category currently publishes something, driving its tab's indicator dot.
    // Print/Printer: any row checked. Material: the slot's "Enable" is on.
    bool category_has_selection(const Category& cat) const;
    // Recompute the indicator dot on every outer/inner tab from the current selection state.
    void refresh_tab_indicators();
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
                              bool is_mixed                          = false,
                              bool is_variant                        = false);
    size_t subcategory_index_for(size_t category_index, const wxString& title, const wxString& icon,
                                 const ConfigOptionsGroup* source, const Page* page, int preset_type);
    // Appends one row to the model. Returns the row index. `id.keys` must already be
    // publish-ready and `options` must line up with it.
    size_t add_row(Row row, size_t category_index, size_t subcategory_index);
    // Creates the ConfigOptionsGroup rows of one category, once. Safe to call repeatedly.
    void build_row_groups(size_t category_index);
    // Fills a material page's sub-tab strip from its source pages, once the rows exist.
    void build_page_tabs(size_t category_index);
    // Adds the owner's variant switch when its preset defines more than one variant.
    void setup_material_variant_switch(size_t category_index);
    // Whether a row belongs to the variant currently shown (slot-level rows always do).
    bool row_on_selected_variant(size_t row_index) const;
    // Pushes one row's model state into its line's row toggle, if the widgets exist.
    void sync_row_widgets(size_t row_index);
    void sync_all_row_widgets();
    // Colours the row's label the way the Tab marks a modified setting.
    void apply_row_label_colour(size_t row_index);
    // The line a row is drawn by, or null before its group exists.
    Line* row_line(size_t row_index) const;
    // Model-backed row state: the payload and the widgets both read this, never each other.
    void set_row_checked(size_t row_index, bool checked);
    void set_row_enabled(size_t row_index, bool enabled);
    // The non-structural filament keys of a slot's preset, for a "Full Publish" entry.
    std::vector<std::string> full_keys_for_slot() const;
    void save_scroll_position(Category& category);
    void show_outer_page(size_t section_index);
    void show_inner_page(size_t section_index, int inner_index);
    void show_mixed_page(size_t section_index, int mixed_index);
    void on_outer_tab_changed(wxCommandEvent& event);
    void on_inner_tab_changed(size_t section_index, wxCommandEvent& event);
    void on_page_tab_changed(size_t section_index, wxCommandEvent& event);
    void on_mixed_tab_changed(size_t section_index, wxCommandEvent& event);
    bool row_is_visible(size_t row_index) const;
    void apply_visibility();
    void bind_tab_events();
    // Replaces a section's inner tab strip with a variant switch (see SectionGroup::variant_switch).
    void setup_variant_switch(size_t section_index);

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
    bool m_legacy_process_selection{false};

    std::vector<Row> m_rows;
    std::vector<Category> m_categories;
    std::vector<SectionGroup> m_sections;
    // One entry per row, in display order, handed to the selection model's conversions.
    std::vector<Slic3r::PublishRowId> m_row_ids;
    // Every publishable value the dialog shows, read once when it opens. Declared before the
    // groups so it outlives them.
    Slic3r::DynamicPrintConfig m_snapshot;
    // The selection state: checked and enabled per row, enabled and Full Publish per owner, and
    // the owner identity the payload needs. The widgets are a view over it.
    Slic3r::PublishSelection m_selection;
};

}} // namespace Slic3r::GUI
