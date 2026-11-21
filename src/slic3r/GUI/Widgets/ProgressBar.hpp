#ifndef slic3r_GUI_ProgressBar_hpp_
#define slic3r_GUI_ProgressBar_hpp_

#include <optional>
#include <vector>

#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/colour.h>
#include <wx/string.h>
#include <wx/window.h>
#include "../wxExtensions.hpp"

// Sent to the bar itself (it does not propagate) when its control height changes, so the owner can re-layout.
wxDECLARE_EVENT(EVT_PROGRESS_BAR_HEIGHT_CHANGED, wxCommandEvent);

class ProgressBar : public wxWindow
{
public:
    // A tick on the bar. A marker with a label shows it in a bubble under the bar while hovered.
    struct Marker
    {
        int      m_position = 0; // in the units of the bar's maximum
        wxString m_label;
    };

    ProgressBar();
    ProgressBar(wxWindow *         parent,
                wxWindowID         id        = wxID_ANY,
                int                max       = 100,
                const wxPoint &    pos       = wxDefaultPosition, 
                const wxSize &     size      = wxDefaultSize,
                bool               shown     = false);


    void create(wxWindow *parent, wxWindowID id,  const wxPoint &pos, wxSize &size);

    ~ProgressBar();

public:
    bool     m_shownumber                 = {false};
    int      m_disable                    = {false};
    int      m_max                        = {100};
    int      m_step                       = {0};
    int      m_miniHeight                 = {0};
    const int      miniHeight             = {14};
    double   m_radius                     = {7};
    double   m_proportion                 = {0};
    wxColour m_progress_background_colour = {233, 233, 233};
    wxColour m_progress_colour            = {0, 150, 136};
    wxColour m_progress_colour_disable    = {255, 111, 0};
    wxString m_disable_text;
    

public:
    void         ShowNumber(bool shown);
    void         Disable(wxString text);
    void         SetValue(int  step);
    void         Reset();
    void         SetProgress(int step);
    void         SetRadius(double radius);
    void         SetProgressForedColour(wxColour colour);
    void         SetProgressBackgroundColour(wxColour colour);
    void         SetMarkers(const std::vector<Marker> &markers);
    void         ClearMarkers() { SetMarkers({}); }
    void         Rescale();
    // Height of the bar itself; the control grows beyond it while markers have labels to show.
    void         SetHeight(int height);
    virtual void SetMinSize(const wxSize &size) override;

protected:
    void         paintEvent(wxPaintEvent &evt);
    void         mouseMove(wxMouseEvent &evt);
    void         mouseLeave(wxMouseEvent &evt);
    void         render(wxDC &dc);
    void         doRender(wxDC &dc);
    void         renderMarkers(wxDC &dc, const wxSize &size, int barHeight);
    virtual void DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO) override;

private:
    int  findHoveredMarker(const wxPoint &position) const;
    void updateControlHeight();

    int                    m_barHeight = miniHeight;
    int                    m_hoveredMarker = -1;
    std::optional<wxPoint> m_lastMousePosition;
    std::vector<Marker>    m_markers;

    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_ProgressBar_hpp_