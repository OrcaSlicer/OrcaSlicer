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

namespace Slic3r { namespace GUI {

class Line;
class PublishFilamentPicker;
class PublishParamsPanel;
class Tab;

struct PublishMaterialIdentity
{
    std::string type;
    std::string vendor;
    std::string id;
};

// Dialog letting a model author select which settings get embedded in a 3MF. Three outer tabs
// (Printer / Filament / Process): the Printer and Process tabs host the sidebar's own settings tab,
// the Filament tab has one inner tab per used material slot and one per mixed slot. Dirty settings
// are pre-checked and drawn in the Tab's modified colour; on OK the print and printer rows become
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

    // The selected process and printer setting keys (in display order); a key of a variant-bearing
    // setting carries its variant column as a '#N' suffix.
    std::vector<std::string> GetPublishedKeys() const;

    // The selected keys grouped per material section. A variant-bearing key carries its file column
    // as a '#N' suffix; a per-slot key is the plain base key.
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
        std::string key;       // base config key
        int         index{-1}; // vector index the row's value addresses: slot, file column, or -1
    };

    // One selectable setting. This list is the dialog's durable state: it drives the filter, All
    // and None, seeding and the payload, whether or not the row's widgets have been built.
    struct Row
    {
        Slic3r::PublishRowId  id;                  // publish-ready keys, owner, column, section, kind
        std::vector<RowOption> options;            // one per published key: base key and its index
        // The hosted tab's own Line this row is drawn by; the publish checkbox is attached to it.
        Line*                 tab_line{nullptr};
        wxString              label;               // the label text the filter matches
        wxString              category;
        wxString              subcategory;         // optgroup heading, filter text only
        wxString              value;               // first option's value as shown, for the filter
        wxString              unit;
        wxString              section_title;       // outer tab title, for filter matching
        size_t                outer_index{0};
        size_t                inner_index{0};
        bool                  publishable{true};      // false for a row whose control is an action
        bool                  dirty{false};          // matches a dirty base key: pre-checked
        bool                  matches_filter{false}; // survives the active filter
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
        // A physical material slot's settings are the sidebar's own Filament tab; the Printer and
        // Process sections' are the sidebar's own tabs. Null on a mixed slot.
        PublishParamsPanel* publish_host{nullptr};
        Tab*                publish_tab{nullptr};
        wxStaticText* info{nullptr};
        wxPoint scroll_pos{0, 0};
        wxStaticBitmap* filament_color_chip{nullptr};
        wxStaticText* title_label{nullptr}; // material title, refreshed on a DPI change
        // "Enable": while unchecked nothing of this slot is exported and everything below the
        // header row is hidden. For physical slots the Full Publish toggle sits on a second
        // line (full_line_item) visible only when enabled; for mixed slots Enable alone implies
        // publishing the mix definition, so no Full Publish widget exists at all.
        ::CheckBox* enable_check{nullptr};
        wxSizerItem* full_line_item{nullptr}; // sizer item of the Full Publish line (physical slots only)
        // "Full Publish": while checked, the whole slot preset is serialized and its rows are
        // disabled.
        ::CheckBox* full_check{nullptr};
        // The slot's filament preset picker (the sidebar's own combo), on its own line below Full
        // Publish and visible only while enabled. Selecting a preset stages it; OK assigns it.
        PublishFilamentPicker* filament_picker{nullptr};
        wxSizerItem*           filament_picker_item{nullptr};
        // True for a mixed-color filament slot: no Material/Retraction rows; Enable publishes
        // the slot's gradient/ratio definition as a whole.
        bool is_mixed{false};
        // The file column this category starts on; -1 when the owner has no variant layout. The
        // hosted tab's switch picks the shown column, with this as the fallback.
        int column{-1};
        // A filament slot's file columns, in preset order (empty without a variant layout).
        std::vector<size_t> columns;
        // The Q1 notice: this printer needs a variant the filament does not define.
        wxStaticText* variant_notice{nullptr};

