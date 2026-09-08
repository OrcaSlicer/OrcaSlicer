#include "CoExtrusionProfileDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/Color.hpp"

#include <wx/colordlg.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/msgdlg.h>
#include <wx/statline.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace Slic3r::GUI {

namespace {

constexpr double PI = 3.14159265358979323846;

wxSpinCtrlDouble* make_angle_ctrl(wxWindow *parent, double value)
{
    auto *control = new wxSpinCtrlDouble(parent, wxID_ANY);
    control->SetRange(-100000.0, 100000.0);
    control->SetDigits(2);
    control->SetIncrement(1.0);
    control->SetValue(value);
    return control;
}

double alignment_offset_from_observation(double measured_azimuth_deg, double sector_center_deg,
    double commanded_angle_deg, int axis_direction, double axis_zero_offset_deg) noexcept
{
    const int direction = axis_direction == -1 ? -1 : 1;
    double offset = std::fmod(measured_azimuth_deg - sector_center_deg - axis_zero_offset_deg -
        double(direction) * commanded_angle_deg, 360.0);
    if (offset < -180.0)
        offset += 360.0;
    else if (offset > 180.0)
        offset -= 360.0;
    return offset;
}

std::optional<CoExtrusion::DelayCalibrationEstimate> delay_from_boundary_shift(
    double boundary_shift_mm, double speed_mm_s, double line_width_mm, double layer_height_mm) noexcept
{
    if (!std::isfinite(boundary_shift_mm) || boundary_shift_mm < 0.0 ||
        !std::isfinite(speed_mm_s) || speed_mm_s <= 0.0 ||
        !std::isfinite(line_width_mm) || line_width_mm <= 0.0 ||
        !std::isfinite(layer_height_mm) || layer_height_mm <= 0.0)
        return std::nullopt;
    return CoExtrusion::DelayCalibrationEstimate {
        boundary_shift_mm / speed_mm_s,
        boundary_shift_mm * line_width_mm * layer_height_mm
    };
}

} // namespace

class CoExtrusionCrossSectionPreview final : public wxPanel
{
public:
    explicit CoExtrusionCrossSectionPreview(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition,
            parent != nullptr ? parent->FromDIP(wxSize(260, 260)) : wxSize(260, 260),
            wxBORDER_SIMPLE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &CoExtrusionCrossSectionPreview::paint, this);
    }

    void set_profile(std::vector<CoExtrusion::ColorSector> sectors)
    {
        m_sectors = std::move(sectors);
        Refresh();
    }

private:
    void paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(wxGetApp().get_window_default_clr()));
        dc.Clear();

        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;

        const wxSize size = GetClientSize();
        const double radius = 0.40 * std::min(size.x, size.y);
        const double cx = 0.5 * size.x;
        const double cy = 0.5 * size.y;
        for (const CoExtrusion::ColorSector &sector : m_sectors) {
            ColorRGBA rgba = ColorRGBA::MAGENTA();
            decode_color(sector.preview_color, rgba);
            const wxColour color(rgba.r_uchar(), rgba.g_uchar(), rgba.b_uchar(), rgba.a_uchar());
            wxGraphicsPath path = gc->CreatePath();
            path.MoveToPoint(cx, cy);
            // Construct the sector explicitly instead of using AddArc(). The
            // clockwise flag has different-looking results in screen-space
            // coordinates and previously turned a 90 degree sector into the
            // complementary 270 degree sector, causing later colors to cover
            // earlier ones. Show the physical top view: +X right, +Y down,
            // with positive profile angles clockwise from +X. Stored numeric
            // angles still run from +X toward +Y, as in existing profiles.
            const double start_deg = sector.center_angle_deg - 0.5 * sector.angular_width_deg;
            const int steps = std::clamp(int(std::ceil(sector.angular_width_deg / 5.0)), 2, 72);
            for (int step = 0; step <= steps; ++step) {
                const double angle_deg = start_deg + sector.angular_width_deg * double(step) / double(steps);
                const double angle_rad = angle_deg * PI / 180.0;
                path.AddLineToPoint(
                    cx + radius * std::cos(angle_rad),
                    cy + radius * std::sin(angle_rad));
            }
            path.CloseSubpath();
            gc->SetBrush(wxBrush(color));
            gc->SetPen(wxPen(wxColour(40, 40, 40), FromDIP(1)));
            gc->DrawPath(path);
        }

        gc->SetPen(wxPen(wxColour(240, 80, 80), FromDIP(2)));
        gc->StrokeLine(cx, cy, cx + radius + FromDIP(12), cy);
        gc->SetFont(GetFont(), wxGetApp().get_label_clr_default());
        gc->DrawText(_L("+X / 0 deg"), cx + FromDIP(8), cy + FromDIP(6));
        gc->DrawText(_L("+Y / 90 deg"), cx + FromDIP(8), cy + radius - FromDIP(18));
    }

    std::vector<CoExtrusion::ColorSector> m_sectors;
};

