#pragma once

#include <string>
#include <vector>
#include <wx/dialog.h>

class wxGrid;
class wxPanel;
class wxStaticText;

namespace Slic3r::GUI {

class SmallAreaInfillFlowCompensationDialog : public wxDialog
{
public:
    SmallAreaInfillFlowCompensationDialog(wxWindow* parent, const std::vector<std::string>& parameters);
    const std::string& get_parameters() const { return m_output_data; }
    bool is_modified() const { return m_modified; }

private:
    void load_points(const std::vector<std::string>& parameters);
    bool read_points(std::vector<double>& lengths, std::vector<double>& factors);
    void update_preview();
    void paint_chart();
    void finish_edit();
    wxRect chart_rect() const;
    wxPoint chart_point(double length, double factor) const;
    int hit_test(const wxPoint& position) const;
    void drag_point(const wxPoint& position);
    void finish_drag();

    wxGrid*       m_grid;
    wxPanel*      m_chart;
    wxStaticText* m_status;
    std::vector<std::string> m_initial_parameters;
    std::vector<double> m_lengths;
    std::vector<double> m_factors;
    std::string m_output_data;
    bool m_modified = false;
    int m_dragged_point = -1;
    wxPoint m_drag_start;
    wxPoint m_drag_previous;
    double m_drag_length = 0.0;
    double m_drag_factor = 0.0;
    double m_chart_max_length = 1.0;
    double m_chart_min_factor = 0.0;
};

} // namespace Slic3r::GUI
