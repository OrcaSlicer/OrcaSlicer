#include "CurveEditorDialog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <wx/checkbox.h>
#include <wx/colour.h>
#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/grid.h>
#include <wx/panel.h>
#include <wx/pen.h>
#include <wx/recguard.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinbutt.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/window.h>
#include "libslic3r/CurveModel.hpp"
#include "CurveEditorPanel.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "KeyChord.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r::GUI {
namespace {

std::string format_number(double value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}

bool read_number(wxString text, double& value)
{
    return CurveModel::read_number(into_u8(text), value);
}

// Native grid header bevels ignore the dialog's border colour on Windows.
template<class Header>
class FlatGridHeaderRenderer : public Header
{
public:
    void DrawBorder(const wxGrid& grid, wxDC& dc, wxRect& rect) const override
    {
        dc.SetPen(wxPen(grid.GetGridLineColour(), 1));
        dc.DrawLine(rect.GetRight(), rect.GetTop(), rect.GetRight(), rect.GetBottom());
        dc.DrawLine(rect.GetLeft(), rect.GetBottom(), rect.GetRight(), rect.GetBottom());
        rect.Deflate(1);
    }

    void DrawHighlighted(const wxGrid& grid, wxDC& dc, wxRect& rect, int, int) const override
    {
        DrawBorder(grid, dc, rect);
    }
};

class FlatGridAttributes : public wxGridCellAttrProvider
{
public:
    const wxGridColumnHeaderRenderer& GetColumnHeaderRenderer(int) override { return m_column; }
    const wxGridRowHeaderRenderer& GetRowHeaderRenderer(int) override { return m_row; }
    const wxGridCornerHeaderRenderer& GetCornerRenderer() override { return m_corner; }

private:
    FlatGridHeaderRenderer<wxGridColumnHeaderRenderer> m_column;
    FlatGridHeaderRenderer<wxGridRowHeaderRenderer> m_row;
    FlatGridHeaderRenderer<wxGridCornerHeaderRenderer> m_corner;
};

} // namespace

