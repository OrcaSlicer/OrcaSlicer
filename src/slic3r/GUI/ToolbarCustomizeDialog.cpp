#include "ToolbarCustomizeDialog.hpp"

#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "KeyChord.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "ToolbarLayout.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"
#include "format.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/AppConfig.hpp"

#include <wx/colour.h>
#include <wx/control.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/toplevel.h>
#include <wx/window.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

wxString toolbar_item_name(const std::string& id)
{
    struct Name
    {
        const char* text;
        const char* context = nullptr;
    };
    // Same msgids and contexts as the toolbar tooltips and gizmo names, so both read the same when translated.
    static const std::map<std::string, Name> names{
        {TOOLBAR_SEPARATOR_ID, {L("Separator")}},
        {"add", {L("Add")}},
        {"addplate", {L("Add plate")}},
        {"orient", {L("Auto orient")}},
        {"arrange", {L("Arrange")}},
        {"more", {L("Add instance")}},
        {"fewer", {L("Remove instance")}},
        {"splitobjects", {L("Split to objects")}},
        {"splitvolumes", {L("Split to parts")}},
        {"layersediting", {L("Variable layer height")}},
        {"move", {L("Move")}},
        {"rotate", {L("Rotate")}},
        {"scale", {L_CONTEXT("Scale", "Verb"), "Verb"}},
        {"flatten", {L("Lay on Face")}},
        {"cut", {L_CONTEXT("Cut", "Cut tool"), "Cut tool"}},
        {"mesh_boolean", {L("Mesh Boolean")}},
        {"fdm_supports", {L("Support Painting")}},
        {"seam", {L("Seam painting")}},
        {"fuzzy_skin", {L("Paint-on fuzzy skin")}},
        {"mmu_segmentation", {L("Color Painting")}},
        {"painted_modifier", {L("Painted modifier")}},
        {"texture_displacement", {L("Texture displacement")}},
        {"emboss", {L("Emboss")}},
        {"measure", {L("Measure")}},
        {"assembly", {L_CONTEXT("Assemble", "Assembly tool"), "Assembly tool"}},
        {"brim_ears", {L("Brim Ears")}},
    };
    const auto it = names.find(id);
    if (it == names.end())
        return wxString::FromUTF8(id);
    return it->second.context != nullptr ? I18N::translate(it->second.text, it->second.context) : I18N::translate(it->second.text);
}

// Asks for a gizmo group's name.
class GroupNameDialog : public DPIDialog
{
public:
    GroupNameDialog(wxWindow* parent, const wxString& name)
        : DPIDialog(parent, wxID_ANY, _L("Group name"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    {
        SetBackgroundColour(*wxWHITE);
        SetFont(Label::Body_14);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_input     = new ::TextInput(this, name, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(280), -1), wxTE_PROCESS_ENTER);
        m_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        sizer->Add(m_input, 0, wxEXPAND | wxALL, FromDIP(12));
        auto* btns = new DialogButtons(this, {"OK", "Cancel"});
        btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        sizer->Add(btns, 0, wxEXPAND);
        SetSizerAndFit(sizer);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
        m_input->GetTextCtrl()->SetFocus();
        m_input->GetTextCtrl()->SelectAll();
    }

    wxString value() const { return m_input->GetTextCtrl()->GetValue(); }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        m_input->Rescale();
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
    ::TextInput* m_input{nullptr};
};

class ToolbarCustomizeDialog : public DPIDialog
{
public:
    explicit ToolbarCustomizeDialog(wxWindow* parent)
        : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Customize Toolbar"),
                    wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
        , m_loaded(parse_toolbar_layout(wxGetApp().app_config->get("toolbar", "layout")))
        , m_working(m_loaded)
    {
        SetBackgroundColour(*wxWHITE);
        SetFont(Label::Body_14);

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* hint  = new Label(this, _L("Uncheck an icon to hide it. Select a row and move it to change the order."));
        sizer->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto* lists = new wxBoxSizer(wxHORIZONTAL);
        m_main_panel  = add_list(lists, _L("Object tools"));
        m_gizmo_panel = add_list(lists, _L("Gizmos"));
        add_group_buttons();
        sizer->Add(lists, 1, wxEXPAND | wxALL, FromDIP(12));

        auto* btns = new DialogButtons(this, {L("Reset to default"), "OK", "Cancel"}, "", 1 /*left_aligned*/);
        btns->GetButtonFromIndex(0)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_working = stock_toolbar_layout();
            m_selection.clear();
            show_working_layout();
        });
        btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        sizer->Add(btns, 0, wxEXPAND);