CoExtrusionProfileDialog::CoExtrusionProfileDialog(
    wxWindow *parent,
    const std::string &serialized_profile,
    double calibration_offset_deg,
    const std::string &delay_model,
    double response_delay_s,
    double transport_volume_mm3,
    int axis_direction,
    double axis_zero_offset_deg)
    : DPIDialog(parent, wxID_ANY, _L("Co-extrusion cross-section and calibration"),
        wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_serialized_profile(serialized_profile)
    , m_calibration_offset_deg(calibration_offset_deg)
    , m_axis_direction(axis_direction == -1 ? -1 : 1)
    , m_axis_zero_offset_deg(axis_zero_offset_deg)
{
    SetBackgroundColour(wxGetApp().get_window_default_clr());
    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *editor = new wxBoxSizer(wxHORIZONTAL);

    m_sector_table = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(520, 250)), wxDV_ROW_LINES | wxDV_VERT_RULES);
    m_sector_table->AppendTextColumn(_L("Color ID"), wxDATAVIEW_CELL_EDITABLE, FromDIP(80));
    m_sector_table->AppendTextColumn(_L("Preview color"), wxDATAVIEW_CELL_EDITABLE, FromDIP(120));
    m_sector_table->AppendTextColumn(_L("Center angle (deg)"), wxDATAVIEW_CELL_EDITABLE, FromDIP(140));
    m_sector_table->AppendTextColumn(_L("Angular width (deg)"), wxDATAVIEW_CELL_EDITABLE, FromDIP(150));
    editor->Add(m_sector_table, 1, wxEXPAND | wxALL, FromDIP(8));

    m_preview = new CoExtrusionCrossSectionPreview(this);
    editor->Add(m_preview, 0, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(8));
    root->Add(editor, 1, wxEXPAND);
    root->Add(new wxStaticText(this, wxID_ANY,
        _L("Top view: +X is 0 degrees; angles increase clockwise toward +Y (90 degrees).")),
        0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    root->Add(new wxStaticText(this, wxID_ANY,
        _L("The G-code rotating cross-section preview currently renders the first four sectors.")),
        0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    auto *row_buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *add = new wxButton(this, wxID_ANY, _L("Add sector"));
    auto *remove = new wxButton(this, wxID_ANY, _L("Remove sector"));
    auto *pick_color = new wxButton(this, wxID_ANY, _L("Choose color"));
    row_buttons->Add(add, 0, wxRIGHT, FromDIP(6));
    row_buttons->Add(remove, 0, wxRIGHT, FromDIP(6));
    row_buttons->Add(pick_color);
    root->Add(row_buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    auto *alignment = new wxStaticBoxSizer(wxVERTICAL, this, _L("C-axis installation alignment"));
    auto *alignment_grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(8));
    alignment_grid->Add(new wxStaticText(this, wxID_ANY, _L("Observed color sector")), 0, wxALIGN_CENTER_VERTICAL);
    m_calibration_sector = new wxChoice(this, wxID_ANY);
    alignment_grid->Add(m_calibration_sector, 1, wxEXPAND);
    alignment_grid->Add(new wxStaticText(this, wxID_ANY, _L("Commanded C angle")), 0, wxALIGN_CENTER_VERTICAL);
    m_command_angle = make_angle_ctrl(this, 0.0);
    alignment_grid->Add(m_command_angle, 1, wxEXPAND);
    alignment_grid->Add(new wxStaticText(this, wxID_ANY, _L("Measured physical azimuth")), 0, wxALIGN_CENTER_VERTICAL);
    m_measured_azimuth = make_angle_ctrl(this, 0.0);
    alignment_grid->Add(m_measured_azimuth, 1, wxEXPAND);
    alignment_grid->AddGrowableCol(1, 1);
    alignment->Add(alignment_grid, 0, wxEXPAND | wxALL, FromDIP(6));
    auto *alignment_result_row = new wxBoxSizer(wxHORIZONTAL);
    auto *calculate_alignment = new wxButton(this, wxID_ANY, _L("Calculate installation offset"));
    m_calibration_result = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Current offset: %.2f deg"), m_calibration_offset_deg));
    alignment_result_row->Add(calculate_alignment, 0, wxRIGHT, FromDIP(10));
    alignment_result_row->Add(m_calibration_result, 0, wxALIGN_CENTER_VERTICAL);
    alignment->Add(alignment_result_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
    root->Add(alignment, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    auto *delay = new wxStaticBoxSizer(wxVERTICAL, this, _L("Color response delay calibration"));
    auto *delay_grid = new wxFlexGridSizer(4, FromDIP(6), FromDIP(8));
    delay_grid->Add(new wxStaticText(this, wxID_ANY, _L("Delay model")), 0, wxALIGN_CENTER_VERTICAL);
    m_delay_model = new wxChoice(this, wxID_ANY);
    m_delay_model->Append("disabled");
    m_delay_model->Append("fixed_time");
    m_delay_model->Append("transport_volume");
    m_delay_model->SetStringSelection(from_u8(delay_model));
    if (m_delay_model->GetSelection() == wxNOT_FOUND)
        m_delay_model->SetSelection(0);
    delay_grid->Add(m_delay_model, 1, wxEXPAND);
    delay_grid->Add(new wxStaticText(this, wxID_ANY, _L("Boundary shift (mm)")), 0, wxALIGN_CENTER_VERTICAL);
    m_boundary_shift = make_angle_ctrl(this, 0.0);
    m_boundary_shift->SetRange(0.0, 1000.0);
    delay_grid->Add(m_boundary_shift, 1, wxEXPAND);
    delay_grid->Add(new wxStaticText(this, wxID_ANY, _L("Test speed (mm/s)")), 0, wxALIGN_CENTER_VERTICAL);
    m_test_speed = make_angle_ctrl(this, 40.0);
    m_test_speed->SetRange(0.01, 1000.0);
    delay_grid->Add(m_test_speed, 1, wxEXPAND);
    delay_grid->Add(new wxStaticText(this, wxID_ANY, _L("Line width (mm)")), 0, wxALIGN_CENTER_VERTICAL);
    m_test_line_width = make_angle_ctrl(this, 0.45);
    m_test_line_width->SetRange(0.01, 10.0);
    delay_grid->Add(m_test_line_width, 1, wxEXPAND);
    delay_grid->Add(new wxStaticText(this, wxID_ANY, _L("Layer height (mm)")), 0, wxALIGN_CENTER_VERTICAL);
    m_test_layer_height = make_angle_ctrl(this, 0.2);
    m_test_layer_height->SetRange(0.01, 10.0);
    delay_grid->Add(m_test_layer_height, 1, wxEXPAND);
    delay_grid->Add(new wxStaticText(this, wxID_ANY, _L("Response delay (s)")), 0, wxALIGN_CENTER_VERTICAL);
    m_response_delay = make_angle_ctrl(this, response_delay_s);
    m_response_delay->SetRange(0.0, 1000.0);
    delay_grid->Add(m_response_delay, 1, wxEXPAND);
    delay_grid->Add(new wxStaticText(this, wxID_ANY, _L("Transport volume (mm^3)")), 0, wxALIGN_CENTER_VERTICAL);
    m_transport_volume = make_angle_ctrl(this, transport_volume_mm3);
    m_transport_volume->SetRange(0.0, 10000.0);
    delay_grid->Add(m_transport_volume, 1, wxEXPAND);
    delay_grid->AddGrowableCol(1, 1);
    delay_grid->AddGrowableCol(3, 1);
    delay->Add(delay_grid, 0, wxEXPAND | wxALL, FromDIP(6));
    auto *calculate_delay = new wxButton(this, wxID_ANY, _L("Calculate delay values"));
    delay->Add(calculate_delay, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
    root->Add(delay, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    auto *dialog_buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    root->Add(dialog_buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    SetSizerAndFit(root);
    SetMinSize(FromDIP(wxSize(820, 720)));

    std::optional<CoExtrusion::Profile> profile = CoExtrusion::Profile::parse(serialized_profile);
    if (!profile || profile->empty()) {
        profile = CoExtrusion::Profile{};
        profile->sectors = {
            {1, "#FF0000", 0.0, 120.0},
            {2, "#0000FF", 120.0, 120.0},
            {3, "#00FF00", 240.0, 120.0}
        };
    }
    for (const CoExtrusion::ColorSector &sector : profile->sectors)
        append_sector(sector);
    refresh_preview_and_choices();

    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const unsigned next_id = unsigned(m_sector_table->GetItemCount() + 1);
        append_sector({next_id, "#FFFFFF", 0.0, 90.0});
        refresh_preview_and_choices();
    });
    remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const int row = m_sector_table->GetSelectedRow();
        if (row != wxNOT_FOUND)
            m_sector_table->DeleteItem(unsigned(row));
        refresh_preview_and_choices();
    });
    pick_color->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const int row = m_sector_table->GetSelectedRow();
        if (row == wxNOT_FOUND)
            return;
        wxColourData data;
        wxVariant current;
        m_sector_table->GetValue(current, unsigned(row), 1);
        data.SetColour(wxColour(current.GetString()));
        wxColourDialog dialog(this, &data);
        if (dialog.ShowModal() == wxID_OK) {
            const wxColour color = dialog.GetColourData().GetColour();
            m_sector_table->SetValue(wxVariant(color.GetAsString(wxC2S_HTML_SYNTAX)), unsigned(row), 1);
            refresh_preview_and_choices();
        }
    });
    m_sector_table->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, [this](wxDataViewEvent &) { refresh_preview_and_choices(); });
    calculate_alignment->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { calculate_alignment_offset(); });
    calculate_delay->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { calculate_delay_values(); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { accept(); }, wxID_OK);
    wxGetApp().UpdateDlgDarkUI(this);
}