CurveEditorDialog::CurveEditorDialog(wxWindow* parent, const wxString& title, const wxString& help_text,
                                     const CurveEditorAppearance& appearance, std::unique_ptr<CurveModel> model)
    : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_appearance(appearance), m_model(std::move(model))
{
    SetBackgroundColour(*wxWHITE);

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);
    m_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_content->SetMinSize(FromDIP(wxSize(260, 80)));
    m_content->SetScrollRate(FromDIP(10), FromDIP(10));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* help = new wxStaticText(m_content, wxID_ANY, help_text);
    help->SetFont(Label::Body_14);
    help->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    help->Wrap(FromDIP(880));
    sizer->Add(help, 0, wxEXPAND | wxALL, FromDIP(10));

    auto* editors = new wxBoxSizer(wxHORIZONTAL);
    auto* plot_panel = new wxPanel(m_content);
    auto* table_panel = m_table_panel = new wxPanel(m_content);
    auto* plot_sizer = new wxBoxSizer(wxVERTICAL);
    auto* table_sizer = new wxBoxSizer(wxVERTICAL);
    editors->Add(plot_panel, 1, wxEXPAND | wxRIGHT, FromDIP(12));
    editors->Add(table_panel, 0, wxEXPAND);
    sizer->Add(editors, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_chart = new CurveEditorPanel(plot_panel, appearance);
    m_chart->SetBackgroundColour(*wxWHITE);
    m_chart->SetFont(Label::Body_12);
    m_chart->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
    m_chart->before_drag = [this] { finish_edit(); update_preview(); };
    m_chart->on_select = [this](int row) {
        m_grid->SetGridCursor(row, 0);
        m_grid->MakeCellVisible(row, 0);
    };
    m_chart->on_move = [this](int row, double x, double y) { move_point(row, x, y); };
    auto change_view = [this](const CurveEditorView& view) {
        const auto& current = m_chart->view();
        if (view.min_x == current.min_x && view.max_x == current.max_x &&
            view.min_y == current.min_y && view.max_y == current.max_y) return;
        m_chart->set_view(view);
        sync_chart_range();
    };
    m_chart->on_pan = [this, change_view](double steps, bool vertical) {
        const auto& view = m_chart->view();
        const double offset = steps * (vertical ? view.max_y - view.min_y : view.max_x - view.min_x) * 0.1;
        change_view(m_model->panned_view(view, vertical ? 0.0 : offset, vertical ? offset : 0.0));
    };
    m_chart->on_zoom = [this, change_view](double steps, bool vertical, double anchor) {
        // Bound the exponent so large wheel events cannot overflow the scale factor.
        const double factor = std::pow(1.1, -std::clamp(steps, -1000.0, 1000.0));
        change_view(m_model->zoomed_view(m_chart->view(), factor, anchor, vertical));
    };
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) { m_chart->finish_drag(); event.Skip(); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { m_chart->finish_drag(); event.Skip(); }, wxID_CANCEL);
    SetEscapeId(wxID_CANCEL);
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) {
            m_chart->finish_drag();
            if (IsModal()) EndModal(wxID_CANCEL);
            else Close();
            return;
        }
        event.Skip();
    });
    plot_sizer->Add(m_chart, 1, wxEXPAND);

    auto make_button = [this](wxWindow* button_parent, const wxString& label) {
        auto* button = new Button(button_parent, label);
        button->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        button->SetFont(Label::Body_12);
        button->SetCornerRadius(FromDIP(4));
        button->SetMinSize(wxSize(-1, FromDIP(26)));
        return button;
    };

    auto* range_panel = new wxPanel(plot_panel);
    auto* range_sizer = new wxBoxSizer(wxVERTICAL);
    auto* ranges = new wxFlexGridSizer(4, FromDIP(6), FromDIP(8));
    auto* apply_btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto add_title = [range_panel, ranges](const wxString& label) {
        auto* title = new wxStaticText(range_panel, wxID_ANY, label);
        title->SetFont(Label::Body_12);
        title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        ranges->Add(title);
    };

    add_title(_L("Visible range"));
    add_title(_L("Minimum"));
    add_title(_L("Maximum"));
    ranges->AddSpacer(0);
    const auto view_limits = m_model->view_limits();
    for (int axis = 0; axis < 2; ++axis) {
        auto range_label = new wxStaticText(range_panel, wxID_ANY, axis == 0 ? appearance.x_label : appearance.y_label);
        range_label->SetFont(Label::Body_12);
        range_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        ranges->Add(range_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
        for (int bound = 0; bound < 2; ++bound) {
            const int index = axis * 2 + bound;
            auto* field = new TextInput(range_panel, "", "", "", wxDefaultPosition, FromDIP(wxSize(78, -1)), wxTE_PROCESS_ENTER);
            auto* text = field->GetTextCtrl();
            text->SetFont(Label::Body_12);
            // Recompute the text size without the minimum retained from TextInput's default font.
            text->SetInitialSize(wxDefaultSize);
            const wxSize field_size(FromDIP(78), std::max(FromDIP(26), text->GetSize().y + FromDIP(8)));
            field->SetMinSize(field_size);
            field->SetSize(field_size);
            m_range_fields[index] = field;
            field->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { apply_chart_range(); });
            field->GetTextCtrl()->Bind(wxEVT_KEY_DOWN, [this, index](wxKeyEvent& event) {
                if (event.GetKeyCode() == WXK_UP || event.GetKeyCode() == WXK_DOWN)
                    step_chart_range(index, event.GetKeyCode() == WXK_UP ? 1 : -1);
                else
                    event.Skip();
            });
            const double minimum = axis == 0 ? view_limits.bounds.min_x : view_limits.bounds.min_y;
            const double maximum = axis == 0 ? view_limits.bounds.max_x : view_limits.bounds.max_y;
            const wxString tooltip =
                _L("Changes only the visible range of the graph, not the model values. Press Enter or Apply to update.") +
                "\n" + wxString::Format(_L("Allowed range: %s to %s."),
                    wxString::FromUTF8(format_number(minimum)), wxString::FromUTF8(format_number(maximum)));
            field->SetToolTip(tooltip);

            // Keep the styled input and its raw text: a native double spin control
            // can silently replace invalid text on focus loss before Apply validates it.
            // The native Windows default height is twice the scrollbar height and would stretch the entire row.
            const wxSize arrow_size(FromDIP(18), field_size.y);
            auto* arrows = new wxSpinButton(range_panel, wxID_ANY, wxDefaultPosition, arrow_size, wxSP_VERTICAL);
            arrows->SetMinSize(arrow_size);
            arrows->SetRange(-1, 1);
            arrows->SetValue(0);
            arrows->SetToolTip(tooltip);
            arrows->Bind(wxEVT_SPIN_UP, [this, index](wxSpinEvent& event) {
                event.Veto(); // Keep the arrows centred for repeated clicks in either direction.
                step_chart_range(index, 1);
            });
            arrows->Bind(wxEVT_SPIN_DOWN, [this, index](wxSpinEvent& event) {
                event.Veto();
                step_chart_range(index, -1);
            });
            auto* input = new wxBoxSizer(wxHORIZONTAL);
            input->Add(field, 1, wxEXPAND);
            input->Add(arrows, 0, wxEXPAND | wxLEFT, FromDIP(2));
            ranges->Add(input, 1, wxEXPAND);
        }
        if (axis == 0)
            ranges->Add(apply_btn_sizer, 0, wxEXPAND | wxLEFT, FromDIP(5));
        else
            ranges->AddSpacer(0);
    }

    auto* apply_range = make_button(range_panel, _L("Apply"));
    apply_range->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply_chart_range(); });
    apply_btn_sizer->Add(apply_range, 1, wxEXPAND);

    range_sizer->Add(ranges, 0, wxALIGN_LEFT);
    m_range_status = new wxStaticText(range_panel, wxID_ANY, wxEmptyString);
    m_range_status->SetFont(Label::Body_12);
    m_range_status->SetForegroundColour(wxColour("#E14747"));
    m_range_status->Hide();
    range_sizer->Add(m_range_status, 0, wxEXPAND | wxTOP, FromDIP(8));
    range_panel->SetSizer(range_sizer);
    range_panel->Hide();

    auto* view_actions = new wxBoxSizer(wxHORIZONTAL);
    auto* show_range = new wxCheckBox(plot_panel, wxID_ANY, _L("Show range"));
    show_range->SetFont(Label::Body_12);
    show_range->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
    show_range->SetValue(false);
    show_range->Bind(wxEVT_CHECKBOX, [this, range_panel](wxCommandEvent& event) {
        range_panel->Show(event.IsChecked());
        // Reuse the content's size handler to update orientation and scrollbars.
        m_content->SendSizeEvent();
    });
    view_actions->Add(show_range, 0, wxALIGN_CENTER_VERTICAL);
    m_show_table = new wxCheckBox(plot_panel, wxID_ANY, _L("Show table"));
    m_show_table->SetFont(Label::Body_12);
    m_show_table->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
    m_show_table->SetValue(true);
    m_show_table->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
        finish_edit();
        update_preview();
        m_table_panel->Show(event.IsChecked());
        m_content->SendSizeEvent();
    });
    view_actions->Add(m_show_table, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    view_actions->AddStretchSpacer();
    auto make_icon_button = [plot_panel](const wxString& name, const wxString& icon) {
        auto* button = new Button(plot_panel, wxEmptyString, icon, 0, 16);
        button->SetStyle(ButtonStyle::Regular, ButtonType::Icon);
        button->SetIconSpacing(0);
        button->SetName(name);
        return button;
    };
    const wxString ctrl = wxString::FromUTF8(KeyChord::modifier_name(wxMOD_CONTROL));
    const wxString shift = wxString::FromUTF8(KeyChord::modifier_name(wxMOD_SHIFT));
    const wxString controls_help = wxString::Format(
        _L("Drag a point to move it, or edit its values in the table.\n\n"
           "Mouse wheel: move horizontally.\n"
           "%s + mouse wheel: move vertically.\n"
           "%s + mouse wheel: zoom horizontally.\n"
           "%s + %s + mouse wheel: zoom vertically.\n\n"
           "Show range: enter exact bounds and press Enter or Apply.\n"
           "Show table: show or hide the point values.\n"
           "Fit curve: show the full curve.\n"
           "Reset to defaults: restore the model's default points."), shift, ctrl, ctrl, shift);
    auto* help_button = make_icon_button(_L("Graph controls"), "thermal_question");
    help_button->SetToolTip(controls_help);
    help_button->Bind(wxEVT_BUTTON, [this, controls_help](wxCommandEvent&) {
        MessageDialog dialog(this, controls_help, _L("Graph controls"), wxOK | wxICON_INFORMATION);
        dialog.ShowModal();
    });
    view_actions->Add(help_button, 0, wxLEFT, FromDIP(8));
    auto* fit_range = make_icon_button(_L("Fit curve"), "design_zoom");
    fit_range->SetToolTip(_L("Fit curve") + "\n" + _L("Show the full curve."));
    fit_range->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        finish_edit();
        update_preview();
        fit_chart();
    });
    view_actions->Add(fit_range, 0, wxLEFT, FromDIP(8));
    plot_sizer->Add(view_actions, 0, wxEXPAND | wxTOP, FromDIP(8));
    plot_sizer->Add(range_panel, 0, wxEXPAND | wxTOP, FromDIP(8));

    auto* grid_frame = new StaticBox(table_panel);
    grid_frame->SetCornerRadius(FromDIP(4));
    grid_frame->SetBorderWidth(FromDIP(1));
    grid_frame->SetBorderColorNormal(wxColour("#DFDFDF"));
    auto* grid_sizer = new wxBoxSizer(wxVERTICAL);
    m_grid = new wxGrid(grid_frame, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_grid->SetFont(Label::Body_12);
    m_grid->SetDefaultCellFont(Label::Body_12);
    m_grid->CreateGrid(0, 2);
    m_grid->GetTable()->SetAttrProvider(new FlatGridAttributes);
    m_grid->SetLabelFont(Label::Body_12);
    m_grid->SetDefaultRowSize(std::max(FromDIP(22), m_grid->GetCharHeight() + FromDIP(6)), true);
    m_grid->SetColLabelValue(0, appearance.x_label);
    m_grid->SetColLabelValue(1, appearance.y_label);
    m_grid->SetRowLabelSize(FromDIP(32));
    wxClientDC grid_dc(m_grid);
    grid_dc.SetFont(m_grid->GetLabelFont());
    const std::array<int, 2> column_widths = {
        grid_dc.GetTextExtent(appearance.x_label).x + FromDIP(12),
        grid_dc.GetTextExtent(appearance.y_label).x + FromDIP(12)
    };
    m_grid->SetColSize(0, column_widths[0]);
    m_grid->SetColSize(1, column_widths[1]);
    m_grid->SetColLabelSize(grid_dc.GetTextExtent("Ag").y + FromDIP(8));
    // Reserve room for row labels and the vertical scrollbar without wrapping column headers.
    m_grid->SetMinSize(wxSize(std::max(FromDIP(280), column_widths[0] + column_widths[1] +
        m_grid->GetRowLabelSize() + wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, m_grid)), FromDIP(180)));
    m_grid->DisableDragRowSize();
    m_grid->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& event) { update_preview(); event.Skip(); });
    m_grid->Bind(wxEVT_GRID_SELECT_CELL, [this](wxGridEvent& event) { m_chart->select_point(event.GetRow()); event.Skip(); });
    m_grid->GetGridWindow()->Bind(wxEVT_SIZE, [this, column_widths](wxSizeEvent& event) {
        event.Skip();
        wxRecursionGuard guard(m_grid_resize_depth);
        if (guard.IsInside()) return;
        const int available = m_grid->GetGridWindow()->GetClientSize().x;
        if (available > 0) {
            const int first = available * column_widths[0] / (column_widths[0] + column_widths[1]);
            if (m_grid->GetColSize(0) != first)
                m_grid->SetColSize(0, first);
            if (m_grid->GetColSize(1) != available - first)
                m_grid->SetColSize(1, available - first);
            if (available != m_grid->GetGridWindow()->GetClientSize().x)
                m_grid->GetGridWindow()->SendSizeEvent(wxSEND_EVENT_POST);
        }
    });
    grid_sizer->Add(m_grid, 1, wxEXPAND | wxALL, FromDIP(2));
    grid_frame->SetSizer(grid_sizer);
    table_sizer->Add(grid_frame, 1, wxEXPAND);

    auto* actions = new wxGridSizer(3, 0, FromDIP(6));
    auto add_button = [plot_panel, actions, make_button](const wxString& label, auto handler) {
        auto* button = make_button(plot_panel, label);
        button->Bind(wxEVT_BUTTON, handler);
        actions->Add(button, 0, wxEXPAND);
    };
    add_button(_L("Add point"), [this](wxCommandEvent&) {
        finish_edit();
        const int count = m_grid->GetNumberRows();
        if (count == 0) {
            load_points(m_model->seed_rows());
        } else {
            std::vector<double> x, y;
            const bool valid = read_points(x, y) && count > 1;
            const int row = valid ? std::min(std::max(1, m_grid->GetGridCursorRow() + 1), count - 1) : count;
            m_grid->InsertRows(row);
            if (valid) {
                m_grid->SetCellValue(row, 0, wxString::FromUTF8(format_number(x[row - 1] + (x[row] - x[row - 1]) / 2)));
                m_grid->SetCellValue(row, 1, wxString::FromUTF8(format_number(y[row - 1] + (y[row] - y[row - 1]) / 2)));
            }
            m_grid->SetGridCursor(row, 0);
            m_grid->MakeCellVisible(row, 0);
        }
        update_preview();
    });
    add_button(_L("Remove point"), [this](wxCommandEvent&) {
        finish_edit();
        const int row = m_grid->GetGridCursorRow();
        if (row >= 0 && row < m_grid->GetNumberRows())
            m_grid->DeleteRows(row);
        update_preview();
    });
    auto* reset = make_button(plot_panel, _L("Reset to defaults"));
    reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        finish_edit();
        load_points(m_model->default_rows());
        update_preview();
    });
    actions->Add(reset, 0, wxEXPAND);
    plot_sizer->Insert(1, actions, 0, wxEXPAND | wxTOP, FromDIP(6));

    m_status = new wxStaticText(table_panel, wxID_ANY, wxEmptyString);
    m_status->SetFont(Label::Body_12);
    m_status->SetForegroundColour(wxColour("#E14747"));
    m_status->Hide();
    table_sizer->Add(m_status, 0, wxEXPAND | wxTOP, FromDIP(8));

    plot_panel->SetSizer(plot_sizer);
    table_panel->SetSizer(table_sizer);
    m_content->SetSizer(sizer);
    m_content->Bind(wxEVT_SIZE, [this, editors, plot_panel, help](wxSizeEvent& event) {
        event.Skip();
        if (m_layout_depth != 0) return;
        const int width = m_content->GetClientSize().x;
        // Wrap retains the original text. Resetting the label would leave it
        // unwrapped when wxWidgets skips wrapping again at the same width.
        // Native static text adds a pixel to its measured width on Windows.
        help->Wrap(std::max(FromDIP(240), width - FromDIP(20) - 1));
        editors->SetOrientation(wxHORIZONTAL);
        auto* plot_item = editors->GetItem(plot_panel);
        plot_item->SetFlag(m_table_panel->IsShown() ? wxEXPAND | wxRIGHT : wxEXPAND);
        // Keep the table's headers readable; stack the panels when both no longer fit.
        if (m_table_panel->IsShown() && width < editors->GetMinSize().x + FromDIP(20)) {
            editors->SetOrientation(wxVERTICAL);
            plot_item->SetFlag(wxEXPAND | wxBOTTOM);
        }
        layout_content();
    });
    main_sizer->Add(m_content, 1, wxEXPAND);
    // Keep the modal actions outside the scrollable content on small/high-DPI displays.
    main_sizer->Add(new DialogButtons(this, {"OK", "Cancel"}), 0, wxEXPAND);
    SetSizerAndFit(main_sizer);
    wxGetApp().UpdateDlgDarkUI(this);
    m_grid->SetCellHighlightColour(StateColor::darkModeColorFor(wxColour("#009688")));
    m_grid->SetGridLineColour(     StateColor::darkModeColorFor(wxColour("#DFDFDF")));
    m_grid->SetSelectionBackground(StateColor::darkModeColorFor(wxColour("#BFE1DE")));
    m_grid->SetSelectionForeground(StateColor::darkModeColorFor(wxColour("#262E30")));
    m_grid->SetDefaultCellBackgroundColour(GetBackgroundColour());
    m_grid->SetDefaultCellTextColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    m_grid->SetLabelBackgroundColour(GetBackgroundColour());
    m_grid->SetLabelTextColour(StateColor::darkModeColorFor(wxColour("#262E30")));

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        finish_edit();
        std::vector<double> x, y;
        if (!read_points(x, y)) return;
        m_model->accept_rows(read_rows());
        EndModal(wxID_OK);
    }, wxID_OK);

    load_points(m_model->rows());
    update_preview();
    fit_chart();
    fit_in_display(*this, ClientToWindowSize(FromDIP(wxSize(920, 480))));
    layout_content();
    CentreOnParent();
}

