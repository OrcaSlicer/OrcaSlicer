#include "ToolbarCustomizeDialog.hpp"

#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "ToolbarLayout.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/AppConfig.hpp"

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/tglbtn.h>
#include <wx/toplevel.h>

#include <algorithm>
#include <cstddef>
#include <functional>
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

        auto* moves = new wxBoxSizer(wxHORIZONTAL);
        auto  add_button = [this, moves](const wxString& text, std::function<void()> action) {
            auto* btn = new Button(this, text);
            btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
            btn->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
            moves->Add(btn, 0, wxRIGHT, FromDIP(8));
            return btn;
        };
        add_button(_L("Move up"), [this]() { move_selected(-1); });
        add_button(_L("Move down"), [this]() { move_selected(1); });
        moves->AddStretchSpacer();
        add_button(_L("Reset to default"), [this]() {
            m_working = stock_toolbar_layout();
            m_selected.clear();
            rebuild_rows();
        });
        sizer->Add(moves, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

        auto* btns = new DialogButtons(this, {"OK", "Cancel"});
        btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        sizer->Add(btns, 0, wxEXPAND);

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
        rebuild_rows();
    }

    void rebuild_rows()
    {
        Freeze();
        fill_list(m_main_panel, m_working.main);
        fill_list(m_gizmo_panel, m_working.gizmos);
        if (GetSizer() != nullptr) {
            Layout();
            GetSizer()->SetSizeHints(this);
        }
        wxGetApp().UpdateDlgDarkUI(this);
        Thaw();
    }

    void fill_list(wxPanel* panel, ToolbarGroupLayout& group)
    {
        panel->DestroyChildren();
        wxSizer*         rows   = panel->GetSizer();
        const GLCanvas3D* canvas = wxGetApp().plater() != nullptr ? wxGetApp().plater()->get_view3D_canvas3D() : nullptr;
        for (const std::string& id : group.order) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);

            auto* check = new ::CheckBox(panel);
            check->SetValue(group.hidden.count(id) == 0);
            check->Bind(wxEVT_TOGGLEBUTTON, [this, id, check](wxCommandEvent& e) {
                ToolbarGroupLayout& g = group_of(id);
                if (check->GetValue())
                    g.hidden.erase(id);
                else
                    g.hidden.insert(id);
                e.Skip();
            });
            row->Add(check, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

            const bool selected = id == m_selected;
            auto*      name     = new Label(panel, selected ? Label::Head_14 : Label::Body_14, toolbar_item_name(id));
            name->Bind(wxEVT_LEFT_DOWN, [this, id](wxMouseEvent&) {
                // Rebuilding destroys this label, so do it after the click has been handled.
                CallAfter([this, id]() {
                    m_selected = id;
                    rebuild_rows();
                });
            });
            row->Add(name, 0, wxALIGN_CENTER_VERTICAL);

            if (id != TOOLBAR_SEPARATOR_ID && canvas != nullptr && !canvas->is_toolbar_item_available(id)) {
                auto* note = new Label(panel, Label::Body_12, _L("(not available now)"));
                note->SetForegroundColour(wxColour(144, 144, 144));
                row->Add(note, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
            }
            rows->Add(row, 0, wxBOTTOM, FromDIP(4));
        }
        panel->Layout();
    }

    ToolbarLayout m_loaded;
    ToolbarLayout m_working;
    std::string   m_selected;
    wxPanel*      m_main_panel{nullptr};
    wxPanel*      m_gizmo_panel{nullptr};
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
