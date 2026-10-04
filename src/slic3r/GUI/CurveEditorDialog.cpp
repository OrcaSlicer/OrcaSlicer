#include "CurveEditorDialog.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>
#include <wx/grid.h>
#include <wx/sizer.h>
#include <wx/spinbutt.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"

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
} // namespace

CurveEditorDialog::CurveEditorDialog(wxWindow* parent, const wxString& title, const wxString& help_text,
                                     const CurveEditorAppearance& appearance, std::unique_ptr<CurveModel> model)
    : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_appearance(appearance), m_model(std::move(model))
{
    SetBackgroundColour(*wxWHITE);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* help = new wxStaticText(this, wxID_ANY, help_text);
    help->SetFont(Label::Body_14);
    help->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
    help->Wrap(FromDIP(640));
    sizer->Add(help, 0, wxEXPAND | wxALL, FromDIP(16));

    m_chart = new CurveEditorPanel(this, appearance);
    m_chart->SetBackgroundColour(*wxWHITE);
    m_chart->SetFont(Label::Body_12);
    m_chart->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
    m_chart->before_drag = [this] { finish_edit(); update_preview(); };
    m_chart->on_select = [this](int row) {
        m_grid->SetGridCursor(row, 0);
        m_grid->MakeCellVisible(row, 0);
    };
    m_chart->on_move = [this](int row, double x, double y) { move_point(row, x, y); };
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) { m_chart->finish_drag(); event.Skip(); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { m_chart->finish_drag(); event.Skip(); }, wxID_CANCEL);
    sizer->Add(m_chart, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    auto* ranges = new wxFlexGridSizer(4, FromDIP(6), FromDIP(8));
    ranges->AddGrowableCol(1, wxHORIZONTAL); // Let inputs shrink while labels and translated buttons retain their width.
    ranges->AddGrowableCol(2, wxHORIZONTAL);

    auto* apply_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* fit_btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto add_title = [this, ranges](const wxString& label) {
        auto* title = new wxStaticText(this, wxID_ANY, label);
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
        auto range_label = new wxStaticText(this, wxID_ANY, axis == 0 ? appearance.x_label : appearance.y_label);
        range_label->SetFont(Label::Body_14);
        range_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        ranges->Add(range_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
        for (int bound = 0; bound < 2; ++bound) {
            const int index = axis * 2 + bound;
            auto* field = new TextInput(this, "", "", "", wxDefaultPosition, FromDIP(wxSize(-1, -1)), wxTE_PROCESS_ENTER);
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
            auto* arrows = new wxSpinButton(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(20, -1)), wxSP_VERTICAL);
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
        ranges->Add(axis == 0 ? apply_btn_sizer : fit_btn_sizer, 0, wxEXPAND | wxLEFT, FromDIP(5));
    }

    auto* apply_range = new Button(this, _L("Apply"));
    apply_range->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    apply_range->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply_chart_range(); });
    apply_btn_sizer->Add(apply_range, 1, wxEXPAND);

    auto* fit_range = new Button(this, _L("Fit curve"));
    fit_range->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    fit_range->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        finish_edit();
        update_preview();
        fit_chart();
    });
    fit_btn_sizer->Add(fit_range, 1, wxEXPAND);

    sizer->Add(ranges, 0, wxEXPAND | wxALL, FromDIP(16));
    m_range_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_range_status->SetFont(Label::Body_14);
    m_range_status->SetForegroundColour(wxColour("#E14747"));
    m_range_status->Hide();
    sizer->Add(m_range_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    m_grid = new wxGrid(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSIMPLE_BORDER);
    m_grid->SetFont(Label::Body_14);
    m_grid->SetDefaultCellFont(Label::Body_14);
    m_grid->CreateGrid(0, 2);
    m_grid->SetColLabelValue(0, appearance.x_label);
    m_grid->SetColLabelValue(1, appearance.y_label);
    m_grid->SetRowLabelSize(FromDIP(40));
    m_grid->SetColSize(0, FromDIP(280));
    m_grid->SetColSize(1, FromDIP(280));
    m_grid->SetMinSize(FromDIP(wxSize(640, 230)));
    m_grid->DisableDragRowSize();
    m_grid->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& event) { update_preview(); event.Skip(); });
    m_grid->Bind(wxEVT_GRID_SELECT_CELL, [this](wxGridEvent& event) { m_chart->select_point(event.GetRow()); event.Skip(); });
    m_grid->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        const int available = m_grid->GetClientSize().x - m_grid->GetRowLabelSize();
        if (available > 0) {
            m_grid->SetColSize(0, available / 2);
            m_grid->SetColSize(1, available - available / 2);
        }
        event.Skip();
    });
    sizer->Add(m_grid, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    sizer->AddSpacer(FromDIP(5));

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto add_button = [this, actions](const wxString& label, auto handler) {
        auto* button = new Button(this, label);
        button->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
        button->Bind(wxEVT_BUTTON, handler);
        actions->Add(button);
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
    actions->AddSpacer(FromDIP(10));
    add_button(_L("Remove point"), [this](wxCommandEvent&) {
        finish_edit();
        const int row = m_grid->GetGridCursorRow();
        if (row >= 0 && row < m_grid->GetNumberRows())
            m_grid->DeleteRows(row);
        update_preview();
    });
    actions->AddStretchSpacer();
    add_button(_L("Reset to defaults"), [this](wxCommandEvent&) {
        finish_edit();
        load_points(m_model->default_rows());
        update_preview();
    });
    sizer->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_status->SetFont(Label::Body_14);
    m_status->SetForegroundColour(wxColour("#E14747"));
    sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    sizer->Add(new DialogButtons(this, {"OK", "Cancel"}), 0, wxEXPAND);
    SetSizerAndFit(sizer);
    wxGetApp().UpdateDlgDarkUI(this);
    m_grid->SetCellHighlightColour(StateColor::darkModeColorFor(wxColour("#009688")));
    m_grid->SetGridLineColour(     StateColor::darkModeColorFor(wxColour("#DBDBDB")));
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
    CentreOnParent();
}

CurveEditorDialog::~CurveEditorDialog()
{
    m_chart->finish_drag();
    m_chart->before_drag = {};
    m_chart->on_select = {};
    m_chart->on_move = {};
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
    m_status->Wrap(FromDIP(640));
    m_status->Show(!m_status->GetLabel().empty());
    Layout();
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
    for (int i = 0; i < 4; ++i)
        m_range_fields[i]->GetTextCtrl()->ChangeValue(wxString::FromUTF8(format_number(values[i])));
    m_range_status->Hide();
    Layout();
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
        m_range_status->Wrap(FromDIP(640));
        m_range_status->Show();
        Layout();
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

} // namespace Slic3r::GUI