CurveEditorDialog::~CurveEditorDialog()
{
    m_chart->finish_drag();
    m_chart->before_drag = {};
    m_chart->on_select = {};
    m_chart->on_move = {};
    m_chart->on_pan = {};
    m_chart->on_zoom = {};
}

void CurveEditorDialog::finish_edit()
{
    if (m_grid->IsCellEditControlEnabled()) {
        m_grid->SaveEditControlValue();
        m_grid->HideCellEditControl();
        m_grid->DisableCellEditControl();
    }
}

void CurveEditorDialog::load_points(const Rows& rows)
{
    m_chart->finish_drag();
    if (m_grid->GetNumberRows() > 0)
        m_grid->DeleteRows(0, m_grid->GetNumberRows());
    if (!rows.empty()) m_grid->AppendRows(rows.size());
    for (size_t row = 0; row < rows.size(); ++row) {
        m_grid->SetCellValue(row, 0, wxString::FromUTF8(rows[row].first));
        m_grid->SetCellValue(row, 1, wxString::FromUTF8(rows[row].second));
    }
}

CurveEditorDialog::Rows CurveEditorDialog::read_rows() const
{
    Rows rows;
    for (int row = 0; row < m_grid->GetNumberRows(); ++row)
        rows.emplace_back(into_u8(m_grid->GetCellValue(row, 0)), into_u8(m_grid->GetCellValue(row, 1)));
    return rows;
}

