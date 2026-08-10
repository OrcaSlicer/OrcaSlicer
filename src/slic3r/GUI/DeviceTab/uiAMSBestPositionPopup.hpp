//**********************************************************/
/* File: uiAMSBestPositionPopup.hpp
*  Description: The popup with suggest best ams position
*
//**********************************************************/

#pragma once
#include "slic3r/GUI/Widgets/AMSItem.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/PopupWindow.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSwitch.h"
#include <algorithm>

#include <tuple>

namespace Slic3r { namespace GUI {


class UiStyledAMSPanel : public wxPanel
{
public:
    UiStyledAMSPanel(wxWindow* parent,
                wxWindowID id = wxID_ANY,
                const wxPoint& pos = wxDefaultPosition,
                const wxSize& size = wxDefaultSize,
                const wxColour& borderColor = wxColour(200, 200, 200),
                const wxColour& bgColor = wxColour(255, 255, 255),
                bool borderDashed = true,
                wxString name = "",
                bool isTop = false);

protected:
    void OnPaint(wxPaintEvent& event);

private:

    bool m_borderDashed;
    int m_borderWidth;
    int m_radius;
    wxColour m_borderColor;
    wxColour m_bgColor;
    wxString m_name;
    bool m_isTop;
};

class UiStyledSwitchPanel : public wxPanel
{
public:
    UiStyledSwitchPanel(wxWindow* parent,
                        wxWindowID id,
                        const wxPoint& pos,
                        const wxSize& size,
                        const wxColour& borderColor,
                        const wxColour& bgColor,
                        bool borderDashed,
                        int borderWidth,
                        int radius,
                        bool isTop);


    void AddToLeft(wxWindow* window, int proportion = 0, int flag = wxEXPAND, int border = 0);
    void AddToRight(wxWindow* window, int proportion = 0, int flag = wxEXPAND, int border = 0);
    void Clear(bool deleteWindows);

    wxSizer* GetLeftSizer() { return m_leftSizer; }
    wxSizer* GetRightSizer() { return m_rightSizer; }
    void LayoutAndFit()
    {
        m_leftSizer->Layout();
        // m_leftSizer->Fit();
        m_rightSizer->Layout();
        // m_rightSizer->Fit();
        m_splitSizer->Layout();
        // m_splitSizer->Fit();
        m_contentSizer->Layout();
        // m_contentSizer->Fit();
        m_mainSizer->Layout();
        // m_mainSizer->Fit();
        Fit();
    }
protected:
    void OnPaint(wxPaintEvent& event);
private:

    bool m_borderDashed;
    int m_borderWidth;
    int m_radius;
    wxColour m_borderColor;
    wxColour m_bgColor;
    bool m_isTop;
 
    wxBoxSizer* m_mainSizer{nullptr};
    wxBoxSizer* m_contentSizer{nullptr};
    wxBoxSizer* m_splitSizer{nullptr};
    wxSizer* m_leftSizer{nullptr};
    wxSizer* m_rightSizer{nullptr};
    
    static constexpr int labelHeight = 30;

};


enum DataStatusType {
    ADJUST,
    OK,
    UNMATCHED
};

class UiAMSSlot : public wxPanel
{
public:
    UiAMSSlot(wxWindow* parent,
            const std::vector<wxColour>& bgColours,
            const wxString&  text,
            DataStatusType status,
            wxWindowID id = wxID_ANY,
            const wxPoint& pos = wxDefaultPosition,
            const wxSize& size = wxDefaultSize,
            double colourFactor = 1.0,
            double scaleFactor = 1.0);


private:
    void OnPaint(wxPaintEvent&);
    void DrawRectangle(wxPaintDC& dc, const wxSize& cli);
    void DrawLine(wxPaintDC& dc, const wxSize& cli);
    wxColour LightenColour(const wxColour& original);
    bool IsDark(const wxColour& c)
    {
        int brightness = (c.Red() * 299 + c.Green() * 587 + c.Blue() * 114) / 1000;
        return brightness < 128;  // 0-255 range, 128 mid
    }
private:
    // Copied, not referenced: the slot repaints long after construction, so it must not
    // depend on the caller's vector staying alive.
    std::vector<wxColour> m_bgColours;
    wxString m_text;
    DataStatusType m_status;
    wxSize m_size;
    ScalableBitmap *m_ams_slot_readonly{nullptr};
    int m_rectangleW = 44;
    int m_rectangleH = 62;
    int rectangleW = 44;
    int rectangleH = 62;
    double m_colourFactor = 1.0f;
    double m_scaleFactor = 1.0f;
    // wxDECLARE_EVENT_TABLE();
};

struct DataAmsSlotInfo
{
    wxString amsName;
    wxString name;
    std::vector<wxColour> colours;
    double colourFactor;
    double scaleFactor;
    DataStatusType status;
};

class UiAMS : public UiStyledAMSPanel
{
public:
    UiAMS(wxWindow* parent,
        const std::vector<DataAmsSlotInfo>& amsInfo,
        wxWindowID id,
        const wxPoint& pos,
        const wxSize& minSize);            
private:
    void init();
    std::vector<DataAmsSlotInfo> m_amsInfo;
    wxString m_amsTitle;
    wxSize m_minSize;
};


struct DataStatusParam {
    // int count = 0;
    int width = 0;
    int height = 0;
    std::vector<DataAmsSlotInfo> slots;
};

class ReselectMachineDialog : public wxDialog
{
public:
    ReselectMachineDialog(wxWindow* parent);
    ~ReselectMachineDialog();
    void Update(MachineObject* obj,
                const std::map<int, int>&  best_pos_map,
                const std::vector<FilamentInfo>& ams_mapping,
                wxString save_time);

private:
    int CaculateSwitcherDistribution(MachineObject* obj, const std::map<int, int>&  best_pos_map, const std::vector<FilamentInfo>& ams_mapping);
    wxString getTrayID(MachineObject* obj, const std::string& amsID, const std::string& slotID);
    void OnRefreshButton(wxCommandEvent& event);

private:
    int saveTimes{0};
    wxBoxSizer* mainSizer{nullptr};
    wxPanel* textPanel{nullptr};
    wxBoxSizer* textSizer{nullptr};
    Label* suggestText{nullptr};
    // wxHyperlinkCtrl* linkwiki{nullptr};
    Label* linkwiki{nullptr};
    Label* summaryText{nullptr};
    UiStyledSwitchPanel* filamentSwitch{nullptr};
    wxStaticText* filamentTips{nullptr};
    std::vector<std::vector<DataAmsSlotInfo>> inAAMS{};
    std::vector<std::vector<DataAmsSlotInfo>> inBAMS{};
    wxPanel* statusBar{nullptr};
    wxBoxSizer* btnSizer{nullptr};
    Button* m_buttonClose{nullptr };
    Button* m_buttonRefresh{ nullptr };
    std::vector<wxColour>colourAdjust{wxColour("#675AFF")};
    std::vector<wxColour>colourOK{wxColour("#FF8181")};
    std::vector<wxColour>colourUnused{wxColour("#FF818140")};
    // wxStaticBitmap* m_bitmapSelectMachine{nullptr};
};

wxDECLARE_EVENT(wxEVT_REFRESH_DATA, wxCommandEvent);

}} // namespace Slic3r::GUI