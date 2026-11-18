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
#include "format.hpp"
#include "libslic3r/AppConfig.hpp"

#include <wx/colour.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/tglbtn.h>
#include <wx/toplevel.h>
#include <wx/window.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
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
        sizer->Add(lists, 1, wxEXPAND | wxALL, FromDIP(12));

        auto* btns = new DialogButtons(this, {L("Reset to default"), "OK", "Cancel"}, "", 1 /*left_aligned*/);
        btns->GetButtonFromIndex(0)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_working = stock_toolbar_layout();
            m_selected.clear();
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
                if (m_selected.empty() && m_rows.count(m_focused_row) != 0)
                    select_row(m_focused_row);
                move_selected(chord == move_up_key() ? -1 : 1);
            }
            else
                e.Skip();
        });

        rebuild_rows();
        SetSizerAndFit(sizer);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    const ToolbarLayout& loaded() const { return m_loaded; }
    const ToolbarLayout& working() const { return m_working; }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        rebuild_rows();
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
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

    static KeyChord move_up_key() { return {WXK_UP, wxMOD_ALT}; }
    static KeyChord move_down_key() { return {WXK_DOWN, wxMOD_ALT}; }

    ToolbarGroupLayout& group_of(const std::string& id)
    {
        return std::count(m_working.main.order.begin(), m_working.main.order.end(), id) != 0 ? m_working.main : m_working.gizmos;
    }

    void move_selected(int step)
    {
        if (m_selected.empty())
            return;
        std::vector<std::string>& order = group_of(m_selected).order;
        const auto it = std::find(order.begin(), order.end(), m_selected);
        if (it == order.end())
            return;
        const std::ptrdiff_t from = it - order.begin();
        const std::ptrdiff_t to   = from + step;
        if (to < 0 || to >= static_cast<std::ptrdiff_t>(order.size()))
            return;
        std::swap(order[static_cast<size_t>(from)], order[static_cast<size_t>(to)]);

        // Move the row itself rather than rebuilding the lists, which flickers.
        wxWindow* row  = m_rows.at(m_selected).panel;
        wxWindow* list = row->GetParent();
        list->GetSizer()->Detach(row);
        list->GetSizer()->Insert(static_cast<size_t>(to), row, row_flags());
        follow_in_tab_order(list, static_cast<size_t>(to));
        list->Layout();
        update_arrows();
    }

    // Only the selected row shows its arrows, so they plainly move that row; each is enabled only when the
    // row can move that way.
    void update_arrows()
    {
        for (const auto& [id, row] : m_rows) {
            const bool selected = id == m_selected;
            row.up->Show(selected);
            row.down->Show(selected);
            if (!selected)
                continue;
            const std::vector<std::string>& order = group_of(id).order;
            const auto it   = std::find(order.begin(), order.end(), id);
            const bool up   = it != order.end() && it != order.begin();
            const bool down = it != order.end() && std::next(it) != order.end();
            // Button draws its icon the same when disabled, so the icon shows the state.
            row.up->Enable(up);
            row.up->SetIcon(up ? "toolbar_row_up" : "toolbar_row_up_disabled");
            row.down->Enable(down);
            row.down->SetIcon(down ? "toolbar_row_down" : "toolbar_row_down_disabled");
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

    // Restyles the old and new selected rows only; rebuilding them all on each click flickers.
    void select_row(const std::string& id)
    {
        const std::string old = std::exchange(m_selected, id);
        if (old != id && m_rows.count(old) != 0)
            style_row(m_rows.at(old).panel, false);
        style_row(m_rows.at(id).panel, true);
        update_arrows();
    }

    void style_row(wxWindow* row, bool selected)
    {
        const wxColour bg = StateColor::darkModeColorFor(wxColour(selected ? "#BFE1DE" : "#FFFFFF"));
        row->SetBackgroundColour(bg);
        for (wxWindow* child : row->GetChildren()) {
            child->SetBackgroundColour(bg);
            if (auto* name = dynamic_cast<Label*>(child); name != nullptr && name->GetName() == "name")
                name->SetFont(selected ? Label::Head_14 : Label::Body_14);
        }
        row->Layout();
        row->Refresh();
    }

    // Puts the existing rows in m_working's order and state. Rows built after the dialog is shown measure
    // 1px taller on GTK, so rebuilding them here would push the last row out of the dialog.
    void show_working_layout()
    {
        const wxWindowUpdateLocker no_repaint(this);
        for (const auto& [panel, group] : {std::make_pair(m_main_panel, &m_working.main), std::make_pair(m_gizmo_panel, &m_working.gizmos)}) {
            wxSizer* rows = panel->GetSizer();
            if (rows->GetItemCount() != group->order.size()) {
                rebuild_rows();
                return;
            }
            for (size_t i = 0; i < group->order.size(); ++i) {
                const auto it = m_rows.find(group->order[i]);
                if (it == m_rows.end() || it->second.panel->GetParent() != panel) {
                    rebuild_rows();
                    return;
                }
                wxWindow* row = it->second.panel;
                rows->Detach(row);
                rows->Insert(i, row, row_flags());
                follow_in_tab_order(panel, i);
                it->second.check->SetValue(group->hidden.count(group->order[i]) == 0);
                style_row(row, group->order[i] == m_selected);
            }
            panel->Layout();
        }
        update_arrows();
    }

    void rebuild_rows()
    {
        Freeze();
        m_rows.clear();
        fill_list(m_main_panel, m_working.main);
        fill_list(m_gizmo_panel, m_working.gizmos);
        update_arrows();
        // The rows keep their sizes, so the dialog does too; refitting it here made it grow.
        if (GetSizer() != nullptr)
            Layout();
        wxGetApp().UpdateDlgDarkUI(this);
        Thaw();
    }

    void fill_list(wxPanel* panel, ToolbarGroupLayout& group)
    {
        panel->DestroyChildren();
        wxSizer*          rows   = panel->GetSizer();
        const GLCanvas3D* canvas = wxGetApp().plater() != nullptr ? wxGetApp().plater()->get_view3D_canvas3D() : nullptr;
        for (const std::string& id : group.order) {
            // A full-width row, so a click anywhere on it selects it.
            auto* row       = new wxPanel(panel);
            auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);

            auto* check = new ::CheckBox(row);
            check->SetValue(group.hidden.count(id) == 0);
            check->Bind(wxEVT_TOGGLEBUTTON, [this, id, check](wxCommandEvent& e) {
                ToolbarGroupLayout& g = group_of(id);
                if (check->GetValue())
                    g.hidden.erase(id);
                else
                    g.hidden.insert(id);
                select_row(id);
                e.Skip();
            });
            // Not for the focus the dialog gives its first box on opening, so it opens with nothing selected.
            check->Bind(wxEVT_SET_FOCUS, [this, id](wxFocusEvent& e) {
                m_focused_row = id;
                if (m_tabbing)
                    select_row(id);
                e.Skip();
            });
            row_sizer->Add(check, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));

            // Sized for the bold font, so selecting the row does not clip its name.
            auto* name = new Label(row, Label::Head_14, toolbar_item_name(id));
            name->SetName("name");
            name->SetMinSize(name->GetBestSize());
            row_sizer->Add(name, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(3));

            if (id != TOOLBAR_SEPARATOR_ID && canvas != nullptr && !canvas->is_toolbar_item_available(id)) {
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
                btn->Bind(wxEVT_BUTTON, [this, step](wxCommandEvent&) { move_selected(step); });
                btn->Hide();
                row_sizer->Add(btn, wxSizerFlags().CenterVertical().Border(wxRIGHT, FromDIP(4)).ReserveSpaceEvenIfHidden());
                return btn;
            };
            // TRN Tooltip of a row's arrow in the Customize Toolbar dialog. %1% is a key chord such as "Alt+Arrow Up".
            Button* up = add_arrow("toolbar_row_up_disabled", format_wxstr(_L("Move up (%1%)"), from_u8(move_up_key().display())), -1);
            // TRN Tooltip of a row's arrow in the Customize Toolbar dialog. %1% is a key chord such as "Alt+Arrow Down".
            Button* down = add_arrow("toolbar_row_down_disabled", format_wxstr(_L("Move down (%1%)"), from_u8(move_down_key().display())), 1);
            row->SetSizer(row_sizer);

            const auto on_click = [this, id, row, up](wxMouseEvent& e) {
                // A click where the hidden arrows sit does nothing: after a move the row slides out from under the
                // pointer, and a second click there would select the neighbour and a third would move it back.
                const bool on_arrows = e.GetEventObject() == row && e.GetX() >= up->GetPosition().x;
                if (!on_arrows)
                    select_row(id);
                e.Skip();
            };
            row->Bind(wxEVT_LEFT_DOWN, on_click);
            for (wxWindow* child : row->GetChildren())
                if (child != check && child != up && child != down)
                    child->Bind(wxEVT_LEFT_DOWN, on_click);

            style_row(row, id == m_selected);
            rows->Add(row, row_flags());
            m_rows[id] = {row, check, up, down};
        }
        panel->Layout();
    }

    ToolbarLayout m_loaded;
    ToolbarLayout m_working;
    std::string   m_selected;
    std::string   m_focused_row;    // the row whose box last took the focus
    bool          m_tabbing{false}; // Tab has been pressed, so focusing a box selects its row
    wxPanel*      m_main_panel{nullptr};
    wxPanel*      m_gizmo_panel{nullptr};
    struct Row
    {
        wxPanel*    panel{nullptr};
        ::CheckBox* check{nullptr};
        Button*     up{nullptr};
        Button*     down{nullptr};
    };
    std::map<std::string, Row> m_rows;
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

}} // namespace Slic3r::GUI