bool CurveEditorDialog::read_points(std::vector<double>& x, std::vector<double>& y)
{
    int error_row = -1;
    const char* message = m_model->read_points(read_rows(), x, y, error_row);
    wxString error = message == nullptr ? wxString() : _L(message);
    if (!error.empty() && error_row >= 0)
        error = wxString::Format(_L("Row %d: "), error_row + 1) + error;
    const char* empty_message = m_model->empty_message();
    m_status->SetLabel(error.empty() && x.empty() && empty_message != nullptr ? _L(empty_message) : error);
    m_status->Wrap(FromDIP(260));
    m_status->Show(!m_status->GetLabel().empty());
    if (!error.empty() && !m_table_panel->IsShown()) {
        // Keep invalid point values and their explanation accessible for correction.
        m_table_panel->Show();
        m_show_table->SetValue(true);
        m_content->SendSizeEvent();
    }
    layout_content();
    return error.empty();
}

void CurveEditorDialog::refresh_chart_data(const std::vector<double>& x, const std::vector<double>& y)
{
    std::vector<wxString> tooltips;
    for (size_t row = 0; row < x.size(); ++row)
        tooltips.push_back(m_appearance.x_label + ": " + m_grid->GetCellValue(row, 0).Trim().Trim(false) + "\n" +
                           m_appearance.y_label + ": " + m_grid->GetCellValue(row, 1).Trim().Trim(false));
    m_chart->set_data(x, y, tooltips, x.empty() ? CurveEditorPanel::Interpolator{} : m_model->make_interpolator(x, y));
    m_chart->select_point(m_grid->GetGridCursorRow());
}

