#ifndef slic3r_GUI_StepCtrlBase_hpp_
#define slic3r_GUI_StepCtrlBase_hpp_

#include "StaticBox.hpp"

wxDECLARE_EVENT( EVT_STEP_CHANGING, wxCommandEvent );
wxDECLARE_EVENT( EVT_STEP_CHANGED, wxCommandEvent );

class StepCtrlBase : public StaticBox
{
protected:
    wxFont font_tip;
    StateColor clr_bar;
    StateColor clr_step;
    StateColor clr_text;
    StateColor clr_tip;
    int radius = 7;
    int bar_width = 4;

    std::vector<wxString> steps;
    std::vector<wxString> tips;
    wxString hint;

    int step = -1;

    wxPoint drag_offset;
    wxPoint pos_thumb;

public:
    StepCtrlBase(wxWindow *      parent,
             wxWindowID      id,
             const wxPoint & pos       = wxDefaultPosition,
             const wxSize &  size      = wxDefaultSize,
             long            style     = 0);

    ~StepCtrlBase();

public:
    void SetHint(wxString hint);

    bool SetTipFont(wxFont const & font);

public:
    int AppendItem(const wxString &item, wxString const & tip = {});

    void DeleteAllItems();

    unsigned int GetCount() const;

    int  GetSelection() const;

    void SelectItem(int item);
    void Idle();

    wxString GetItemText(unsigned int item) const;
    int      GetItemUseText(wxString txt) const;
    void     SetItemText(unsigned int item, wxString const& value);

private:
    // some useful events
    bool sendStepCtrlEvent(bool changing = false);
};

class StepCtrl : public StepCtrlBase
{
    ScalableBitmap bmp_thumb;

public:
    StepCtrl(wxWindow *      parent,
             wxWindowID      id,
             const wxPoint & pos       = wxDefaultPosition,
             const wxSize &  size      = wxDefaultSize,
             long            style     = 0);

    virtual void Rescale();

private:
    void mouseDown(wxMouseEvent &event);
    void mouseMove(wxMouseEvent &event);
    void mouseUp(wxMouseEvent &event);
    void mouseCaptureLost(wxMouseCaptureLostEvent &event);

    void doRender(wxDC &dc) override;

    DECLARE_EVENT_TABLE()
};

class StepIndicator : public StepCtrlBase
{
    ScalableBitmap bmp_ok;

public:
    StepIndicator(wxWindow *parent,
             wxWindowID      id,
             const wxPoint & pos       = wxDefaultPosition,
             const wxSize &  size      = wxDefaultSize,
             long            style     = 0);

    virtual void Rescale();

    void SelectNext();
private:
    void doRender(wxDC &dc) override;
};


class FilamentStepIndicator : public StepCtrlBase

{
    ScalableBitmap bmp_ok;
    //wxBitmap bmp_extruder;
    wxString m_slot_information = "";

public:
    FilamentStepIndicator(wxWindow* parent,
        wxWindowID      id,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long            style = 0);

    virtual void Rescale();

    void SelectNext();
    void SetSlotInformation(wxString slot);
private:
    void doRender(wxDC& dc) override;
};


#endif // !slic3r_GUI_StepCtrlBase_hpp_
