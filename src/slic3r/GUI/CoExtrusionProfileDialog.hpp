#pragma once

#include "GUI_Utils.hpp"
#include "libslic3r/CoExtrusion/CoExtrusionTypes.hpp"

#include <wx/choice.h>
#include <wx/dataview.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

namespace Slic3r::GUI {

class CoExtrusionCrossSectionPreview;

class CoExtrusionProfileDialog final : public DPIDialog
{
public:
    CoExtrusionProfileDialog(
        wxWindow *parent,
        const std::string &serialized_profile,
        double calibration_offset_deg,
        const std::string &delay_model,
        double response_delay_s,
        double transport_volume_mm3,
        int axis_direction,
        double axis_zero_offset_deg);

    std::string serialized_profile() const { return m_serialized_profile; }
    double calibration_offset_deg() const { return m_calibration_offset_deg; }
    std::string delay_model() const;
    double response_delay_s() const;
    double transport_volume_mm3() const;

    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void append_sector(const CoExtrusion::ColorSector &sector);
    std::optional<CoExtrusion::Profile> read_profile(bool show_error) const;
    void refresh_preview_and_choices();
    void calculate_alignment_offset();
    void calculate_delay_values();
    void accept();

    wxDataViewListCtrl                   *m_sector_table { nullptr };
    CoExtrusionCrossSectionPreview      *m_preview { nullptr };
    wxChoice                            *m_calibration_sector { nullptr };
    wxSpinCtrlDouble                    *m_command_angle { nullptr };
    wxSpinCtrlDouble                    *m_measured_azimuth { nullptr };
    wxStaticText                        *m_calibration_result { nullptr };
    wxChoice                            *m_delay_model { nullptr };
    wxSpinCtrlDouble                    *m_boundary_shift { nullptr };
    wxSpinCtrlDouble                    *m_test_speed { nullptr };
    wxSpinCtrlDouble                    *m_test_line_width { nullptr };
    wxSpinCtrlDouble                    *m_test_layer_height { nullptr };
    wxSpinCtrlDouble                    *m_response_delay { nullptr };
    wxSpinCtrlDouble                    *m_transport_volume { nullptr };

    std::string m_serialized_profile;
    double      m_calibration_offset_deg { 0.0 };
    int         m_axis_direction { 1 };
    double      m_axis_zero_offset_deg { 0.0 };
};

} // namespace Slic3r::GUI