void CurveEditorDialog::update_preview()
{
    std::vector<double> x, y;
    if (!read_points(x, y)) {
        x.clear();
        y.clear();
    }
    refresh_chart_data(x, y);
}

void CurveEditorDialog::move_point(int row, double proposed_x, double proposed_y)
{
    std::vector<double> x, y;
    if (!read_points(x, y) || row < 0 || row >= int(x.size())) return;
    const CurveEditorView bounds = m_model->drag_bounds(row, x, y, m_chart->view());
    if (bounds.min_x > bounds.max_x || bounds.min_y > bounds.max_y) return;
    auto set_value = [this, row](int column, double value, double lower, double upper, double& current) {
        value = std::clamp(value, lower, upper);
        if (value == current) return;
        wxString text = wxString::FromCDouble(value, 4);
        double rounded;
        if (!read_number(text, rounded) || rounded < lower || rounded > upper) {
            text = wxString::FromUTF8(format_number(value));
            rounded = value;
        }
        if (rounded != current) {
            current = rounded;
            m_grid->SetCellValue(row, column, text);
        }
    };
    set_value(0, proposed_x, bounds.min_x, bounds.max_x, x[row]);
    set_value(1, proposed_y, bounds.min_y, bounds.max_y, y[row]);
    refresh_chart_data(x, y);
}