void CoExtrusionProfileDialog::append_sector(const CoExtrusion::ColorSector &sector)
{
    wxVector<wxVariant> values;
    values.push_back(wxVariant(wxString::Format("%u", unsigned(sector.color_id))));
    values.push_back(wxVariant(from_u8(sector.preview_color)));
    values.push_back(wxVariant(wxString::Format("%.3f", sector.center_angle_deg)));
    values.push_back(wxVariant(wxString::Format("%.3f", sector.angular_width_deg)));
    m_sector_table->AppendItem(values);
}

std::optional<CoExtrusion::Profile> CoExtrusionProfileDialog::read_profile(bool show_error) const
{
    CoExtrusion::Profile profile;
    for (unsigned row = 0; row < m_sector_table->GetItemCount(); ++row) {
        wxVariant id_value, color_value, center_value, width_value;
        m_sector_table->GetValue(id_value, row, 0);
        m_sector_table->GetValue(color_value, row, 1);
        m_sector_table->GetValue(center_value, row, 2);
        m_sector_table->GetValue(width_value, row, 3);
        unsigned long id = 0;
        double center = 0.0;
        double width = 0.0;
        if (!id_value.GetString().ToULong(&id) ||
            id >= std::numeric_limits<CoExtrusion::SurfaceColorId>::max() ||
            !center_value.GetString().ToDouble(&center) ||
            !width_value.GetString().ToDouble(&width)) {
            if (show_error)
                wxMessageBox(_L("A sector contains an invalid numeric value."), _L("Invalid co-extrusion profile"), wxOK | wxICON_ERROR, const_cast<CoExtrusionProfileDialog*>(this));
            return std::nullopt;
        }
        profile.sectors.push_back({CoExtrusion::SurfaceColorId(id), into_u8(color_value.GetString()), center, width});
    }
    if (profile.sectors.empty()) {
        if (show_error)
            wxMessageBox(_L("At least one color sector is required."), _L("Invalid co-extrusion profile"), wxOK | wxICON_ERROR, const_cast<CoExtrusionProfileDialog*>(this));
        return std::nullopt;
    }
    const std::vector<std::string> errors = profile.validate();
    if (!errors.empty()) {
        if (show_error)
            wxMessageBox(from_u8(errors.front()), _L("Invalid co-extrusion profile"), wxOK | wxICON_ERROR, const_cast<CoExtrusionProfileDialog*>(this));
        return std::nullopt;
    }
    return profile;
}