        // Alt+Up and Alt+Down move the selected row, so rows can be reordered without the mouse; Tab selects a
        // row by focusing its box.
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
            if (e.GetKeyCode() == WXK_TAB)
                m_tabbing = true;
            const KeyChord chord = KeyChord::from_event(e);
            if (chord == move_up_key() || chord == move_down_key()) {
                // The dialog opens with focus on a box but nothing selected; act on that box's row.
                if (m_selection.empty() && m_rows.count(m_focused_row) != 0)
                    select_row(m_focused_row);
                move_selected(chord == move_up_key() ? -1 : 1);
            }
            else
                e.Skip();
        });

        rebuild_rows();
        // Rows built later, after the dialog is shown, measure taller on GTK; they take the heights measured now.
        m_name_height = m_measured_height;
        SetSizerAndFit(sizer);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    const ToolbarLayout& loaded() const { return m_loaded; }
    const ToolbarLayout& working() const { return m_working; }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        m_name_height     = 0;
        m_measured_height = 0;
        m_name_sizes.clear();
        for (Button* btn : {m_group_button, m_rename_button, m_ungroup_button})
            btn->Rescale();
        rebuild_rows();
        m_name_height = m_measured_height;
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
    // One row of either list. Gizmo rows also carry what they show: a plain gizmo, a group header or a member.
    struct Row
    {
        wxPanel*        panel{nullptr};
        ::CheckBox*     check{nullptr};
        Button*         up{nullptr};
        Button*         down{nullptr};
        Label*          name{nullptr};
        bool            gizmo{false};
        ToolbarGizmoRow model{ToolbarRowKind::Plain, {}, -1};
    };

    // What the gizmo rows in the selection are: plain gizmos (in list order) and the groups they touch.
    struct GizmoSelection
    {
        std::vector<std::string> plain;
        std::set<int>            groups;
        int                      headers{0};
    };

    wxPanel* add_list(wxBoxSizer* lists, const wxString& title)
    {
        auto* column = new wxBoxSizer(wxVERTICAL);
        column->Add(new Label(this, Label::Head_14, title), 0, wxBOTTOM, FromDIP(6));
        auto* panel = new wxPanel(this);
        panel->SetBackgroundColour(*wxWHITE);
        panel->SetSizer(new wxBoxSizer(wxVERTICAL));
        column->Add(panel, 1, wxEXPAND);
        lists->Add(column, 1, wxEXPAND | wxRIGHT, FromDIP(16));
        return panel;
    }

    // Under the Gizmos list, acting on its selection.
    void add_group_buttons()
    {
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto  add     = [this, buttons](const wxString& text, void (ToolbarCustomizeDialog::*action)()) {
            auto* btn = new Button(this, text);
            btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            btn->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent&) { (this->*action)(); });
            buttons->Add(btn, 0, wxRIGHT, FromDIP(6));
            return btn;
        };
        m_group_button   = add(_L("Group selected"), &ToolbarCustomizeDialog::group_selected);
        m_rename_button  = add(_L("Rename"), &ToolbarCustomizeDialog::rename_selected);
        m_ungroup_button = add(_L("Ungroup"), &ToolbarCustomizeDialog::ungroup_selected);
        // TRN Tooltip of the "Group selected" button. %1% and %2% are key names such as "Ctrl" and "Shift".
        m_group_button->SetToolTip(format_wxstr(_L("%1%+click or %2%+click gizmos to select several, then group them under one toolbar button."),
                                                from_u8(KeyChord::modifier_name(wxMOD_CONTROL)), from_u8(KeyChord::modifier_name(wxMOD_SHIFT))));
        m_gizmo_panel->GetContainingSizer()->Add(buttons, 0, wxTOP, FromDIP(8));
    }

    static KeyChord move_up_key() { return {WXK_UP, wxMOD_ALT}; }
    static KeyChord move_down_key() { return {WXK_DOWN, wxMOD_ALT}; }

    ToolbarGroupLayout& group_of(const std::string& id)
    {
        return std::count(m_working.main.order.begin(), m_working.main.order.end(), id) != 0 ? m_working.main : m_working.gizmos;
    }

    std::string row_key(const ToolbarGizmoRow& row) const
    {
        return row.kind == ToolbarRowKind::Header ? "#group:" + m_working.groups[static_cast<size_t>(row.group)].members.front() : row.id;
    }

    // A group's name as a label: user text, so an "&" in it is not taken as a mnemonic.
    wxString group_label(int group) const { return wxControl::EscapeMnemonics(from_u8(m_working.groups[static_cast<size_t>(group)].name)); }

    std::string single_selection() const { return m_selection.size() == 1 ? m_selection.front() : std::string(); }
    bool        is_selected(const std::string& key) const { return std::find(m_selection.begin(), m_selection.end(), key) != m_selection.end(); }

    GizmoSelection gizmo_selection() const
    {
        GizmoSelection out;
        for (const ToolbarGizmoRow& row : gizmo_rows(m_working)) {
            if (!is_selected(row_key(row)))
                continue;
            if (row.kind == ToolbarRowKind::Plain)
                out.plain.push_back(row.id);
            else
                out.groups.insert(row.group);
            if (row.kind == ToolbarRowKind::Header)
                ++out.headers;
        }
        return out;
    }

    void move_selected(int step)
    {
        const std::string key = single_selection();
        if (key.empty() || m_rows.count(key) == 0)
            return;
        if (m_rows.at(key).gizmo) {
            if (move_gizmo_row(m_working, m_rows.at(key).model, step))
                sync_gizmo_rows();
            return;
        }
        std::vector<std::string>& order = group_of(key).order;
        const auto it = std::find(order.begin(), order.end(), key);
        if (it == order.end())
            return;
        const std::ptrdiff_t from = it - order.begin();
        const std::ptrdiff_t to   = from + step;
        if (to < 0 || to >= static_cast<std::ptrdiff_t>(order.size()))
            return;
        std::swap(order[static_cast<size_t>(from)], order[static_cast<size_t>(to)]);

        // Move the row itself rather than rebuilding the lists, which flickers.
        wxWindow* row  = m_rows.at(key).panel;
        wxWindow* list = row->GetParent();
        list->GetSizer()->Detach(row);
        list->GetSizer()->Insert(static_cast<size_t>(to), row, row_flags());
        follow_in_tab_order(list, static_cast<size_t>(to));
        list->Layout();
        update_arrows();
    }

    // Only a single selected row shows its arrows, so they plainly move that row; each is enabled only when the
    // row can move that way.
    void update_arrows()
    {
        const std::string selected = single_selection();
        for (const auto& [key, row] : m_rows) {
            const bool show = key == selected;
            row.up->Show(show);
            row.down->Show(show);
            if (!show)
                continue;
            bool up = false, down = false;
            if (row.gizmo) {
                ToolbarLayout probe = m_working;
                up                  = move_gizmo_row(probe, row.model, -1);
                probe               = m_working;
                down                = move_gizmo_row(probe, row.model, 1);
            } else {
                const std::vector<std::string>& order = m_working.main.order;
                const auto it = std::find(order.begin(), order.end(), key);
                up            = it != order.end() && it != order.begin();
                down          = it != order.end() && std::next(it) != order.end();
            }
            // Button draws its icon the same when disabled, so the icon shows the state.
            row.up->Enable(up);
            row.up->SetIcon(up ? "toolbar_row_up" : "toolbar_row_up_disabled");
            row.down->Enable(down);
            row.down->SetIcon(down ? "toolbar_row_down" : "toolbar_row_down_disabled");
        }
    }

    void update_group_buttons()
    {
        const GizmoSelection sel = gizmo_selection();
        m_group_button->Enable((sel.groups.empty() && sel.plain.size() >= 2) || (sel.groups.size() == 1 && !sel.plain.empty()));
        m_rename_button->Enable(m_selection.size() == 1 && sel.headers == 1);
        m_ungroup_button->Enable(sel.groups.size() == 1);
    }

    // A header's box is ticked when all its members show, half-ticked when only some do.
    void update_header_boxes()
    {
        for (const auto& [key, row] : m_rows) {
            if (!row.gizmo || row.model.kind != ToolbarRowKind::Header)
                continue;
            const std::vector<std::string>& members = m_working.groups[static_cast<size_t>(row.model.group)].members;
            const auto hidden = std::count_if(members.begin(), members.end(), [this](const std::string& id) { return m_working.gizmos.hidden.count(id) != 0; });
            row.check->SetValue(hidden == 0);
            row.check->SetHalfChecked(hidden > 0 && hidden < static_cast<std::ptrdiff_t>(members.size()));
        }
    }

    // Keeps Tab moving through the rows in the order they are shown.
    static void follow_in_tab_order(wxWindow* list, size_t index)
    {
        wxSizer* rows = list->GetSizer();
        if (index > 0)
            rows->GetItem(index)->GetWindow()->MoveAfterInTabOrder(rows->GetItem(index - 1)->GetWindow());
        else if (rows->GetItemCount() > 1)
            rows->GetItem(index)->GetWindow()->MoveBeforeInTabOrder(rows->GetItem(1)->GetWindow());
    }

    wxSizerFlags row_flags() const { return wxSizerFlags().Expand().Border(wxBOTTOM, FromDIP(2)); }

    // Restyles only the rows whose selection changed; rebuilding them all on each click flickers.
    void set_selection(std::vector<std::string> keys)
    {
        const std::vector<std::string> old = std::exchange(m_selection, std::move(keys));
        for (const std::vector<std::string>* list : {&old, static_cast<const std::vector<std::string>*>(&m_selection)})
            for (const std::string& key : *list)
                if (m_rows.count(key) != 0)
                    style_row(m_rows.at(key));
        update_arrows();
        update_group_buttons();
    }

    void select_row(const std::string& key)
    {
        m_anchor = key;
        set_selection({key});
    }

    // Ctrl+click adds or removes a gizmo row, Shift+click selects the gizmo rows from the last one clicked.
    void click_row(const std::string& key, const wxMouseEvent& e)
    {
        const bool gizmo = m_rows.at(key).gizmo;
        if (gizmo && e.CmdDown()) {
            std::vector<std::string> keys;
            for (const std::string& k : m_selection)
                if (m_rows.count(k) != 0 && m_rows.at(k).gizmo && k != key)
                    keys.push_back(k);
            if (!is_selected(key))
                keys.push_back(key);
            m_anchor = key;
            set_selection(std::move(keys));
            return;
        }
        if (gizmo && e.ShiftDown() && m_rows.count(m_anchor) != 0 && m_rows.at(m_anchor).gizmo) {
            std::vector<std::string> order;
            for (const ToolbarGizmoRow& row : gizmo_rows(m_working))
                order.push_back(row_key(row));
            auto a = std::find(order.begin(), order.end(), m_anchor);
            auto b = std::find(order.begin(), order.end(), key);
            if (a > b)
                std::swap(a, b);
            set_selection(std::vector<std::string>(a, std::next(b)));
            return;
        }
        select_row(key);
    }

    void style_row(const Row& row)
    {
        const bool     selected = is_selected(row.gizmo ? row_key(row.model) : row.model.id);
        const wxColour bg       = StateColor::darkModeColorFor(wxColour(selected ? "#BFE1DE" : "#FFFFFF"));
        row.panel->SetBackgroundColour(bg);
        for (wxWindow* child : row.panel->GetChildren())
            child->SetBackgroundColour(bg);
        // A header's name is always bold.
        row.name->SetFont(selected || row.model.kind == ToolbarRowKind::Header ? Label::Head_14 : Label::Body_14);
        row.panel->Layout();
        row.panel->Refresh();
    }

    std::optional<std::string> ask_group_name(const std::string& current)
    {
        GroupNameDialog dlg(this, from_u8(current));
        if (dlg.ShowModal() != wxID_OK)
            return std::nullopt;
        const std::string name = into_u8(dlg.value());
        if (name.find_first_not_of(" \t\r\n") != std::string::npos)
            return name;
        // TRN Default name of a new gizmo group in the Customize Toolbar dialog. %1% is a number.
        return default_group_name(m_working, [](int n) { return into_u8(format_wxstr(_L("Group %1%"), n)); });
    }

    void group_selected()
    {
        const GizmoSelection sel = gizmo_selection();
        if (sel.groups.empty() && sel.plain.size() >= 2) {
            const std::optional<std::string> name = ask_group_name(
                default_group_name(m_working, [](int n) { return into_u8(format_wxstr(_L("Group %1%"), n)); }));
            if (!name || !group_gizmos(m_working, sel.plain, *name))
                return;
            m_selection = {"#group:" + sel.plain.front()};
            m_anchor    = m_selection.front();
        } else if (sel.groups.size() == 1 && !sel.plain.empty()) {
            if (!add_to_group(m_working, *sel.groups.begin(), sel.plain))
                return;
        } else
            return;
        sync_gizmo_rows();
    }

    void rename_selected()
    {
        const std::string key = single_selection();
        if (m_rows.count(key) == 0 || m_rows.at(key).model.kind != ToolbarRowKind::Header)
            return;
        const int                        group = m_rows.at(key).model.group;
        const std::optional<std::string> name  = ask_group_name(m_working.groups[static_cast<size_t>(group)].name);
        if (name && rename_group(m_working, group, *name))
            sync_gizmo_rows();
    }

    void ungroup_selected()
    {
        const GizmoSelection sel = gizmo_selection();
        if (sel.groups.size() == 1 && ungroup_gizmos(m_working, *sel.groups.begin()))
            sync_gizmo_rows();
    }

    // Brings the Gizmos rows in line with m_working: in place when the same rows remain (a move, a rename,
    // Reset without groups), rebuilt when rows come or go (a group made, ungrouped or dissolved).
    void sync_gizmo_rows()
    {
        const wxWindowUpdateLocker   no_repaint(this);
        const std::vector<ToolbarGizmoRow> rows = gizmo_rows(m_working);
        std::vector<std::string>     keys;
        for (const ToolbarGizmoRow& row : rows)
            keys.push_back(row_key(row));
        std::set<std::string> existing;
        for (const auto& [key, row] : m_rows)
            if (row.gizmo)
                existing.insert(key);

        const bool rebuilt = existing != std::set<std::string>(keys.begin(), keys.end());
        bool grew = false;
        if (!rebuilt) {
            wxSizer* sizer = m_gizmo_panel->GetSizer();
            for (size_t i = 0; i < keys.size(); ++i) {
                Row& row = m_rows.at(keys[i]);
                // A row joining or leaving a group gains or loses its indent, which may need a wider dialog.
                if (row.model.kind != rows[i].kind)
                    grew = true;
                row.model = rows[i];
                sizer->Detach(row.panel);
                sizer->Insert(i, row.panel, row_flags());
                follow_in_tab_order(m_gizmo_panel, i);
                if (row.model.kind == ToolbarRowKind::Header) {
                    row.name->SetLabel(group_label(row.model.group));
                    // A longer name after Rename needs a wider row.
                    row.name->InvalidateBestSize();
                    const int width = row.name->GetBestSize().x + FromDIP(6);
                    if (width > row.name->GetMinSize().x) {
                        row.name->SetMinSize(wxSize(width, row.name->GetMinSize().y));
                        grew = true;
                    }
                } else
                    row.check->SetValue(m_working.gizmos.hidden.count(row.model.id) == 0);
                set_indent(row);
                style_row(row);
            }
            m_gizmo_panel->Layout();
            if (grew)
                GetSizer()->SetSizeHints(this);
        } else {
            Freeze();
            for (auto it = m_rows.begin(); it != m_rows.end();)
                it = it->second.gizmo ? m_rows.erase(it) : std::next(it);
            fill_gizmo_list();
            Layout();
            // Rows came or went: fit the dialog to them.
            GetSizer()->SetSizeHints(this);
            wxGetApp().UpdateDlgDarkUI(this);
            Thaw();
        }

        std::vector<std::string> kept;
        for (const std::string& key : m_selection)
            if (m_rows.count(key) != 0)
                kept.push_back(key);
        update_header_boxes();
        set_selection(std::move(kept));
        // A rebuild destroys the box that had the keyboard focus, and with it Alt+Up/Down; give it to the
        // selected row's box, or to the dialog.
        if (rebuilt) {
            const std::string key = single_selection();
            if (m_rows.count(key) != 0)
                m_rows.at(key).check->SetFocus();
            else
                SetFocus();
        }
    }

    // Puts the existing rows in m_working's order and state. Rows built after the dialog is shown measure
    // 1px taller on GTK, so rebuilding them here would push the last row out of the dialog.
    void show_working_layout()
    {
        const wxWindowUpdateLocker no_repaint(this);
        wxSizer*                   rows  = m_main_panel->GetSizer();
        const ToolbarGroupLayout&  group = m_working.main;
        for (size_t i = 0; i < group.order.size(); ++i) {
            const auto it = m_rows.find(group.order[i]);
            if (it == m_rows.end() || it->second.gizmo || rows->GetItemCount() != group.order.size()) {
                rebuild_rows();
                return;
            }
            wxWindow* row = it->second.panel;
            rows->Detach(row);
            rows->Insert(i, row, row_flags());
            follow_in_tab_order(m_main_panel, i);
            it->second.check->SetValue(group.hidden.count(group.order[i]) == 0);
            style_row(it->second);
        }
        m_main_panel->Layout();
        sync_gizmo_rows();
    }

    void rebuild_rows()
    {
        Freeze();
        m_rows.clear();
        fill_main_list();
        fill_gizmo_list();
        update_header_boxes();
        update_arrows();
        update_group_buttons();
        // The rows keep their sizes, so the dialog does too; refitting it here made it grow.
        if (GetSizer() != nullptr)
            Layout();
        wxGetApp().UpdateDlgDarkUI(this);
        Thaw();
    }

    void fill_main_list()
    {
        m_main_panel->DestroyChildren();
        for (const std::string& id : m_working.main.order)
            add_row(m_main_panel, id, toolbar_item_name(id), false, {ToolbarRowKind::Plain, id, -1}, m_working.main.hidden.count(id) == 0);
        m_main_panel->Layout();
    }

    void fill_gizmo_list()
    {
        m_gizmo_panel->DestroyChildren();
        for (const ToolbarGizmoRow& row : gizmo_rows(m_working)) {
            const wxString text = row.kind == ToolbarRowKind::Header ? group_label(row.group) : toolbar_item_name(row.id);
            add_row(m_gizmo_panel, row_key(row), text, true, row, m_working.gizmos.hidden.count(row.id) == 0);
        }
        m_gizmo_panel->Layout();
    }

    // Members are indented by their name, not their box, so the boxes stay in one column.
    void set_indent(const Row& row)
    {
        if (wxSizerItem* indent = row.panel->GetSizer()->GetItem(static_cast<size_t>(1)); indent != nullptr && indent->IsSpacer())
            indent->AssignSpacer(row.model.kind == ToolbarRowKind::Member ? FromDIP(16) : 0, 0);
    }

    void toggle_box(const std::string& key)
    {
        Row& row = m_rows.at(key);
        if (row.gizmo && row.model.kind == ToolbarRowKind::Header) {
            // Shows every member if any is hidden, else hides them all.
            const std::vector<std::string>& members = m_working.groups[static_cast<size_t>(row.model.group)].members;
            const bool any_hidden = std::any_of(members.begin(), members.end(), [this](const std::string& id) { return m_working.gizmos.hidden.count(id) != 0; });
            for (const std::string& id : members) {
                if (any_hidden)
                    m_working.gizmos.hidden.erase(id);
                else
                    m_working.gizmos.hidden.insert(id);
                if (m_rows.count(id) != 0)
                    m_rows.at(id).check->SetValue(any_hidden);
            }
        } else {
            ToolbarGroupLayout& g = row.gizmo ? m_working.gizmos : m_working.main;
            if (row.check->GetValue())
                g.hidden.erase(row.model.id);
            else
                g.hidden.insert(row.model.id);
        }
        update_header_boxes();
        select_row(key);
    }

    void add_row(wxPanel* panel, const std::string& key, const wxString& text, bool gizmo, const ToolbarGizmoRow& model, bool shown)
    {
        const GLCanvas3D* canvas = wxGetApp().plater() != nullptr ? wxGetApp().plater()->get_view3D_canvas3D() : nullptr;
        // A full-width row, so a click anywhere on it selects it.
        auto* row       = new wxPanel(panel);
        auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto* check = new ::CheckBox(row);
        check->SetValue(shown);
        check->Bind(wxEVT_TOGGLEBUTTON, [this, key](wxCommandEvent& e) {
            toggle_box(key);
            e.Skip();
        });
        // Not for the focus the dialog gives its first box on opening, so it opens with nothing selected.
        check->Bind(wxEVT_SET_FOCUS, [this, key](wxFocusEvent& e) {
            m_focused_row = key;
            if (m_tabbing)
                select_row(key);
            e.Skip();
        });
        row_sizer->Add(check, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
        if (gizmo)
            row_sizer->Add(0, 0);

        // Sized for the bold font, so selecting the row does not clip its name. GTK measures text differently once
        // the dialog is shown, so a row rebuilt later keeps the size measured when the dialog was built; a name
        // first seen later (a new group's) gets a small margin.
        auto*  name = new Label(row, Label::Head_14, text);
        wxSize best = name->GetBestSize();
        m_measured_height = std::max(m_measured_height, best.y);
        const std::string size_key = into_u8(text);
        if (m_name_height == 0)
            m_name_sizes[size_key] = best;
        else if (const auto known = m_name_sizes.find(size_key); known != m_name_sizes.end())
            best = known->second;
        else
            best = wxSize(best.x + FromDIP(6), m_name_height);
        name->SetMinSize(best);
        row_sizer->Add(name, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(3));

        if (model.kind != ToolbarRowKind::Header && model.id != TOOLBAR_SEPARATOR_ID && canvas != nullptr && !canvas->is_toolbar_item_available(model.id)) {
            // TRN Shown next to a toolbar icon the current printer or selection leaves off the toolbar.
            auto* note = new Label(row, Label::Body_12, _L("(not on the toolbar right now)"));
            note->SetForegroundColour(wxColour(144, 144, 144));
            row_sizer->Add(note, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        }

        // Hidden until the row is selected; their space is kept so the row does not shift when they show.
        auto add_arrow = [this, row, row_sizer](const char* icon, const wxString& tip, int step) {
            auto* btn = new Button(row, "", icon, 0, 16);
            btn->SetStyle(ButtonStyle::Regular, ButtonType::Icon);
            btn->SetPaddingSize(FromDIP(wxSize(2, 2)));
            btn->SetMinSize(FromDIP(wxSize(22, 22)));
            btn->SetCornerRadius(FromDIP(3));
            btn->SetToolTip(tip);
            // Mouse only: a focused arrow that gets disabled at the end of a list would hand the focus to
            // another control on Windows, and the next Space would act on that control.
            btn->SetCanFocus(false);
            // After the click: a move that dissolves a group rebuilds the rows, this arrow included.
            btn->Bind(wxEVT_BUTTON, [this, step](wxCommandEvent&) { CallAfter([this, step]() { move_selected(step); }); });
            btn->Hide();
            row_sizer->Add(btn, wxSizerFlags().CenterVertical().Border(wxRIGHT, FromDIP(4)).ReserveSpaceEvenIfHidden());
            return btn;
        };
        // TRN Tooltip of a row's arrow in the Customize Toolbar dialog. %1% is a key chord such as "Alt+Arrow Up".
        Button* up = add_arrow("toolbar_row_up_disabled", format_wxstr(_L("Move up (%1%)"), from_u8(move_up_key().display())), -1);
        // TRN Tooltip of a row's arrow in the Customize Toolbar dialog. %1% is a key chord such as "Alt+Arrow Down".
        Button* down = add_arrow("toolbar_row_down_disabled", format_wxstr(_L("Move down (%1%)"), from_u8(move_down_key().display())), 1);
        row->SetSizer(row_sizer);

        const auto on_click = [this, key, row, up](wxMouseEvent& e) {
            // A click where the hidden arrows sit does nothing: after a move the row slides out from under the
            // pointer, and a second click there would select the neighbour and a third would move it back.
            const bool on_arrows = e.GetEventObject() == row && e.GetX() >= up->GetPosition().x;
            if (!on_arrows)
                click_row(key, e);
            e.Skip();
        };
        // Double-clicking a group's header renames the group.
        const auto on_double_click = [this, key](wxMouseEvent& e) {
            if (m_rows.count(key) != 0 && m_rows.at(key).model.kind == ToolbarRowKind::Header) {
                select_row(key);
                // The name dialog is modal, so it opens after the mouse event.
                CallAfter([this]() { rename_selected(); });
                return;
            }
            e.Skip();
        };
        for (wxWindow* target : {static_cast<wxWindow*>(row), static_cast<wxWindow*>(name)}) {
            target->Bind(wxEVT_LEFT_DOWN, on_click);
            target->Bind(wxEVT_LEFT_DCLICK, on_double_click);
        }
        for (wxWindow* child : row->GetChildren())
            if (child != check && child != up && child != down && child != name)
                child->Bind(wxEVT_LEFT_DOWN, on_click);

        panel->GetSizer()->Add(row, row_flags());
        Row& added = m_rows[key];
        added      = {row, check, up, down, name, gizmo, model};
        set_indent(added);
        style_row(added);
    }

    ToolbarLayout            m_loaded;
    ToolbarLayout            m_working;
    std::vector<std::string> m_selection;           // row keys: gizmo or item ids, "#group:<first member>" for a header
    std::string              m_anchor;              // the row Shift+click selects from
    std::string              m_focused_row;         // the row whose box last took the focus
    bool                     m_tabbing{false};      // Tab has been pressed, so focusing a box selects its row
    int                      m_name_height{0};      // row name height measured when the dialog was built
    int                      m_measured_height{0};  // tallest row name built so far
    wxPanel*                 m_main_panel{nullptr};
    wxPanel*                 m_gizmo_panel{nullptr};
    Button*                  m_group_button{nullptr};
    Button*                  m_rename_button{nullptr};
    Button*                  m_ungroup_button{nullptr};
    std::map<std::string, Row> m_rows;
    std::map<std::string, wxSize> m_name_sizes; // row names' sizes measured when the dialog was built, by text
};

} // namespace

