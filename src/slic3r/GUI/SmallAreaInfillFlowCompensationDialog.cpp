#include "SmallAreaInfillFlowCompensationDialog.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <wx/dcbuffer.h>
#include <wx/grid.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/PchipInterpolatorHelper.hpp"

namespace Slic3r::GUI {
namespace {

std::pair<wxString, wxString> split_point(const std::string& parameter)
{
    const wxString text = wxString::FromUTF8(parameter);
    const int comma = text.Find(',');
    // Keep malformed input visible so the user can repair it without losing rows.
    wxString length = comma == wxNOT_FOUND ? text : text.Left(comma);
    wxString factor = comma == wxNOT_FOUND ? wxString() : text.Mid(comma + 1);
    return {length.Trim().Trim(false), factor.Trim().Trim(false)};
}

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

SmallAreaInfillFlowCompensationDialog::SmallAreaInfillFlowCompensationDialog(
    wxWindow* parent, const std::vector<std::string>& parameters)
    : wxDialog(parent, wxID_ANY, _L("Flow Compensation Model"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), m_initial_parameters(parameters)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* help = new wxStaticText(this, wxID_ANY,
        _L("Drag points on the graph or edit their extrusion length and flow correction factor in the table. "
           "Lengths and factors must strictly increase. The first length must be 0 and the last factor must be 1."));
    help->Wrap(FromDIP(640));
    sizer->Add(help, 0, wxEXPAND | wxALL, FromDIP(16));

    m_chart = new wxPanel(this);
    m_chart->SetMinSize(FromDIP(wxSize(640, 230)));
    m_chart->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_chart->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_chart(); });
    m_chart->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { m_chart->Refresh(); event.Skip(); });
    m_chart->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        finish_edit();
        update_preview();
        m_dragged_point = hit_test(event.GetPosition());
        if (m_dragged_point < 0)
            return;
        m_drag_start = event.GetPosition();
        m_drag_previous = m_drag_start;
        m_drag_length = m_lengths[m_dragged_point];
        m_drag_factor = m_factors[m_dragged_point];
        m_grid->SetGridCursor(m_dragged_point, 0);
        m_grid->MakeCellVisible(m_dragged_point, 0);
        m_chart->SetFocus();
        m_chart->CaptureMouse();
        m_chart->Refresh();
    });
    m_chart->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        if (m_dragged_point >= 0) {
            if (event.LeftIsDown())
                drag_point(event.GetPosition());
            else
                finish_drag();
        } else
            m_chart->SetCursor(wxCursor(hit_test(event.GetPosition()) >= 0 ? wxCURSOR_HAND : wxCURSOR_ARROW));
    });
    m_chart->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { finish_drag(); });
    m_chart->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { finish_drag(); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) { finish_drag(); event.Skip(); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { finish_drag(); event.Skip(); }, wxID_CANCEL);
    sizer->Add(m_chart, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    m_grid = new wxGrid(this, wxID_ANY);
    m_grid->CreateGrid(0, 2);
    m_grid->SetColLabelValue(0, _L("Extrusion length") + " (mm)");
    m_grid->SetColLabelValue(1, _L("Flow correction factor"));
    m_grid->SetRowLabelSize(FromDIP(40));
    m_grid->SetColSize(0, FromDIP(280));
    m_grid->SetColSize(1, FromDIP(280));
    m_grid->SetMinSize(FromDIP(wxSize(640, 230)));
    m_grid->DisableDragRowSize();
    m_grid->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& event) { update_preview(); event.Skip(); });
    m_grid->Bind(wxEVT_GRID_SELECT_CELL, [this](wxGridEvent& event) { m_chart->Refresh(); event.Skip(); });
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
            load_points({"0,0", "10,1"});
        } else {
            std::vector<double> lengths, factors;
            const bool valid = read_points(lengths, factors);
            const int row = valid ? std::min(std::max(1, m_grid->GetGridCursorRow() + 1), count - 1) : count;
            m_grid->InsertRows(row);
            if (valid) {
                m_grid->SetCellValue(row, 0, wxString::FromUTF8(format_number(lengths[row - 1] + (lengths[row] - lengths[row - 1]) / 2)));
                m_grid->SetCellValue(row, 1, wxString::FromUTF8(format_number(factors[row - 1] + (factors[row] - factors[row - 1]) / 2)));
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
        const auto* defaults = static_cast<const ConfigOptionStrings*>(
            print_config_def.get("small_area_infill_flow_compensation_model")->default_value.get());
        load_points(defaults->values);
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

    load_points(parameters);
    update_preview();
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        finish_edit();
        update_preview();
        std::vector<double> lengths, factors;
        if (!read_points(lengths, factors))
            return;
        std::vector<std::string> output;
        for (int row = 0; row < m_grid->GetNumberRows(); ++row) {
            const auto original = row < m_initial_parameters.size() ? split_point(m_initial_parameters[row]) : std::pair<wxString, wxString>();
            if (row < m_initial_parameters.size() && m_grid->GetCellValue(row, 0) == original.first && m_grid->GetCellValue(row, 1) == original.second)
                output.push_back(m_initial_parameters[row]);
            else {
                wxString length = m_grid->GetCellValue(row, 0);
                wxString factor = m_grid->GetCellValue(row, 1);
                length.Trim().Trim(false).Replace(",", ".");
                factor.Trim().Trim(false).Replace(",", ".");
                output.push_back((row == 0 ? "" : "\n") + length.ToStdString() + "," + factor.ToStdString());
            }
        }
        // The option's existing serialized GUI path splits on semicolons into ConfigOptionStrings.
        m_output_data.clear();
        for (const auto& point : output)
            m_output_data += point + ";";
        m_modified = output != m_initial_parameters;
        EndModal(wxID_OK);
    }, wxID_OK);
    CentreOnParent();
}