void CurveEditorDialog::sync_chart_range()
{
    const auto& view = m_chart->view();
    const double values[] = {view.min_x, view.max_x, view.min_y, view.max_y};
    for (int i = 0; i < 4; ++i) {
        auto* text = m_range_fields[i]->GetTextCtrl();
        const wxString value = wxString::FromUTF8(format_number(values[i]));
        if (text->GetValue() != value) text->ChangeValue(value);
    }
    // Only a visibility change needs layout; wheel navigation must not resize sibling controls.
    if (m_range_status->Hide()) layout_content();
}

void CurveEditorDialog::fit_chart()
{
    std::vector<double> x, y;
    if (!read_points(x, y)) {
        x.clear();
        y.clear();
    }
    m_chart->set_view(m_model->fitted_view(x, y));
    sync_chart_range();
}

void CurveEditorDialog::apply_chart_range()
{
    std::array<std::string, 4> bounds;
    for (int i = 0; i < 4; ++i)
        bounds[i] = into_u8(m_range_fields[i]->GetTextCtrl()->GetValue());
    CurveEditorView view;
    if (const char* error = m_model->read_view(bounds, view)) {
        m_range_status->SetLabel(_L(error));
        m_range_status->Wrap(FromDIP(520));
        m_range_status->Show();
        layout_content();
        return;
    }
    m_chart->set_view(view);
    sync_chart_range();
}

