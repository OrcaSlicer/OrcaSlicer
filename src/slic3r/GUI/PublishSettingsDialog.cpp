#include "PublishSettingsDialog.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "Tab.hpp"
#include "ConfigValueFormatter.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PublishSettings.hpp"

#include <wx/graphics.h>
#include <boost/algorithm/string/trim.hpp>
#include <set>
#include <algorithm>
#include <functional>

// Custom-painted collapse chevron: a vector path (down when expanded, right
// when collapsed) drawn in the dialog's secondary-text grey. Vector drawing
// keeps it crisp at any DPI (the earlier 16px bitmap chevron looked
// blurry/wide).
class CollapseChevron : public wxWindow
{
public:
    explicit CollapseChevron(wxWindow* parent) : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundColour(parent->GetBackgroundColour());
        DisableFocusFromKeyboard();
        SetMinSize(FromDIP(wxSize(10, 10)));
        Bind(wxEVT_PAINT, &CollapseChevron::on_paint, this);
    }

    void SetCollapsed(bool collapsed)
    {
        if (m_collapsed == collapsed)
            return;
        m_collapsed = collapsed;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        wxGraphicsContext* ctx = wxGraphicsContext::Create(dc);
        if (ctx == nullptr)
            return;
        ctx->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        // Same grey as the row value labels, dark-mode aware.
        wxPen pen(StateColor::darkModeColorFor(wxColour("#6B6B6B")), FromDIP(1.5), wxPENSTYLE_SOLID);
        pen.SetCap(wxCAP_ROUND);
        pen.SetJoin(wxJOIN_ROUND);
        ctx->SetPen(pen);

        const wxSize sz = GetClientSize();
        const double cx = sz.x / 2.0;
        const double cy = sz.y / 2.0;
        const double r  = std::min(sz.x, sz.y) * 0.32;

        wxGraphicsPath path = ctx->CreatePath();
        if (m_collapsed) {
            // Right-pointing chevron ">".
            path.MoveToPoint(cx - r, cy - r);
            path.AddLineToPoint(cx + r, cy);
            path.AddLineToPoint(cx - r, cy + r);
        } else {
            // Down-pointing chevron "v".
            path.MoveToPoint(cx - r, cy - r);
            path.AddLineToPoint(cx, cy + r);
            path.AddLineToPoint(cx + r, cy - r);
        }
        ctx->StrokePath(path);
        delete ctx;
    }

    bool m_collapsed{false};
};