void CoExtrusionProfileDialog::refresh_preview_and_choices()
{
    const std::optional<CoExtrusion::Profile> profile = read_profile(false);
    m_preview->set_profile(profile ? profile->sectors : std::vector<CoExtrusion::ColorSector>{});
    const int old_selection = m_calibration_sector->GetSelection();
    m_calibration_sector->Clear();
    if (profile)
        for (const CoExtrusion::ColorSector &sector : profile->sectors)
            m_calibration_sector->Append(wxString::Format(_L("Color ID %u"), unsigned(sector.color_id)));
    if (m_calibration_sector->GetCount() > 0)
        m_calibration_sector->SetSelection(std::clamp(old_selection, 0, int(m_calibration_sector->GetCount()) - 1));
}

void CoExtrusionProfileDialog::calculate_alignment_offset()
{
    const std::optional<CoExtrusion::Profile> profile = read_profile(true);
    const int selection = m_calibration_sector->GetSelection();
    if (!profile || selection == wxNOT_FOUND || size_t(selection) >= profile->sectors.size())
        return;
    const CoExtrusion::ColorSector &sector = profile->sectors[size_t(selection)];
    m_calibration_offset_deg = alignment_offset_from_observation(
        m_measured_azimuth->GetValue(), sector.center_angle_deg, m_command_angle->GetValue(),
        m_axis_direction, m_axis_zero_offset_deg);
    m_calibration_result->SetLabel(wxString::Format(_L("Calculated offset: %.2f deg"), m_calibration_offset_deg));
    Layout();
}

