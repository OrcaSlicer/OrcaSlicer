#ifndef slic3r_GUI_ProgressBar_hpp_
#define slic3r_GUI_ProgressBar_hpp_

#include <wx/window.h>
#include "../wxExtensions.hpp"

class ProgressBar : public wxWindow
{
public: 
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
    void         Rescale();
    void         SetHeight(int height) {
        m_minHeight = height;
        m_radius    = m_minHeight / 2;
        SetSize(GetSize().x,  height);
    }
    virtual void SetMinSize(const wxSize &size) override;

protected:
    void         paintEvent(wxPaintEvent &evt);
    void         render(wxDC &dc);
    void         doRender(wxDC &dc);
    virtual void DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO);



    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_ProgressBar_hpp_