namespace Slic3r { namespace GUI {
namespace {

// Identity of a filament slot: the stable material id when present, else the
// type+vendor pair. Used to emit the PublishedMaterialEntry identity fields.
struct MaterialIdentity
{
    std::string type;
    std::string vendor;
    std::string id;
};

// Menu ids for show_menu(). Dedicated range above the standard ids so the popup cannot
// collide with application-level bindings (e.g. MainFrame's recent-files wxID_FILE1.. range).
enum {
    kPublishSelectAll        = wxID_HIGHEST + 1,
    kPublishDeselectAll,
    kPublishSelectVisible,
    kPublishDeselectVisible,
    kPublishFilterSelected,
    kPublishFilterNonSelected
};

MaterialIdentity material_identity(size_t slot, const DynamicPrintConfig& full)
{
    MaterialIdentity identity;
    if (const auto* types = full.opt<ConfigOptionStrings>("filament_type"))
        if (slot < types->size())
            identity.type = types->get_at(slot);
    if (const auto* vendors = full.opt<ConfigOptionStrings>("filament_vendor"))
        if (slot < vendors->size())
            identity.vendor = vendors->get_at(slot);
    if (const auto* ids = full.opt<ConfigOptionStrings>("filament_ids"))
        if (slot < ids->size())
            identity.id = ids->get_at(slot);
    return identity;
}

// "Generic PLA @System" -> "Generic PLA"; mirrors the alias derivation in
// PresetBundle::load_vendor_configs_from_json (PresetBundle.cpp) and
// PresetCollection::set_custom_preset_alias (Preset.cpp).
std::string material_display_name(const std::string& preset_name)
{
    const size_t at = preset_name.find_first_of('@');
    if (at == std::string::npos)
        return preset_name;
    std::string bare = preset_name.substr(0, at);
    boost::trim_right(bare);
    return bare.empty() ? preset_name : bare;
}

// Human-readable section title for a filament slot: the resolved preset name,
// falling back to the filament type, then to the generic "Material".
wxString material_title(size_t slot, const PresetBundle* bundle, const DynamicPrintConfig& full)
{
    if (slot < bundle->filament_presets.size()) {
        const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[slot]);
        if (preset != nullptr && !preset->name.empty())
            return from_u8(material_display_name(preset->name));
    }
    const MaterialIdentity identity = material_identity(slot, full);
    if (!identity.type.empty())
        return from_u8(identity.type);
    return _L("Material");
}

} // namespace

PublishSettingsDialog::PublishSettingsDialog(wxWindow* parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Publish Settings"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    , m_search(this, "search", 16)
    , m_menu(this, "filter", 16)
{
    SetBackgroundColour(*wxWHITE);

    build_option_model();

    // --- filter bar: search box, All/None, menu button ---
    wxPanel* f_bar = new wxPanel(this, wxID_ANY);
    f_bar->SetBackgroundColour(GetBackgroundColour());
    wxBoxSizer* f_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_filter_box = new TextInput(f_bar, "", "", "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_filter_box->SetIcon(m_search.bmp());
    m_filter_box->SetMinSize(FromDIP(wxSize(200, 24)));
    m_filter_box->SetSize(FromDIP(wxSize(-1, 24)));
    m_filter_box->SetFocus();
    m_filter_ctrl = m_filter_box->GetTextCtrl();
    m_filter_ctrl->SetFont(Label::Body_13);
    m_filter_ctrl->SetSize(wxSize(-1, FromDIP(16))); // centers text vertically
    m_filter_ctrl->SetHint(_L("Type to filter..."));
    m_filter_ctrl->Bind(wxEVT_TEXT, [this](auto&) { apply_filter(m_filter_ctrl->GetValue()); });
    m_filter_ctrl->Bind(wxEVT_TEXT_ENTER, [this](auto&) { apply_filter(m_filter_ctrl->GetValue()); });
    m_filter_ctrl->Bind(wxEVT_SET_FOCUS, [this](auto& e) {
        apply_filter(m_filter_ctrl->GetValue());
        e.Skip();
    });
    m_filter_ctrl->Bind(wxEVT_KILL_FOCUS, [this](auto& e) {
        apply_filter(m_filter_ctrl->GetValue());
        e.Skip();
    });
    f_sizer->Add(m_filter_box, 1, wxEXPAND);
    Bind(wxEVT_SET_FOCUS, [this](auto&) { m_filter_box->SetFocus(); });

    m_fb_sizer      = new wxBoxSizer(wxHORIZONTAL);
    auto create_btn = [this, f_bar](wxString title, bool select) {
        auto btn = new wxStaticText(f_bar, wxID_ANY, title);
        btn->SetForegroundColour("#009687");
        btn->SetCursor(wxCURSOR_HAND);
        btn->SetFont(Label::Body_13);
        btn->Bind(wxEVT_LEFT_DOWN, [this, select](wxMouseEvent&) { select_all(select); });
        m_fb_sizer->Add(btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    };
    f_sizer->Add(m_fb_sizer, 0, wxALIGN_CENTER_VERTICAL);
    create_btn(_L("All"), true);
    create_btn(_L("None"), false);

    m_menu_button = new wxStaticBitmap(f_bar, wxID_ANY, m_menu.bmp());
    m_menu_button->SetCursor(wxCURSOR_HAND);
    m_menu_button->Bind(wxEVT_LEFT_DOWN, &PublishSettingsDialog::show_menu, this);
    f_sizer->Add(m_menu_button, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    f_bar->SetSizerAndFit(f_sizer);

    wxBoxSizer* w_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* msg = new wxStaticText(this, wxID_ANY, _L("Select which settings to embed in the 3MF file"));
    msg->SetFont(Label::Body_13);
    msg->Wrap(-1);
    w_sizer->Add(msg, 0, wxRIGHT | wxLEFT | wxTOP, FromDIP(10));

    w_sizer->Add(f_bar, 0, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));
    w_sizer->Add(m_scroll, 1, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // At least one checked, enabled (publishable) key is required. A gated
        // material row is disabled even if its value is pre-checked, so it must
        // not count.
        for (const Row& row : m_rows)
            if (row.check->GetValue() && row.check->IsEnabled()) {
                EndModal(wxID_OK);
                return;
            }
        MessageDialog(this, _L("No settings selected. Please select at least one setting to publish."), _L("Publish Settings"),
                      wxOK | wxICON_WARNING)
            .ShowModal();
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    w_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizerAndFit(w_sizer);
    SetMinSize(FromDIP(wxSize(600, 500)));
    SetSize(FromDIP(wxSize(600, 500))); // initial size only; the dialog is resizable
    wxGetApp().UpdateDlgDarkUI(this);
}

PublishSettingsDialog::~PublishSettingsDialog() {}

void PublishSettingsDialog::build_option_model()
{
    // Structural / non-publishable keys, shared with the published-3MF overlay
    // path (see libslic3r/PublishSettings.hpp).
    const std::set<std::string>& denylist = publish_structural_keys();
    // Base keys already added in the print/printer sections. Printer rows share
    // this set: a base key appears once (per-extruder "#N" variants collapse to
    // the first occurrence - acceptable MVP; the per-extruder context is lost).
    std::set<std::string> added;

    PresetBundle* bundle    = wxGetApp().preset_bundle;
    DynamicPrintConfig full = bundle->full_config();

    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scroll->SetScrollRate(0, 10);
    m_scroll->SetBackgroundColour(GetBackgroundColour());
    m_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_scroll->SetSizer(m_list_sizer);
    m_scroll->DisableFocusFromKeyboard();
    m_scroll->Bind(wxEVT_RIGHT_DOWN, &PublishSettingsDialog::show_menu, this);

    // "no matching rows" info label, shown by apply_filter().
    m_info = new wxStaticText(m_scroll, wxID_ANY, "");
    m_info->SetFont(Label::Body_13);
    m_list_sizer->Add(m_info, 1, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(10));
    m_info->Hide();
    m_info_nonsel = _L("No selected items...");
    m_info_allsel = _L("All items selected...");
    m_info_empty  = _L("No matching items...");

    // Shared per-option label/value computation; returns false when the option
    // must be skipped (denylisted / unknown / empty label). value is the pure
    // stringified value; unit is the translated sidetext (may be empty).
    auto option_text = [&denylist, &full](const std::string& opt_id, const std::string& pure_key, wxString& label, wxString& value,
                                          wxString& unit) -> bool {
        if (denylist.count(pure_key) > 0)
            return false;
        const ConfigOptionDef* def = print_config_def.get(pure_key);
        if (def == nullptr)
            return false;
        label = _(def->full_label.empty() ? def->label : def->full_label);
        if (label.IsEmpty())
            return false;
        value = get_string_value(opt_id, full);
        unit  = _(def->sidetext);
        return true;
    };

    // Find-or-create a main category; builds its header row UI on first use.
    // Material sections are additionally matched by identity and slot so two
    // identities (or slots) that happen to share a title stay separate.
    auto category_index_for = [this, &full](const wxString& title, Section section, const std::string& icon_name, size_t group,
                                            const MaterialIdentity& identity = MaterialIdentity(), size_t slot = 0) -> size_t {
        for (size_t i = 0; i < m_categories.size(); ++i) {
            if (m_categories[i].title != title || m_categories[i].section != section)
                continue;
            if (section == Section::Material &&
                (m_categories[i].filament_id != identity.id || m_categories[i].filament_type != identity.type ||
                 m_categories[i].filament_vendor != identity.vendor || m_categories[i].filament_slot != slot))
                continue;
            return i;
        }
        Category cat;
        cat.title           = title;
        cat.section         = section;
        cat.group           = group;
        cat.icon_name       = icon_name;
        cat.filament_type   = identity.type;
        cat.filament_vendor = identity.vendor;
        cat.filament_id     = identity.id;
        cat.filament_slot   = slot;
        if (!icon_name.empty()) {
            cat.icon_bmp = ScalableBitmap(m_scroll, icon_name, 18);
            cat.icon = new wxStaticBitmap(m_scroll, wxID_ANY, cat.icon_bmp.bmp());
        }
        if (section == Section::Material) {
            // Material header: [master (title)][slim tri-state select-all].
            // The master is a 2-state opt-in that carries the material title;
            // the tri-state is label-less and gates on the master.
            cat.master_check = new wxCheckBox(m_scroll, wxID_ANY, title);
            cat.master_check->SetFont(Label::Head_14);
            cat.master_check->SetToolTip(_L("Export this material"));
            cat.header = new wxCheckBox(m_scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxCHK_3STATE);
            cat.header->SetFont(Label::Head_14);
            cat.header->SetToolTip(_L("Select/deselect all settings in this material"));
        } else {
            cat.header = new wxCheckBox(m_scroll, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxCHK_3STATE);
            cat.header->SetFont(Label::Head_14.Bold());
        }
        const size_t new_index = m_categories.size();
        cat.chevron            = create_chevron(m_scroll, wxEVT_LEFT_DOWN, [this, new_index] { toggle_category(new_index); });
        // [icon][chevron][(chip)(master)(tri-state) | checkbox]: the chevron
        // collapses/expands the category, the checkbox is the select-all.
        auto header_sizer = new wxBoxSizer(wxHORIZONTAL);
        if (cat.icon != nullptr)
            header_sizer->Add(cat.icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
        header_sizer->Add(cat.chevron, 0, wxALIGN_CENTER_VERTICAL);
        if (section == Section::Material) {
            // Per-slot colour chip, before the master title.
            std::string hex;
            if (const auto* colours = full.opt<ConfigOptionStrings>("filament_colour"))
                if (slot < colours->size())
                    hex = colours->get_at(slot);
            wxBitmap* chip_bmp = get_extruder_color_icon(hex, "", FromDIP(12), FromDIP(12));
            // chip_bmp points into get_extruder_color_icon's static BitmapCache
            // and must NOT be deleted; the wxStaticBitmap takes its own copy.
            header_sizer->Add(new wxStaticBitmap(m_scroll, wxID_ANY, *chip_bmp), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
            header_sizer->Add(cat.master_check, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
            header_sizer->Add(cat.header, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        } else {
            header_sizer->Add(cat.header, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(4));
        }
        // A wrapper sizer splits the vertical separation (TOP 10) from the
        // horizontal indent (LEFT|RIGHT 22), so the top gap collapses with the
        // header when it is hidden.
        auto wrap = new wxBoxSizer(wxVERTICAL);
        wrap->Add(header_sizer, 0, wxTOP, FromDIP(10));
        cat.item = m_list_sizer->Add(wrap, 0, wxLEFT | wxRIGHT, FromDIP(22));
        // Register the new category with its section group: the group's visibility
        // and select-all logic iterates section.categories.
        m_sections[group].categories.push_back(new_index);
        m_categories.push_back(std::move(cat));
        return new_index;
    };

    // Find-or-create a subcategory (optgroup) heading within a category.
    auto subcategory_index_for = [this](size_t cat_index, const wxString& title, const wxString& icon) -> size_t {
        Category& cat = m_categories[cat_index];
        for (size_t i = 0; i < cat.subs.size(); ++i)
            if (cat.subs[i].title == title)
                return i;
        Subcategory sub;
        sub.title = title;
        if (!title.IsEmpty()) {
            // Same look as the Tab's optgroup headers (incl. its icon), plus a
            // collapse chevron. A click on the chevron bitmap does not reach the
            // StaticLine, so both are bound to the same toggle (LEFT_UP so the
            // StaticLine's label acts as the click target too).
            sub.header = new ::StaticLine(m_scroll, false, title, icon);
            sub.header->SetFont(Label::Head_14);
            sub.header->SetForegroundColour("#363636");
            sub.header->SetCursor(wxCURSOR_HAND);
            const size_t new_index = cat.subs.size();
            auto toggle            = [this, cat_index, new_index] { toggle_subcategory(cat_index, new_index); };
            sub.header->Bind(wxEVT_LEFT_UP, [toggle](wxMouseEvent&) { toggle(); });
            sub.chevron       = create_chevron(m_scroll, wxEVT_LEFT_UP, toggle);
            auto header_sizer = new wxBoxSizer(wxHORIZONTAL);
            header_sizer->Add(sub.chevron, 0, wxALIGN_CENTER_VERTICAL);
            header_sizer->Add(sub.header, 1, wxEXPAND | wxLEFT, FromDIP(4));
            // A wrapper sizer splits the vertical separation (TOP|BOTTOM 6) from
            // the horizontal indent (LEFT|RIGHT 38), so the gaps collapse with
            // the header when it is hidden.
            auto wrap = new wxBoxSizer(wxVERTICAL);
            wrap->Add(header_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
            sub.item = m_list_sizer->Add(wrap, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(38));
        }
        cat.subs.push_back(std::move(sub));
        return cat.subs.size() - 1;
    };

    // Creates the row UI (checkbox + white ellipsized value + grey unit) and
    // registers the row into the given category/subcategory.
    auto add_row_ui = [this](const std::string& key, const wxString& label, const wxString& value, const wxString& unit, size_t cat_index,
                             size_t sub_index) {
        Row row;
        row.key                = key;
        row.label              = label;
        row.value              = value;
        row.unit               = unit;
        row.category           = m_categories[cat_index].title;
        row.subcategory        = m_categories[cat_index].subs[sub_index].title;
        row.section            = m_categories[cat_index].section;
        row.section_title      = m_sections[m_categories[cat_index].group].title;
        const size_t row_index = m_rows.size();
        m_rows.push_back(std::move(row));

        Row& r  = m_rows[row_index];
        r.check = new wxCheckBox(m_scroll, wxID_ANY, label, wxDefaultPosition, wxDefaultSize);
        r.check->SetFont(Label::Body_13);
        // Value in the Tab's text color (near-black light / #EFEFF0 dark); the
        // unit is the grey secondary text.
        r.value_label = new wxStaticText(m_scroll, wxID_ANY, value, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        r.value_label->SetFont(Label::Body_13);
        r.value_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
        r.value_label->SetToolTip(unit.IsEmpty() ? value : value + " " + unit);
        if (!unit.IsEmpty()) {
            r.unit_label = new wxStaticText(m_scroll, wxID_ANY, unit);
            r.unit_label->SetFont(Label::Body_13);
            r.unit_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
        }

        auto row_sizer = new wxBoxSizer(wxHORIZONTAL);
        row_sizer->Add(r.check, 0, wxALIGN_CENTER_VERTICAL);
        row_sizer->Add(r.value_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        if (r.unit_label != nullptr)
            row_sizer->Add(r.unit_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
        r.item = m_list_sizer->Add(row_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(54));

        m_categories[cat_index].rows.push_back(row_index);
        m_categories[cat_index].subs[sub_index].rows.push_back(row_index);
    };

    // --- Phase 1: printer per-extruder retraction settings (displayed first,
    // mirroring the sidebar's Printer group). The printer tab's
    // "Extruder"/"Extruder N" pages carry the per-extruder retraction options.
    {
        size_t g = section_group_for(Section::Printer);
        for (Tab* tab : wxGetApp().tabs_list) {
            if (tab->m_type != Preset::TYPE_PRINTER)
                continue;
            for (const PageShp& page : tab->m_pages) {
                if (!page->title().StartsWith("Extruder"))
                    continue;
                const wxString page_title = Tab::translate_category(page->title(), tab->m_type);
                for (const ConfigOptionsGroupShp& optgroup : page->m_optgroups) {
                    // Allowlist on the untranslated optgroup title; the "Retraction
                    // when switching material" group is intentionally skipped.
                    if (optgroup->title != "Retraction" && optgroup->title != "Z-Hop")
                        continue;
                    const wxString subcategory = page_title + L" \u00B7 " + _(optgroup->title);
                    for (const auto& opt : optgroup->opt_map()) {
                        const std::string& opt_id   = opt.first;
                        const std::string& pure_key = opt.second.first;
                        // Per-extruder "#N" variants collapse to the first base key. The row stores
                        // the BASE key (whole-vector semantics on load: the size-guarded apply
                        // copies the author's full vector), while the "#0" opt_id is only used to
                        // display the first extruder's value.
                        if (!added.insert(pure_key).second)
                            continue;
                        wxString label, value, unit;
                        if (!option_text(opt_id, pure_key, label, value, unit))
                            continue;
                        size_t cat_index = category_index_for(_L("Retraction & Z-hop"), Section::Printer, "custom-gcode_extruder", g);
                        size_t sub_index = subcategory_index_for(cat_index, subcategory, optgroup->icon);
                        add_row_ui(pure_key, label, value, unit, cat_index, sub_index);
                    }
                }
            }
        }
    }

    // --- Phase 2: per-material sections synthesized from the filament tab's
    // "Setting Overrides" page, under the Filament group.
    {
        size_t g          = section_group_for(Section::Material);
        Tab* filament_tab = nullptr;
        for (Tab* tab : wxGetApp().tabs_list)
            if (tab->m_type == Preset::TYPE_FILAMENT) {
                filament_tab = tab;
                break;
            }
        if (filament_tab != nullptr) {
            const Page* overrides_page = nullptr;
            for (const PageShp& page : filament_tab->m_pages)
                if (page->title() == "Setting Overrides") {
                    overrides_page = page.get();
                    break;
                }

            if (overrides_page != nullptr) {
                // One section per filament slot: a 4-slot printer (e.g. 1 PLA +
                // 3 PETG) shows 4 sections, each disambiguated by its colour chip
                // and slot number. The "· Slot N" title suffix keeps the category
                // titles unique across slots.
                for (size_t slot = 0; slot < bundle->filament_presets.size(); ++slot) {
                    const MaterialIdentity identity = material_identity(slot, full);
                    const wxString title            = material_title(slot, bundle, full) + L" \u00B7 " +
                                                      wxString::Format(_L("Slot %d"), static_cast<int>(slot) + 1);
                    // A material section must not repeat a key; the same key may
                    // appear in other material sections - that is intended.
                    std::set<std::string> material_added;

                    for (const ConfigOptionsGroupShp& optgroup : overrides_page->m_optgroups) {
                        // Allowlist on the untranslated optgroup title; the
                        // "Ironing" group is intentionally skipped.
                        if (optgroup->title != "Retraction" && optgroup->title != "Retraction when switching material")
                            continue;
                        for (const auto& opt : optgroup->opt_map()) {
                            // Row keys are base keys (no "#N"): the load side
                            // matches the material and uses the author's slot.
                            const std::string& opt_id = opt.first;
                            std::string base          = opt_id.substr(0, opt_id.find('#'));
                            if (!material_added.insert(base).second)
                                continue;
                            // Show the value of this slot; fall back to slot 0 if out of range.
                            std::string value_opt_id = base + "#" + std::to_string(slot);
                            if (const ConfigOption* opt_cfg = full.option(base))
                                if (const auto* vec = dynamic_cast<const ConfigOptionVectorBase*>(opt_cfg))
                                    if (vec->size() > 0 && slot >= vec->size())
                                        value_opt_id = base + "#0";
                            wxString label, value, unit;
                            if (!option_text(value_opt_id, base, label, value, unit))
                                continue;
                            size_t cat_index = category_index_for(title, Section::Material, "custom-gcode_filament", g, identity, slot);
                            size_t sub_index = subcategory_index_for(cat_index, _(optgroup->title), optgroup->icon);
                            add_row_ui(base, label, value, unit, cat_index, sub_index);
                        }
                    }
                }
            }
        }
    }

    // --- Phase 3: process (print) settings; grouping matches the process tab.
    {
        size_t g = section_group_for(Section::Print);
        for (Tab* tab : wxGetApp().tabs_list) {
            if (tab->m_type != Preset::TYPE_PRINT)
                continue;
            const auto& icon_map = tab->get_category_icon_map();
            for (const PageShp& page : tab->m_pages) {
                wxString category = Tab::translate_category(page->title(), tab->m_type);
                // Page icon, keyed by the untranslated page title (per-Tab map).
                std::string icon_name;
                auto icon_it = icon_map.find(page->title());
                if (icon_it != icon_map.end())
                    icon_name = icon_it->second;

                for (const ConfigOptionsGroupShp& optgroup : page->m_optgroups) {
                    for (const auto& opt : optgroup->opt_map()) {
                        // opt_map() key is the opt_id (may carry "#N"); the value is
                        // (pure_opt_key, opt_index).
                        const std::string& opt_id   = opt.first;
                        const std::string& pure_key = opt.second.first;
                        // A key may appear in more than one page/group; keep the first.
                        if (!added.insert(pure_key).second)
                            continue;
                        wxString label, value, unit;
                        if (!option_text(opt_id, pure_key, label, value, unit))
                            continue;
                        size_t cat_index = category_index_for(category, Section::Print, icon_name, g);
                        size_t sub_index = subcategory_index_for(cat_index, _(optgroup->title), optgroup->icon);
                        add_row_ui(opt_id, label, value, unit, cat_index, sub_index);
                    }
                }
            }
        }
    }

    // Pre-check the dirty (modified) settings and mark them bold. The base-key
    // match covers all sections; collect_dirty_settings_keys already unions the
    // prints, printers and filaments of the bundle.
    std::set<std::string> dirty_base;
    for (const std::string& key : collect_dirty_settings_keys(*wxGetApp().preset_bundle)) {
        auto n = key.find('#');
        dirty_base.insert(n == std::string::npos ? key : key.substr(0, n));
    }
    for (Row& row : m_rows) {
        std::string base = row.key.substr(0, row.key.find('#'));
        row.dirty        = dirty_base.count(base) > 0;
        if (row.dirty) {
            row.check->SetValue(true);
            set_row_bold(row, true);
        }
    }

    // Wire the section group tri-state headers (chevrons/StaticLine toggles were
    // bound at creation in section_group_for).
    for (size_t s = 0; s < m_sections.size(); ++s) {
        if (m_sections[s].header != nullptr) {
            m_sections[s].header->Bind(wxEVT_CHECKBOX, [this, s](wxCommandEvent&) { on_section_toggle(s); });
            update_section_header(m_sections[s]);
        }
    }

    // Wire the tri-state headers: clicking a header toggles all its children;
    // toggling any child re-syncs its header. Bind by index so the lambdas stay
    // valid even if the vectors are reallocated later.
    for (size_t c = 0; c < m_categories.size(); ++c) {
        if (m_categories[c].master_check != nullptr)
            m_categories[c].master_check->Bind(wxEVT_CHECKBOX, [this, c](wxCommandEvent&) { on_master_toggle(c); });
        m_categories[c].header->Bind(wxEVT_CHECKBOX, [this, c](wxCommandEvent&) { on_category_toggle(c); });
        for (size_t r : m_categories[c].rows)
            m_rows[r].check->Bind(wxEVT_CHECKBOX, [this, c](wxCommandEvent&) { update_category_header(m_categories[c]); });
        update_category_header(m_categories[c]);
    }

    // Material sections start gated (master OFF): their rows and tri-state are
    // disabled until the author opts the material in.
    for (size_t c = 0; c < m_categories.size(); ++c)
        if (m_categories[c].section == Section::Material)
            on_master_toggle(c);

    // No filter is active at startup: every row matches until the user types.
    for (Row& row : m_rows)
        row.matches_filter = true;
    apply_visibility();

    m_scroll->FitInside();
    m_list_sizer->Layout();
}

size_t PublishSettingsDialog::section_group_for(Section kind)
{
    for (size_t i = 0; i < m_sections.size(); ++i)
        if (m_sections[i].kind == kind)
            return i;

    SectionGroup section;
    section.kind           = kind;
    const size_t new_index = m_sections.size();

    switch (kind) {
    case Section::Printer:
        section.title     = _L("Printer");
        section.icon_name = "printer";
        break;
    case Section::Material:
        section.title     = _L("Filament");
        section.icon_name = "filament";
        break;
    case Section::Print:
        section.title     = _L("Process");
        section.icon_name = "process";
        break;
    }

    if (!section.icon_name.empty()) {
        section.icon_bmp = ScalableBitmap(m_scroll, section.icon_name, 18);
        section.icon = new wxStaticBitmap(m_scroll, wxID_ANY, section.icon_bmp.bmp());
    }

    section.chevron = create_chevron(m_scroll, wxEVT_LEFT_DOWN, [this, new_index] { toggle_section(new_index); });

    auto header_sizer = new wxBoxSizer(wxHORIZONTAL);
    if (section.icon != nullptr)
        header_sizer->Add(section.icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    header_sizer->Add(section.chevron, 0, wxALIGN_CENTER_VERTICAL);

    if (kind == Section::Material) {
        // Filament group: clickable StaticLine title, no tri-state (each
        // material below opts in individually).
        section.header_line = new ::StaticLine(m_scroll, false, section.title);
        section.header_line->SetFont(Label::Head_14.Bold());
        section.header_line->SetForegroundColour("#363636");
        section.header_line->SetCursor(wxCURSOR_HAND);
        section.header_line->SetToolTip(_L("Enable each material below to export its settings"));
        auto toggle = [this, new_index] { toggle_section(new_index); };
        section.header_line->Bind(wxEVT_LEFT_UP, [toggle](wxMouseEvent&) { toggle(); });
        header_sizer->Add(section.header_line, 1, wxEXPAND | wxLEFT, FromDIP(4));
    } else {
        // Printer/Process: tri-state select-all carries the title.
        section.header = new wxCheckBox(m_scroll, wxID_ANY, section.title, wxDefaultPosition, wxDefaultSize, wxCHK_3STATE);
        section.header->SetFont(Label::Head_14.Bold());
        header_sizer->Add(section.header, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(4));
    }

    // A wrapper sizer splits the larger vertical separation (TOP 14) from the
    // shallow horizontal indent (LEFT|RIGHT 6), so the top gap collapses with
    // the header when it is hidden. wxEXPAND lets the Filament StaticLine's
    // separator span the width like the subcategory headers.
    auto wrap = new wxBoxSizer(wxVERTICAL);
    wrap->Add(header_sizer, 0, wxEXPAND | wxTOP, FromDIP(14));
    section.item = m_list_sizer->Add(wrap, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
    m_sections.push_back(std::move(section));
    return new_index;
}

void PublishSettingsDialog::on_category_toggle(size_t category_index)
{
    Category& cat = m_categories[category_index];
    // Defensive: a gated material header is disabled and cannot fire.
    if (cat.section == Section::Material && !cat.master)
        return;
    // A click on the header toggles between "all" and "none": if every child is
    // checked, uncheck all; otherwise check all.
    bool all_checked = true;
    for (size_t r : cat.rows)
        if (!m_rows[r].check->GetValue()) {
            all_checked = false;
            break;
        }
    bool value = !all_checked;
    for (size_t r : cat.rows)
        m_rows[r].check->SetValue(value);
    update_category_header(cat);
}

void PublishSettingsDialog::on_master_toggle(size_t category_index)
{
    Category& cat = m_categories[category_index];
    cat.master    = cat.master_check->GetValue();
    for (size_t r : cat.rows)
        m_rows[r].check->Enable(cat.master);
    cat.header->Enable(cat.master);
    update_category_header(cat);
}

void PublishSettingsDialog::on_section_toggle(size_t section_index)
{
    SectionGroup& section = m_sections[section_index];
    if (section.header == nullptr)
        return; // defensive: the Filament group has no select-all
    // All-or-none over every enabled row in the group's categories.
    bool all_checked = true;
    for (size_t c : section.categories) {
        for (size_t r : m_categories[c].rows)
            if (m_rows[r].check->IsEnabled() && !m_rows[r].check->GetValue()) {
                all_checked = false;
                break;
            }
        if (!all_checked)
            break;
    }
    const bool value = !all_checked;
    for (size_t c : section.categories)
        for (size_t r : m_categories[c].rows)
            if (m_rows[r].check->IsEnabled())
                m_rows[r].check->SetValue(value);
    for (size_t c : section.categories)
        update_category_header(m_categories[c]);
    update_section_header(section);
}

void PublishSettingsDialog::update_section_header(SectionGroup& section)
{
    if (section.header == nullptr)
        return; // Filament group has no tri-state.
    int checked = 0;
    int total   = 0;
    for (size_t c : section.categories) {
        for (size_t r : m_categories[c].rows) {
            if (!m_rows[r].check->IsEnabled())
                continue; // gated material rows don't count (defensive)
            ++total;
            if (m_rows[r].check->GetValue())
                ++checked;
        }
    }
    if (total == 0 || checked == 0)
        section.header->Set3StateValue(wxCHK_UNCHECKED);
    else if (checked == total)
        section.header->Set3StateValue(wxCHK_CHECKED);
    else
        section.header->Set3StateValue(wxCHK_UNDETERMINED);
}

void PublishSettingsDialog::update_category_header(Category& category)
{
    // A gated material section's tri-state must not reflect the preserved
    // (greyed-out) row values.
    if (category.section == Section::Material && !category.master) {
        category.header->Set3StateValue(wxCHK_UNCHECKED);
        return;
    }
    int checked = 0;
    for (size_t r : category.rows)
        if (m_rows[r].check->GetValue())
            ++checked;

    if (checked == 0)
        category.header->Set3StateValue(wxCHK_UNCHECKED);
    else if (checked == static_cast<int>(category.rows.size()))
        category.header->Set3StateValue(wxCHK_CHECKED);
    else
        category.header->Set3StateValue(wxCHK_UNDETERMINED);
}

void PublishSettingsDialog::set_row_bold(Row& row, bool bold)
{
    // Real set/clear: rebase on the dialog's body font so that clearing bold
    // restores the exact original font (the old CheckList::SetBold was one-way).
    row.check->SetFont(bold ? Label::Body_13.Bold() : Label::Body_13);
}

void PublishSettingsDialog::apply_filter(const wxString& filter_text)
{
    Freeze();
    wxString filter = filter_text.Lower();

    // Pseudo filters (menu only): show only checked ("::sel") or only
    // unchecked ("::nonsel") rows.
    const bool pseudo = (filter == "::sel" || filter == "::nonsel");
    m_fb_sizer->Show(!pseudo);

    // Update the per-row match flags only; actual visibility is computed by
    // apply_visibility() (which also respects the collapse state).
    if (pseudo) {
        if (m_filter_ctrl->GetValue().Lower() != filter) {
            m_filter_ctrl->SetValue(filter);
            m_filter_ctrl->SetSelection(0, -1);
        }
        const bool want_checked = (filter == "::sel");
        for (Row& row : m_rows)
            row.matches_filter = row.check->IsEnabled() && row.check->GetValue() == want_checked;
    } else {
        const bool clear = filter.IsEmpty();
        for (Row& row : m_rows) {
            row.matches_filter = clear || (row.section_title + " " + row.category + " " + row.subcategory + " " + row.label + " " +
                                           row.value + " " + row.unit)
                                              .Lower()
                                              .Contains(filter);
        }
    }

    // The info label reflects the filter result only; a collapsed section
    // hiding its matches is a user choice, not "no match".
    m_info->Show();
    for (const Row& row : m_rows) {
        if (row.matches_filter) {
            m_info->Hide();
            break;
        }
    }
    if (m_info->IsShown())
        m_info->SetLabel(pseudo ? (filter == "::sel" ? m_info_nonsel : m_info_allsel) : m_info_empty);

    apply_visibility();
    m_scroll->FitInside();
    Layout();
    Thaw();
}

void PublishSettingsDialog::apply_visibility()
{
    Freeze();
    for (SectionGroup& section : m_sections) {
        // The section header stays visible whenever any row in the group matches
        // (a section header never depends on its own collapsed state).
        bool section_any = false;
        for (size_t c : section.categories) {
            for (size_t r : m_categories[c].rows)
                if (m_rows[r].matches_filter) {
                    section_any = true;
                    break;
                }
            if (section_any)
                break;
        }
        section.item->Show(section_any);
        section.chevron->SetCollapsed(section.collapsed);

        for (size_t c : section.categories) {
            Category& cat = m_categories[c];

            // The category header stays visible whenever it has any match and
            // the section is not collapsed, so it can always be re-expanded.
            bool cat_any = false;
            for (size_t r : cat.rows)
                if (m_rows[r].matches_filter) {
                    cat_any = true;
                    break;
                }
            cat.item->Show(cat_any && !section.collapsed);
            cat.chevron->SetCollapsed(cat.collapsed);

            for (Subcategory& sub : cat.subs) {
                if (sub.header != nullptr) {
                    // The subcategory header visibility depends on its rows' matches
                    // and on its ancestors, but NOT on its own collapsed state.
                    bool sub_any = false;
                    for (size_t r : sub.rows)
                        if (m_rows[r].matches_filter) {
                            sub_any = true;
                            break;
                        }
                    sub.item->Show(sub_any && !section.collapsed && !cat.collapsed);
                    sub.chevron->SetCollapsed(sub.collapsed);
                }
                // Rows are hidden by the filter and by any collapsed ancestor.
                for (size_t r : sub.rows)
                    m_rows[r].item->Show(m_rows[r].matches_filter && !section.collapsed && !cat.collapsed && !sub.collapsed);
            }
        }
    }
    Thaw();
}

void PublishSettingsDialog::toggle_section(size_t section_index)
{
    m_sections[section_index].collapsed = !m_sections[section_index].collapsed;
    apply_visibility();
    m_scroll->FitInside();
    m_list_sizer->Layout();
}

void PublishSettingsDialog::toggle_category(size_t category_index)
{
    m_categories[category_index].collapsed = !m_categories[category_index].collapsed;
    apply_visibility();
    m_scroll->FitInside();
    m_list_sizer->Layout();
}

void PublishSettingsDialog::toggle_subcategory(size_t category_index, size_t subcategory_index)
{
    m_categories[category_index].subs[subcategory_index].collapsed = !m_categories[category_index].subs[subcategory_index].collapsed;
    apply_visibility();
    m_scroll->FitInside();
    m_list_sizer->Layout();
}

CollapseChevron* PublishSettingsDialog::create_chevron(wxWindow* parent,
                                                       const wxEventTypeTag<wxMouseEvent>& event_type,
                                                       std::function<void()> toggle)
{
    CollapseChevron* chevron = new CollapseChevron(parent);
    chevron->SetCursor(wxCURSOR_HAND);
    // The tag type (not wxEventType) keeps Bind's EventTag template deduced as
    // wxEventTypeTag<wxMouseEvent>; wxEvent& is used so the helper works with
    // any mouse event tag, and the toggle itself does not inspect the event.
    chevron->Bind(event_type, [toggle](wxEvent&) { toggle(); });
    return chevron;
}

void PublishSettingsDialog::select_all(bool value)
{
    // "All" does not auto-enable gated material sections; "None" leaves a gated
    // row's preserved value untouched.
    for (Row& row : m_rows)
        if (row.check->IsEnabled())
            row.check->SetValue(value);
    for (Category& cat : m_categories)
        update_category_header(cat);
}

void PublishSettingsDialog::select_visible(bool value)
{
    wxString filter = m_filter_ctrl->GetValue().Lower();
    // In a pseudo-filter view the rows being toggled would all disappear;
    // drop the filter afterwards so the result stays visible.
    bool clear_pseudo = (!value && filter == "::nonsel") || (value && filter == "::sel");

    // Toggle the rows that are visible under the *current* filter.
    for (Row& row : m_rows)
        if (row.check->IsShown() && row.check->IsEnabled())
            row.check->SetValue(value);

    if (clear_pseudo) {
        // Note: SetValue() may fire wxEVT_TEXT on some platforms, which
        // re-enters apply_filter() - that is fine, the rows above were already
        // toggled and the trailing call below is idempotent.
        m_filter_ctrl->SetValue("");
        apply_filter(""); // resync visibility, headers and the All/None bar
    }
    for (Category& cat : m_categories)
        update_category_header(cat);
}

void PublishSettingsDialog::show_menu(wxMouseEvent& evt)
{
    bool filtering  = !m_filter_ctrl->GetValue().IsEmpty();
    bool list_empty = m_info->IsShown();

    wxMenu m;
    m.Append(kPublishSelectAll, _L("Select All"))->Enable(!filtering);
    m.Append(kPublishDeselectAll, _L("Deselect All"))->Enable(!filtering);
    m.AppendSeparator();
    m.Append(kPublishSelectVisible, _L("Select visible"))->Enable(!list_empty && filtering);
    m.Append(kPublishDeselectVisible, _L("Deselect visible"))->Enable(!list_empty && filtering);
    m.AppendSeparator();
    m.Append(kPublishFilterSelected, _L("Filter selected"));
    m.Append(kPublishFilterNonSelected, _L("Filter nonSelected"));

    m.Bind(
        wxEVT_MENU,
        [this](wxCommandEvent& e) {
            switch (e.GetId()) {
            case kPublishSelectAll: select_all(true); break;
            case kPublishDeselectAll: select_all(false); break;
            case kPublishSelectVisible: select_visible(true); break;
            case kPublishDeselectVisible: select_visible(false); break;
            case kPublishFilterSelected: apply_filter("::sel"); break;
            case kPublishFilterNonSelected: apply_filter("::nonsel"); break;
            default: break;
            }
        },
        kPublishSelectAll, kPublishFilterNonSelected);

    wxWindow* src = dynamic_cast<wxWindow*>(evt.GetEventObject());
    if (!src)
        return;
    wxPoint screen_pos = src->ClientToScreen(evt.GetPosition());
    wxPoint local_pos  = ScreenToClient(screen_pos);
    PopupMenu(&m, local_pos);
}

std::vector<std::string> PublishSettingsDialog::GetPublishedKeys() const
{
    std::vector<std::string> out;
    // Process and printer sections both travel through published_keys (the load-side
    // overlay applies process keys to the prints edited preset and the allowlisted
    // printer keys to the printers edited preset). Material keys use a separate API.
    for (const Row& row : m_rows)
        if ((row.section == Section::Print || row.section == Section::Printer) && row.check->GetValue())
            out.push_back(row.key);
    return out;
}

std::vector<Slic3r::PublishedMaterialEntry> PublishSettingsDialog::GetPublishedMaterialKeys() const
{
    std::vector<Slic3r::PublishedMaterialEntry> out;
    for (const Category& cat : m_categories) {
        // Only opted-in materials export their keys.
        if (cat.section != Section::Material || !cat.master)
            continue;
        Slic3r::PublishedMaterialEntry entry;
        entry.filament_type   = cat.filament_type;
        entry.filament_vendor = cat.filament_vendor;
        entry.filament_id     = cat.filament_id;
        entry.slot            = static_cast<int>(cat.filament_slot);
        for (size_t r : cat.rows)
            if (m_rows[r].check->GetValue())
                entry.keys.push_back(m_rows[r].key);
        // A section without any checked key carries no information for the writer.
        if (!entry.keys.empty())
            out.push_back(std::move(entry));
    }
    return out;
}

void PublishSettingsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    // Rescale toolbar bitmaps and icons; collapse chevrons are vector-drawn and repaint themselves.
    m_search.msw_rescale();
    m_menu.msw_rescale();
    m_filter_box->SetIcon(m_search.bmp());
    m_menu_button->SetBitmap(m_menu.bmp());

    for (SectionGroup& section : m_sections) {
        if (section.icon != nullptr && section.icon_bmp.bmp().IsOk()) {
            section.icon_bmp.msw_rescale();
            section.icon->SetBitmap(section.icon_bmp.bmp());
        }
        if (section.header_line != nullptr)
            section.header_line->Rescale();
    }

    for (Category& cat : m_categories) {
        if (cat.icon != nullptr && cat.icon_bmp.bmp().IsOk()) {
            cat.icon_bmp.msw_rescale();
            cat.icon->SetBitmap(cat.icon_bmp.bmp());
        }
        for (Subcategory& sub : cat.subs) {
            if (sub.header != nullptr)
                sub.header->Rescale();
        }
    }

    SetMinSize(FromDIP(wxSize(600, 500)));
    m_scroll->FitInside();
    m_list_sizer->Layout();
    Refresh();
}

}} // namespace Slic3r::GUI