void CoExtrusionProfileDialog::calculate_delay_values()
{
    const std::optional<CoExtrusion::DelayCalibrationEstimate> estimate =
        delay_from_boundary_shift(
            m_boundary_shift->GetValue(), m_test_speed->GetValue(),
            m_test_line_width->GetValue(), m_test_layer_height->GetValue());
    if (!estimate) {
        wxMessageBox(_L("Delay calibration values must be positive and finite."), _L("Invalid calibration values"), wxOK | wxICON_ERROR, this);
        return;
    }
    m_response_delay->SetValue(estimate->response_delay_s);
    m_transport_volume->SetValue(estimate->transport_volume_mm3);
}

void CoExtrusionProfileDialog::accept()
{
    const std::optional<CoExtrusion::Profile> profile = read_profile(true);
    if (!profile)
        return;
    m_serialized_profile = profile->serialize();
    EndModal(wxID_OK);
}

std::string CoExtrusionProfileDialog::delay_model() const
{
    return into_u8(m_delay_model->GetStringSelection());
}

double CoExtrusionProfileDialog::response_delay_s() const
{
    return m_response_delay->GetValue();
}

double CoExtrusionProfileDialog::transport_volume_mm3() const
{
    return m_transport_volume->GetValue();
}

void CoExtrusionProfileDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    SetSize(suggested_rect);
    Layout();
}

} // namespace Slic3r::GUI