        // The private preset's config as the hosted tab was first created (the seed plus the Tab's
        // own initial normalization). commit_publish_edits() diffs the tab's edits against it.
        Slic3r::DynamicPrintConfig seed;
        // Material identity, only for Section::Material categories.
        std::string filament_type;
        std::string filament_vendor;
        std::string filament_id;
        // The author's 0-based filament slot this material section represents.
        size_t filament_slot{0};
        // The slot's preset as the project uses it, and the one the picker staged. They differ
        // only between a pick and OK; Cancel leaves `filament` in place.
        std::string filament;
        std::string pending_filament;
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
        // Holds every category in order and stays the selection model. Hidden on the Printer and
        // Process sections, which have one hosted page each; there the tab's own strip (pages and
        // extruder switch) is the visible navigation.
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
    // "Enable" not checked on the component. An enabled component publishes its type and colour
    // implicitly, and colour is deliberately not required anyway (the receiver renders the mix
    // from its own components' colours). Sorted by mixed slot, then component slot.
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
    // Creates the slot's private Filament tab from `pending_filament` and hands it to the
    // category's host (spec 7.4.2). Used once at build and again whenever the picker stages
    // another preset; the previous tab is destroyed by the caller.
    void build_material_tab(Category& category);
    // Picker callback: stage `preset_name` for the physical slot, rebuild its tab and rows,
    // uncheck them and refresh the payload identity. Nothing of the project changes until OK.
    void on_filament_picked(size_t slot, const std::string& preset_name);
    // Drops a category's rows and re-indexes every other category's rows.
    void remove_category_rows(size_t category_index);
    // Recomputes the file-column layout of every material slot from the staged presets, remaps
    // the rows' "#col" keys and keeps the selection state on the remapped ids.
    void refresh_material_layout();
    // The uncompacted full config the project would have if every staged preset were assigned.
    Slic3r::DynamicPrintConfig staged_full_config(bool apply_extruder) const;
    // Appends one row drawn by the hosted tab's own Line.
    size_t add_tab_row(Row row, size_t category_index);
    // Builds a physical slot's rows from its hosted Filament tab's lines, and attaches the publish
    // checkbox to each line they came from.
    void add_material_tab_rows(size_t category_index);
    // Builds the hosted Printer page's rows from its tab's lines: one per variant column, keyed
    // "base#column", with the checkbox attached to the shared line.
    void add_printer_tab_rows(size_t category_index);
    // Builds the hosted Process page's rows from its tab's lines: a per-variant line gets one row
    // per extruder column ("base#column"), every other line one row over its whole keys. The
    // checkbox is attached to the shared line.
    void add_print_tab_rows(size_t category_index);
    // The variant column the hosted Printer or Process tab's extruder switch shows, or -1 when
    // unknown.
    int tab_active_column(Tab* tab) const;
    // The row of `line` shown for the variant the tab's switch currently selects, or -1.
    size_t tab_row_for_line(size_t category_index, const Line* line) const;
    // Whether the tab's own rules keep the row's line shown (its hide rules run natively).
    bool tab_line_visible(const Tab& tab, const Row& row) const;
    // Whether a row belongs to the variant currently shown (slot-level rows always do).
    bool row_on_selected_variant(size_t row_index) const;
    // Pushes one row's model state into its line's row toggle, if the widgets exist.
    void sync_row_widgets(size_t row_index);
    void sync_all_row_widgets();
    // Colours the row's label the way the Tab marks a modified setting.
    void apply_row_label_colour(size_t row_index);
    void apply_row_label_colours();
    // The line a row is drawn by.
    Line* row_line(size_t row_index) const;
    // Model-backed row state: the payload and the widgets both read this, never each other.
    void set_row_checked(size_t row_index, bool checked);
    void set_row_enabled(size_t row_index, bool enabled);
    // Whether a row's value can be written to the project (spec 7.10): a material slot must be
    // enabled and not Full Publish, and the row must resolve to a real write target.
    bool row_writable(size_t row_index) const;
    // Enables the line's fields while the row it draws is checked and writable, and disables them
    // otherwise. A row of a column the tab does not show leaves the line alone.
    void apply_line_field_gating(size_t row_index);
    // Applies the gating of every row of the category to the tab's fields.
    void apply_row_field_gating(size_t category_index);
    // Writes every checked row's edited value into the project's preset (or project config). Runs on
    // OK only, so Cancel leaves the app's presets untouched.
    void commit_publish_edits();
    // The non-structural filament keys of a slot's preset, for a "Full Publish" entry.
    std::vector<std::string> full_keys_for_slot() const;
    void save_scroll_position(Category& category);
    void show_outer_page(size_t section_index);
    void show_inner_page(size_t section_index, int inner_index);
    void show_mixed_page(size_t section_index, int mixed_index);
    void on_outer_tab_changed(wxCommandEvent& event);
    void on_inner_tab_changed(size_t section_index, wxCommandEvent& event);
    void on_mixed_tab_changed(size_t section_index, wxCommandEvent& event);
    bool row_is_visible(size_t row_index) const;
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
