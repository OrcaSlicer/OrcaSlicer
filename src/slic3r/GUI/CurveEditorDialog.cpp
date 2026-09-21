#include "CurveEditorDialog.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <wx/grid.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include "GUI_App.hpp"
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
    text.Trim().Trim(false);
    text.Replace(",", ".");
    return text.ToCDouble(&value) && std::isfinite(value);
}
} // namespace

CurveEditorDialog::CurveEditorDialog(wxWindow* parent, const wxString& title, const wxString& help_text,
                                     const CurveEditorAppearance& appearance)
    : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_appearance(appearance)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* help = new wxStaticText(this, wxID_ANY, help_text);
    help->Wrap(FromDIP(640));
    sizer->Add(help, 0, wxEXPAND | wxALL, FromDIP(16));

    m_chart = new CurveEditorPanel(this, appearance);
    m_chart->before_drag = [this] { finish_edit(); update_preview(); };
    m_chart->on_select = [this](int row) {
        m_grid->SetGridCursor(row, 0);
        m_grid->MakeCellVisible(row, 0);
    };
    m_chart->on_move = [this](int row, double x, double y) { move_point(row, x, y); };
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) { m_chart->finish_drag(); event.Skip(); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { m_chart->finish_drag(); event.Skip(); }, wxID_CANCEL);
    sizer->Add(m_chart, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    auto* ranges = new wxFlexGridSizer(3, FromDIP(6), FromDIP(8));
    ranges->Add(new wxStaticText(this, wxID_ANY, _L("Visible range")), 0, wxALIGN_CENTER_VERTICAL);
    ranges->Add(new wxStaticText(this, wxID_ANY, _L("Minimum")));
    ranges->Add(new wxStaticText(this, wxID_ANY, _L("Maximum")));
    for (int axis = 0; axis < 2; ++axis) {
        ranges->Add(new wxStaticText(this, wxID_ANY, axis == 0 ? appearance.x_label : appearance.y_label),
                    0, wxALIGN_CENTER_VERTICAL);
        for (int bound = 0; bound < 2; ++bound) {
            auto* field = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(100, -1)), wxTE_PROCESS_ENTER);
            m_range_fields[axis * 2 + bound] = field;
            field->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { apply_chart_range(); });
            field->SetToolTip(_L("Changes only the visible range of the graph, not the model values. Press Enter or Apply to update."));
            ranges->Add(field, 0, wxEXPAND);
        }
    }
    auto* range_buttons = new wxBoxSizer(wxVERTICAL);
    auto* apply_range = new Button(this, _L("Apply"));
    apply_range->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    apply_range->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply_chart_range(); });
    range_buttons->Add(apply_range, 0, wxBOTTOM, FromDIP(6));
    auto* fit_range = new Button(this, _L("Show entire curve"));
    fit_range->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    fit_range->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        finish_edit();
        update_preview();
        fit_chart();
    });
    range_buttons->Add(fit_range);
    auto* range_sizer = new wxBoxSizer(wxHORIZONTAL);
    range_sizer->Add(ranges, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
    range_sizer->Add(range_buttons, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(range_sizer, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    m_range_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_range_status->Hide();
    sizer->Add(m_range_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_grid = new wxGrid(this, wxID_ANY);
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
    sizer->Add(m_grid, 1, wxEXPAND | wxALL, FromDIP(16));

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto add_button = [this, actions](const wxString& label, auto handler) {
        auto* button = new Button(this, label);
        button->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
        button->Bind(wxEVT_BUTTON, handler);
        actions->Add(button, 0, wxRIGHT, FromDIP(8));
    };
    add_button(_L("Add point"), [this](wxCommandEvent&) {
        finish_edit();
        const int count = m_grid->GetNumberRows();
        if (count == 0) {
            load_points(seed_rows());
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
    add_button(_L("Reset to defaults"), [this](wxCommandEvent&) {
        finish_edit();
        load_points(default_rows());
        update_preview();
    });
    sizer->Add(actions, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    sizer->Add(new DialogButtons(this, {"OK", "Cancel"}), 0, wxEXPAND);
    SetSizerAndFit(sizer);
    wxGetApp().UpdateDlgDarkUI(this);
    m_grid->SetDefaultCellBackgroundColour(GetBackgroundColour());
    m_grid->SetDefaultCellTextColour(GetForegroundColour());
    m_grid->SetLabelBackgroundColour(GetBackgroundColour());
    m_grid->SetLabelTextColour(GetForegroundColour());

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        finish_edit();
        std::vector<double> x, y;
        if (!read_points(x, y)) return;
        Rows rows;
        for (int row = 0; row < m_grid->GetNumberRows(); ++row)
            rows.emplace_back(m_grid->GetCellValue(row, 0), m_grid->GetCellValue(row, 1));
        accept_rows(rows);
        EndModal(wxID_OK);
    }, wxID_OK);
}

CurveEditorDialog::~CurveEditorDialog()
{
    m_chart->finish_drag();
    m_chart->before_drag = {};
    m_chart->on_select = {};
    m_chart->on_move = {};
}

void CurveEditorDialog::initialize(const Rows& rows)
{
    load_points(rows);
    update_preview();
    fit_chart();
    CentreOnParent();
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
        m_grid->SetCellValue(row, 0, rows[row].first);
        m_grid->SetCellValue(row, 1, rows[row].second);
    }
}

bool CurveEditorDialog::read_points(std::vector<double>& x, std::vector<double>& y)
{
    x.clear();
    y.clear();
    wxString error;
    int error_row = -1;
    for (int row = 0; row < m_grid->GetNumberRows(); ++row) {
        double vx, vy;
        if (!read_number(m_grid->GetCellValue(row, 0), vx) || !read_number(m_grid->GetCellValue(row, 1), vy)) {
            error = _L("Enter a finite number in each cell.");
            error_row = row;
            break;
        }
        x.push_back(vx);
        y.push_back(vy);
    }
    if (error.empty()) error = validate_points(x, y, error_row);
    if (!error.empty() && error_row >= 0)
        error = wxString::Format(_L("Row %d: "), error_row + 1) + error;
    m_status->SetLabel(error.empty() && x.empty() ? empty_message() : error);
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
    m_chart->set_data(x, y, tooltips, x.empty() ? CurveEditorPanel::Interpolator{} : make_interpolator(x, y));
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
    const CurveEditorView bounds = drag_bounds(row, x, y, m_chart->view());
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
        m_range_fields[i]->ChangeValue(wxString::FromUTF8(format_number(values[i])));
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
    m_chart->set_view(fitted_view(x, y));
    sync_chart_range();
}

void CurveEditorDialog::apply_chart_range()
{
    double values[4];
    bool valid = true;
    for (int i = 0; i < 4; ++i)
        valid = read_number(m_range_fields[i]->GetValue(), values[i]) && valid;
    if (valid)
        valid = values[0] < values[1] && values[2] < values[3] &&
                std::isfinite(values[1] - values[0]) && std::isfinite(values[3] - values[2]);
    wxString error;
    CurveEditorView view;
    if (valid) {
        view = {values[0], values[1], values[2], values[3]};
        error = validate_view(view);
    } else
        error = _L("Enter finite bounds with minimum less than maximum.");
    if (!error.empty()) {
        m_range_status->SetLabel(error);
        m_range_status->Wrap(FromDIP(640));
        m_range_status->Show();
        Layout();
        return;
    }
    m_chart->set_view(view);
    sync_chart_range();
}

} // namespace Slic3r::GUI