void show_toolbar_customize_dialog(wxWindow* parent)
{
    ToolbarCustomizeDialog dlg(parent);
    if (dlg.ShowModal() != wxID_OK || dlg.working() == dlg.loaded())
        return;

    // Reset sets the stock layout without unknown fields, which serialises to "": the key is then erased.
    const std::string value = serialize_toolbar_layout(dlg.working());
    if (value.empty())
        wxGetApp().app_config->erase("toolbar", "layout");
    else
        wxGetApp().app_config->set("toolbar", "layout", value);

    if (wxGetApp().plater() != nullptr)
        wxGetApp().plater()->get_view3D_canvas3D()->apply_toolbar_layout();
}

void show_toolbar_context_menu()
{
    // Not on the stack of the canvas's mouse handler: the menu, and the dialog after it, run their own event loops.
    wxGetApp().CallAfter([] {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr)
            return;
        wxMenu menu;
        append_menu_item(&menu, wxID_ANY, _L("Customize Toolbar..."), _L("Choose which toolbar icons are shown and in what order."),
                         [](wxCommandEvent&) { wxGetApp().CallAfter([] { show_toolbar_customize_dialog(wxGetApp().mainframe); }); }, "", &menu);
        plater->PopupMenu(&menu);
    });
}

}} // namespace Slic3r::GUI
