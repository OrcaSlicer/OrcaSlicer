#ifndef slic3r_SliceInfoPanel_hpp_
#define slic3r_SliceInfoPanel_hpp_

#include "slic3r/GUI/MonitorBasePanel.h"
#include "libslic3r/ProjectTask.hpp"
#include "DeviceManager.hpp"
#include "GUI.hpp"
#include <wx/panel.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/webrequest.h>
#include "Widgets/PopupWindow.hpp"

namespace Slic3r {
namespace GUI {

class SliceInfoPopup : public PopupWindow
{
public:
    SliceInfoPopup(wxWindow *parent, wxBitmap bmp= wxNullBitmap, BBLSliceInfo* info=nullptr);
    virtual ~SliceInfoPopup() {}

    // PopupWindow virtual methods are all overridden to log them
    virtual void Popup(wxWindow *focus = NULL) wxOVERRIDE;
    virtual void OnDismiss() wxOVERRIDE;
    virtual bool ProcessLeftDown(wxMouseEvent &event) wxOVERRIDE;
    virtual bool Show(bool show = true) wxOVERRIDE;

private:
    wxScrolledWindow *m_panel;
    BBLSliceInfo *m_info { nullptr };

    void OnMouse(wxMouseEvent &event);
    void OnSize(wxSizeEvent &event);
    void OnSetFocus(wxFocusEvent &event);
    void OnKillFocus(wxFocusEvent &event);

private:
    wxDECLARE_ABSTRACT_CLASS(SliceInfoPopup);
    wxDECLARE_EVENT_TABLE();
};

class SliceInfoPanel : public wxPanel
{
private:
protected:
    wxWebRequest web_request;
    std::shared_ptr<ImageTransientPopup> m_thumbnail_popup;
    std::shared_ptr<SliceInfoPopup> m_slice_info_popup;

    wxImage m_thumbnail_img;

    wxBoxSizer *    m_item_top_sizer;
    wxStaticBitmap* m_bmp_item_thumbnail;
    wxStaticBitmap* m_bmp_item_prediction;
    wxStaticBitmap* m_bmp_item_print;
    wxStaticText*   m_text_item_prediction;
    wxStaticBitmap* m_bmp_item_cost;
    wxStaticText*   m_text_item_cost;
    wxGridSizer*    m_filament_info_sizer;
    wxStaticText*   m_text_plate_index;
    
public:
    SliceInfoPanel(wxWindow *      parent,
                 wxBitmap   &prediction,
                 wxBitmap   &cost,
                 wxBitmap   &print,
                 wxWindowID      id    = wxID_ANY,
                 const wxPoint & pos   = wxDefaultPosition,
                 const wxSize &  size  = wxDefaultSize,
                 long            style = wxTAB_TRAVERSAL,
                 const wxString &name  = wxEmptyString);
    ~SliceInfoPanel();

    void SetImages(wxBitmap &prediction, wxBitmap &cost, wxBitmap &printing);

    void on_subtask_print(wxCommandEvent &evt);
    void on_thumbnail_enter(wxMouseEvent &event);
    void on_thumbnail_leave(wxMouseEvent &event);

    void on_mouse_enter(wxMouseEvent &event);
    void on_mouse_leave(wxMouseEvent &event);

    void on_webrequest_state(wxWebRequestEvent &evt);
    void update(BBLSliceInfo* info);
    void msw_rescale();
};


}
}
#endif