void CurveEditorDialog::step_chart_range(int field, int direction)
{
    double value;
    auto* text = m_range_fields[field]->GetTextCtrl();
    if (!read_number(text->GetValue(), value)) {
        apply_chart_range();
        return;
    }
    const auto limits = m_model->view_limits();
    const bool x_axis = field < 2;
    const double step = x_axis ? limits.x_step : limits.y_step;
    const double minimum = x_axis ? limits.bounds.min_x : limits.bounds.min_y;
    const double maximum = x_axis ? limits.bounds.max_x : limits.bounds.max_y;
    value = std::clamp(value + direction * step, minimum, maximum);
    text->ChangeValue(wxString::FromCDouble(value, 4));
    apply_chart_range();
}

void CurveEditorDialog::layout_content()
{
    wxRecursionGuard guard(m_layout_depth);
    if (guard.IsInside()) return;
    // Sizers force size events even when geometry is unchanged. Relayout only
    // the content: laying out the dialog here re-enters its child's size handler.
    const int width = m_content->GetClientSize().x;
    m_content->Layout();
    m_content->FitInside();
    // Scrollbars can change the available width. Rewrap after the current layout completes.
    if (width != m_content->GetClientSize().x) m_content->SendSizeEvent(wxSEND_EVENT_POST);
}

} // namespace Slic3r::GUI