void SmallAreaInfillFlowCompensationDialog::finish_edit()
{
    if (m_grid->IsCellEditControlEnabled()) {
        m_grid->SaveEditControlValue();
        m_grid->HideCellEditControl();
        m_grid->DisableCellEditControl();
    }
}

void SmallAreaInfillFlowCompensationDialog::load_points(const std::vector<std::string>& parameters)
{
    if (m_grid->GetNumberRows() > 0)
        m_grid->DeleteRows(0, m_grid->GetNumberRows());
    if (!parameters.empty())
        m_grid->AppendRows(parameters.size());
    for (size_t row = 0; row < parameters.size(); ++row) {
        const auto point = split_point(parameters[row]);
        m_grid->SetCellValue(row, 0, point.first);
        m_grid->SetCellValue(row, 1, point.second);
    }
}

bool SmallAreaInfillFlowCompensationDialog::read_points(std::vector<double>& lengths, std::vector<double>& factors)
{
    wxString error;
    const int count = m_grid->GetNumberRows();
    if (count == 1)
        error = _L("The model must contain at least two points.");
    for (int row = 0; row < count && error.empty(); ++row) {
        double length, factor;
        if (!read_number(m_grid->GetCellValue(row, 0), length) || !read_number(m_grid->GetCellValue(row, 1), factor))
            error = _L("Enter a finite number in each cell.");
        else if (row == 0 && length != 0.0)
            error = _L("The first extrusion length must be 0.");
        else if (row > 0 && length <= lengths.back())
            error = _L("Extrusion lengths must strictly increase.");
        else if (row > 0 && factor <= factors.back())
            error = _L("Flow correction factors must strictly increase.");
        else if (row == count - 1 && factor != 1.0)
            error = _L("The last flow correction factor must be 1.");
        if (!error.empty())
            error = wxString::Format(_L("Row %d: "), row + 1) + error;
        else {
            lengths.push_back(length);
            factors.push_back(factor);
        }
    }
    m_status->SetLabel(error.empty() ? (count == 0 ? _L("The model is empty. No flow compensation will be applied.") : wxString()) : error);
    m_status->Wrap(FromDIP(640));
    m_status->Show(!m_status->GetLabel().empty());
    Layout();
    return error.empty();
}

void SmallAreaInfillFlowCompensationDialog::update_preview()
{
    m_lengths.clear();
    m_factors.clear();
    if (!read_points(m_lengths, m_factors)) {
        m_lengths.clear();
        m_factors.clear();
    }
    if (!m_lengths.empty()) {
        // Leave room to move the last point to the right. Freeze the axes during a drag.
        m_chart_max_length = m_lengths.back() <= std::numeric_limits<double>::max() / 1.1 ? m_lengths.back() * 1.1 : m_lengths.back();
        m_chart_min_factor = std::min(0.0, m_factors.front());
    }
    m_chart->Refresh();
}

wxRect SmallAreaInfillFlowCompensationDialog::chart_rect() const
{
    const wxSize size = m_chart->GetClientSize();
    return wxRect(FromDIP(55), FromDIP(28), std::max(1, size.x - FromDIP(80)), std::max(1, size.y - FromDIP(76)));
}

wxPoint SmallAreaInfillFlowCompensationDialog::chart_point(double length, double factor) const
{
    const wxRect plot = chart_rect();
    return wxPoint(plot.x + int(length / m_chart_max_length * plot.width),
                   plot.y + plot.height - int((factor - m_chart_min_factor) / (1.0 - m_chart_min_factor) * plot.height));
}

int SmallAreaInfillFlowCompensationDialog::hit_test(const wxPoint& position) const
{
    int closest = -1;
    double distance = double(FromDIP(9)) * FromDIP(9);
    for (size_t i = 0; i < m_lengths.size(); ++i) {
        const wxPoint delta = position - chart_point(m_lengths[i], m_factors[i]);
        const double squared = double(delta.x) * delta.x + double(delta.y) * delta.y;
        if (squared < distance) {
            closest = int(i);
            distance = squared;
        }
    }
    return closest;
}

