#ifndef slic3r_GUI_TabCtrl_hpp_
#define slic3r_GUI_TabCtrl_hpp_

#include "Button.hpp"

wxDECLARE_EVENT( wxEVT_TAB_SEL_CHANGING, wxCommandEvent );
wxDECLARE_EVENT( wxEVT_TAB_SEL_CHANGED, wxCommandEvent );

class TabCtrl : public StaticBox
{
    std::vector<Button*> btns;
    wxImageList* images = nullptr;
    wxBoxSizer * sizer = nullptr;

    int sel = -1;
    wxFont bold;

public:
    TabCtrl(wxWindow *      parent,
             wxWindowID      id,
             const wxPoint & pos       = wxDefaultPosition,
             const wxSize &  size      = wxDefaultSize,
             long            style     = 0);

    ~TabCtrl();

public:
    virtual bool SetFont(wxFont const & font) override;

public:
    int AppendItem(const wxString &item, int image = -1, int selImage = -1, void *clientData = nullptr);

    bool DeleteItem(int item);

    void DeleteAllItems();

    unsigned int GetCount() const;

    int  GetSelection() const;

    void SelectItem(int item);

    void Unselect();

    virtual void Rescale();

    wxString GetItemText(unsigned int item) const;
    void     SetItemText(unsigned int item, wxString const &value);

    bool     GetItemBold(unsigned int item) const;
    void     SetItemBold(unsigned int item, bool bold);

    void*    GetItemData(unsigned int item) const;
    void     SetItemData(unsigned int item, void *clientData);
    
    void AssignImageList(wxImageList *imageList);

    void SetItemTextColour(unsigned int item, const StateColor& col);

    /* fakes */
    int GetFirstVisibleItem() const;
    int GetNextVisible(int item) const;
    bool IsVisible(unsigned int item) const;

private:
    virtual void DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO);

#ifdef __WIN32__
    WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

    void relayout();

    void buttonClicked(wxCommandEvent & event);
    void keyDown(wxKeyEvent &event);

    void doRender(wxDC & dc) override;

    // some useful events
    bool sendTabCtrlEvent(bool changing = false);

    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_TabCtrl_hpp_