void SmallAreaInfillFlowCompensationDialog::drag_point(const wxPoint& position)
{
    if (position == m_drag_previous)
        return;
    m_drag_previous = position;
    const int row = m_dragged_point;
    const int last = int(m_lengths.size()) - 1;
    const wxRect plot = chart_rect();
    const wxPoint delta = position - m_drag_start;
    double length = std::clamp(m_drag_length / m_chart_max_length + double(delta.x) / plot.width, 0.0, 1.0) * m_chart_max_length;
    double factor = m_chart_min_factor + std::clamp((m_drag_factor - m_chart_min_factor) / (1.0 - m_chart_min_factor) - double(delta.y) / plot.height, 0.0, 1.0) * (1.0 - m_chart_min_factor);
    if (delta.x == 0)
        length = m_drag_length;
    if (delta.y == 0)
        factor = m_drag_factor;

    // Keep the model valid without sorting points or changing their neighbours.
    const double lower_length = row == 0 ? 0.0 : std::nextafter(m_lengths[row - 1], std::numeric_limits<double>::infinity());
    const double upper_length = row == last ? m_chart_max_length : std::nextafter(m_lengths[row + 1], -std::numeric_limits<double>::infinity());
    const double lower_factor = row == 0 ? m_chart_min_factor : std::nextafter(m_factors[row - 1], std::numeric_limits<double>::infinity());
    const double upper_factor = row == last ? 1.0 : std::nextafter(m_factors[row + 1], -std::numeric_limits<double>::infinity());
    length = row == 0 ? 0.0 : std::clamp(length, lower_length, upper_length);
    factor = row == last ? 1.0 : std::clamp(factor, lower_factor, upper_factor);

    // Round mouse input for readable values; retain full precision for tightly spaced points.
    auto set_value = [this, row](int column, double value, double lower, double upper, double& current) {
        if (value == current)
            return;
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
    set_value(0, length, lower_length, upper_length, m_lengths[row]);
    set_value(1, factor, lower_factor, upper_factor, m_factors[row]);
    m_chart->Refresh();
}

void SmallAreaInfillFlowCompensationDialog::finish_drag()
{
    if (m_dragged_point < 0)
        return;
    m_dragged_point = -1;
    if (m_chart->HasCapture())
        m_chart->ReleaseMouse();
    m_chart->SetCursor(wxCursor(wxCURSOR_ARROW));
    update_preview();
}

void SmallAreaInfillFlowCompensationDialog::paint_chart()
{
    wxAutoBufferedPaintDC dc(m_chart);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();
    dc.SetTextForeground(GetForegroundColour());
    dc.SetFont(GetFont());
    const wxRect plot = chart_rect();
    const int left = plot.x, top = plot.y, width = plot.width, height = plot.height;
    if (width <= 0 || height <= 0)
        return;
    dc.DrawText(_L("Flow correction factor"), left, 0);
    const wxString x_label = _L("Extrusion length") + " (mm)";
    dc.DrawText(x_label, left + (width - dc.GetTextExtent(x_label).x) / 2, top + height + FromDIP(23));
    if (m_lengths.empty())
        return;

    // Use the same PCHIP interpolation as the slicing algorithm, not the ramming chart's curve.
    const PchipInterpolatorHelper model(m_lengths, m_factors);
    dc.SetPen(wxPen(GetForegroundColour()));
    dc.DrawLine(left, top, left, top + height);
    dc.DrawLine(left, top + height, left + width, top + height);
    const wxString minimum_label = wxString::Format("%.3g", m_chart_min_factor);
    dc.DrawText(minimum_label, left - dc.GetTextExtent(minimum_label).x - FromDIP(8), top + height - dc.GetCharHeight());
    dc.DrawText("1", left - dc.GetTextExtent("1").x - FromDIP(8), top);
    dc.DrawText("0", left, top + height + FromDIP(3));
    const wxString maximum = wxString::Format("%.4g", m_chart_max_length);
    dc.DrawText(maximum, left + width - dc.GetTextExtent(maximum).x, top + height + FromDIP(3));
    const wxColour colour = StateColor::darkModeColorFor(wxColour("#009688"));
    dc.SetPen(wxPen(colour, FromDIP(2)));
    wxPoint previous = chart_point(0, m_factors.front());
    for (int pixel = 1; pixel <= width; ++pixel) {
        const double x = m_chart_max_length * (double(pixel) / width);
        const double y = model.interpolate(x);
        if (!std::isfinite(y))
            return;
        const wxPoint current = chart_point(x, y);
        dc.DrawLine(previous, current);
        previous = current;
    }
    dc.SetBrush(wxBrush(colour));
    for (size_t i = 0; i < m_lengths.size(); ++i) {
        const bool selected = int(i) == m_grid->GetGridCursorRow();
        dc.SetPen(wxPen(selected ? GetForegroundColour() : colour, FromDIP(2)));
        dc.DrawCircle(chart_point(m_lengths[i], m_factors[i]), FromDIP(selected ? 5 : 4));
    }
}

} // namespace Slic3r::GUI
