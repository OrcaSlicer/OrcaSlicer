#ifndef slic3r_Notebook_hpp_
#define slic3r_Notebook_hpp_

//#ifdef _WIN32

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include <wx/bookctrl.h>
#include <wx/bitmap.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/debug.h>
#include <wx/chartype.h>
#include <wx/object.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/withimages.h>
#include <wx/window.h>
#include <wx/defs.h>
#include <wx/notebook.h>

class wxObject;

class Button;

// custom message the ButtonsListCtrl sends to its parent (Notebook) to notify a selection change:
wxDECLARE_EVENT(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, wxCommandEvent);
// ORCA: posted to the Notebook after the user dragged a tab to a new position.
wxDECLARE_EVENT(wxCUSTOMEVT_NOTEBOOK_PAGES_REORDERED, wxCommandEvent);

class ButtonsListCtrl : public wxControl
{
public:
    // BBS
    ButtonsListCtrl(wxWindow* parent, wxBoxSizer* side_tools = NULL);
    ~ButtonsListCtrl() {}

    void OnPaint(wxPaintEvent&);
    void SetSelection(int sel);
    void UpdateMode();
    void Rescale();
    bool InsertPage(size_t n, const wxString &text, bool bSelect = false, const std::string &bmp_name = "", const wxBitmap &bmp = wxNullBitmap);
    void RemovePage(size_t n);
    bool SetPageImage(size_t n, const std::string& bmp_name) const;
    void SetPageText(size_t n, const wxString& strText);
    void SetCompact(size_t n, bool compact); // ORCA
    wxString GetPageText(size_t n) const;
    // ORCA: the full page label, unaffected by SetCompact() blanking the button text.
    wxString GetPageLabel(size_t n) const;
    // Resource name the page was inserted with (empty for plugin pages, which pass a wxBitmap).
    const std::string& GetPageIcon(size_t n) const
    {
        static const std::string empty;
        return n < m_pageIcons.size() ? m_pageIcons[n] : empty;
    }
    wxFlexGridSizer* GetBtnsSizer(){return m_buttons_sizer;}; // ORCA
    // ORCA: a companion widget shown right after the tab buttons (before any side_tools), e.g.
    // an overflow indicator. Pass nullptr to remove it; ownership stays with the caller.
    void SetOverflowButton(wxWindow* button);
    wxWindow* GetOverflowButton() const { return m_overflow_button; }
    // ORCA: moves tab button `from` to index `to`, keeping the selection on the same button.
    void MovePage(size_t from, size_t to);
    // ORCA: hold Cmd (macOS) or Ctrl and drag a tab to move it. Off by default.
    void EnableReorder(bool enable) { m_reorder_enabled = enable; }
    // ORCA: width of a tab button, full or compacted to its icon.
    int GetTabWidth(bool compact);
    // ORCA: width of a tab showing `label` in full: its text, icon and padding, at least GetTabWidth(false).
    int GetFullTabWidth(const wxString& label);
    // ORCA: width the tab buttons and the overflow button can use without covering the side tools.
    int GetTabsAreaWidth() const;

private:
    void OnTabLeftDown(wxMouseEvent& evt, Button* btn);
    void OnDragMotion(wxMouseEvent& evt);
    void OnDragKey(wxKeyEvent& evt);
    void EndDrag(bool commit);

    wxFlexGridSizer*                m_buttons_sizer;
    wxBoxSizer*                     m_sizer;
    // BBS: use Button
    std::vector<Button*>            m_pageButtons;
    int                             m_selection {-1};
    int                             m_btn_margin;
    int                             m_line_margin;
    std::vector<wxString>           m_pageLabels; // ORCA
    std::vector<std::string>        m_pageIcons;  // ORCA: resource icon name per page, plugin pages empty
    wxWindow*                       m_overflow_button{nullptr}; // ORCA
    wxBoxSizer*                     m_side_tools{nullptr};      // ORCA
    bool                            m_reorder_enabled{false};   // ORCA
    int                             m_drag_from{-1};            // ORCA: index the dragged tab started at, -1 when not dragging
    int                             m_drag_at{-1};              // ORCA: index the dragged tab is at now
};

class Notebook : public wxBookCtrlBase
{
public:
    Notebook(wxWindow * parent,
                 wxWindowID winid = wxID_ANY,
                 const wxPoint & pos = wxDefaultPosition,
                 const wxSize & size = wxDefaultSize,
                // BBS
                 wxBoxSizer* side_tools = NULL,
                 long style = 0)
    {
        Init();
        Create(parent, winid, pos, size, side_tools, style);
    }

    bool Create(wxWindow * parent,
                wxWindowID winid = wxID_ANY,
                const wxPoint & pos = wxDefaultPosition,
                const wxSize & size = wxDefaultSize,
                // BBS
                wxBoxSizer* side_tools = NULL,
                long style = 0)
    {
        if (!wxBookCtrlBase::Create(parent, winid, pos, size, style | wxBK_TOP))
            return false;

        m_bookctrl = new ButtonsListCtrl(this, side_tools);

        wxSizer* mainSizer = new wxBoxSizer(IsVertical() ? wxVERTICAL : wxHORIZONTAL);

        if (style & wxBK_RIGHT || style & wxBK_BOTTOM)
            mainSizer->Add(0, 0, 1, wxEXPAND, 0);

        m_controlSizer = new wxBoxSizer(IsVertical() ? wxHORIZONTAL : wxVERTICAL);
        m_controlSizer->Add(m_bookctrl, wxSizerFlags(1).Expand());
        wxSizerFlags flags;
        if (IsVertical())
            flags.Expand();
        else
            flags.CentreVertical();
        mainSizer->Add(m_controlSizer, flags.Border(wxALL, m_controlMargin));
        SetSizer(mainSizer);

        this->Bind(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, [this](wxCommandEvent& evt)
        {
            if (int page_idx = evt.GetId(); page_idx >= 0)
                SetSelection(page_idx);
        });

        this->Bind(wxEVT_NAVIGATION_KEY, &Notebook::OnNavigationKey, this);

        return true;
    }


    // Methods specific to this class.

    // A method allowing to add a new page without any label (which is unused
    // by this control) and show it immediately.
    bool ShowNewPage(wxWindow * page)
    {
        return AddPage(page, wxString(), false, NO_IMAGE);
    }


    // Set effect to use for showing/hiding pages.
    void SetEffects(wxShowEffect showEffect, wxShowEffect hideEffect)
    {
        m_showEffect = showEffect;
        m_hideEffect = hideEffect;
    }

    // Or the same effect for both of them.
    void SetEffect(wxShowEffect effect)
    {
        SetEffects(effect, effect);
    }

    // And the same for time outs.
    void SetEffectsTimeouts(unsigned showTimeout, unsigned hideTimeout)
    {
        m_showTimeout = showTimeout;
        m_hideTimeout = hideTimeout;
    }

    void SetEffectTimeout(unsigned timeout)
    {
        SetEffectsTimeouts(timeout, timeout);
    }


    // Implement base class pure virtual methods.

    // Page management. Every insertion funnels through the InsertPage() below; `id` is the
    // stable page name FindPageByName() resolves. Built-in tabs name a resource bitmap,
    // plugin pages hand over a ready wxBitmap; wx's own imageId overloads carry neither.
    bool AddPage(const wxString& id,
                 wxWindow* page,
                 const wxString& text,
                 const std::string& bmp_name = "",
                 bool bSelect = false)
    {
        DoInvalidateBestSize();
        return InsertPage(GetPageCount(), id, page, text, bmp_name, bSelect);
    }

    bool AddPage(wxWindow* page, const wxString& text, bool bSelect = false, int imageId = NO_IMAGE) override
    {
        DoInvalidateBestSize();
        return InsertPage(GetPageCount(), page, text, bSelect, imageId);
    }

    bool InsertPage(size_t n,
                    const wxString& id,
                    wxWindow * page,
                    const wxString & text,
                    const std::string& bmp_name = "",
                    bool bSelect = false,
                    const wxBitmap& bmp = wxNullBitmap)
    {
        n = OrderedPosition(m_pageOrder, m_pageNames, id, n);
        if (!wxBookCtrlBase::InsertPage(n, page, text, bSelect))
            return false;
        // ORCA: the base keeps the selected index, so a page inserted before the selected one would
        // leave it on the page to the left. Tabs now go before the selected one routinely, e.g. a
        // plugin tab dragged between built-in tabs.
        if (m_selection != wxNOT_FOUND && static_cast<int>(n) <= m_selection)
            ++m_selection;

        m_pageNames.insert(m_pageNames.begin() + n, id);
        GetBtnsListCtrl()->InsertPage(n, text, bSelect, bmp_name, bmp);

        // wxBookCtrlBase::InsertPage() only inserts into the page list and sizes the new
        // page to the current page's rect — it never touches visibility, and a freshly
        // constructed page defaults to shown. Without this it renders on top of whatever
        // page is currently selected until the next SetSelection() call hides it.
        if (!DoSetSelectionAfterInsertion(n, bSelect))
            page->Hide();

        return true;
    }

    virtual bool InsertPage(size_t n,
                            wxWindow * page,
                            const wxString & text,
                            bool bSelect = false,
                            int WXUNUSED(imageId) = NO_IMAGE) override
    {
        return InsertPage(n, wxString(), page, text, "", bSelect);
    }

    virtual int SetSelection(size_t n) override
    {
        int ret = DoSetSelection(n, SetSelection_SendEvent);
        int new_sel = GetSelection();
        //check the new_sel firstly
        if (new_sel != n) {
            //not allowed, skip it
            return ret;
        }
        GetBtnsListCtrl()->SetSelection(n);

        // check that only the selected page is visible and others are hidden:
        for (size_t page = 0; page < m_pages.size(); page++) {
            wxWindow* win_a = GetPage(page);
            wxWindow* win_b = GetPage(n);
            if (page != n && GetPage(page) != GetPage(n)) {
                m_pages[page]->Hide();
            }
        }

        return ret;
    }

    virtual int ChangeSelection(size_t n) override
    {
        GetBtnsListCtrl()->SetSelection(n);
        return DoSetSelection(n);
    }

    // Labels are stored by the custom button list; wx's image-list API is unused — tab icons
    // are set directly on the buttons, either from a resource name or a ready wxBitmap.
    virtual bool SetPageText(size_t n, const wxString & strText) override
    {
        wxCHECK_MSG(n < GetPageCount(), false, wxS("Invalid page"));

        GetBtnsListCtrl()->SetPageText(n, strText);

        return true;
    }

    virtual wxString GetPageText(size_t n) const override
    {
        wxCHECK_MSG(n < GetPageCount(), wxString(), wxS("Invalid page"));
        return GetBtnsListCtrl()->GetPageText(n);
    }

    // ORCA: the real page label. GetPageText() returns the button label, which SetCompact() blanks.
    wxString GetPageLabel(size_t n) const
    {
        wxCHECK_MSG(n < GetPageCount(), wxString(), wxS("Invalid page"));
        return GetBtnsListCtrl()->GetPageLabel(n);
    }

    // Resource icon name the page was inserted with; empty for pages added with a wxBitmap.
    std::string GetPageIcon(size_t n) const
    {
        wxCHECK_MSG(n < GetPageCount(), std::string(), wxS("Invalid page"));
        return GetBtnsListCtrl()->GetPageIcon(n);
    }

    virtual bool SetPageImage(size_t WXUNUSED(n), int WXUNUSED(imageId)) override
    {
        return false;
    }

    virtual int GetPageImage(size_t WXUNUSED(n)) const override
    {
        return NO_IMAGE;
    }

    bool SetPageImage(size_t n, const std::string& bmp_name)
    {
        return GetBtnsListCtrl()->SetPageImage(n, bmp_name);
    }

    // Override some wxWindow methods too.
    virtual void SetFocus() override
    {
        wxWindow* const page = GetCurrentPage();
        if (page)
            page->SetFocus();
    }

    // The base clears its page list directly instead of calling DoRemovePage() per page,
    // which would leave m_pageNames behind. No caller today; kept in sync regardless.
    virtual bool DeleteAllPages() override
    {
        m_pageNames.clear();
        return wxBookCtrlBase::DeleteAllPages();
    }

    ButtonsListCtrl* GetBtnsListCtrl() const { return static_cast<ButtonsListCtrl*>(m_bookctrl); }
    void SetOverflowButton(wxWindow* button) { GetBtnsListCtrl()->SetOverflowButton(button); }

    // Insertion index just past the first of `ids` that is present, or the end of the bar
    // if none is — lets call sites state tab order as "after X" instead of re-deriving it.
    size_t PositionAfter(std::initializer_list<const char*> ids) const
    {
        for (const char* id : ids)
            if (const int idx = FindPageByName(id); idx != wxNOT_FOUND)
                return static_cast<size_t>(idx) + 1;
        return GetPageCount();
    }

    int FindPageByName(const wxString& id) const
    {
        if (id.empty())
            return wxNOT_FOUND;
        for (size_t i = 0; i < m_pageNames.size(); ++i)
            if (m_pageNames[i] == id)
                return static_cast<int>(i);
        return wxNOT_FOUND;
    }

    wxWindow* GetPageByName(const wxString& id) const
    {
        const int idx = FindPageByName(id);
        return idx == wxNOT_FOUND ? nullptr : GetPage(static_cast<size_t>(idx));
    }

    bool SelectPageByName(const wxString& id)
    {
        const int idx = FindPageByName(id);
        if (idx == wxNOT_FOUND)
            return false;
        SetSelection(static_cast<size_t>(idx));
        return true;
    }

    // Inverse of FindPageByName: index -> id. Empty string for an out-of-range
    // index or a page that was never given an id (e.g. settings Tab pages).
    wxString GetPageName(size_t n) const
    {
        return n < m_pageNames.size() ? m_pageNames[n] : wxString();
    }

    // ORCA: user-chosen tab order, as page ids. Pages listed here are inserted at their place in it.
    void SetPageOrder(std::vector<wxString> order) { m_pageOrder = std::move(order); }
    const std::vector<wxString>& GetPageOrder() const { return m_pageOrder; }
    // Position of `id` in the page order, or SIZE_MAX when it has none. Pages without one (a newly
    // installed plugin) go where they are inserted, which for plugin pages is the end of the bar.
    size_t GetPageRank(const wxString& id) const { return PageRank(m_pageOrder, id); }
    static size_t PageRank(const std::vector<wxString>& order, const wxString& id)
    {
        const auto it = std::find(order.begin(), order.end(), id);
        return id.empty() || it == order.end() ? SIZE_MAX : static_cast<size_t>(it - order.begin());
    }

    // ORCA: plugin pages are named "plugin.<plugin key>.<page name>" (PluginPages::page_tab_id()).
    static bool IsPluginPageId(const wxString& id) { return id.StartsWith("plugin."); }

    // ORCA: moves page `from` to `to`; built-in and plugin pages share one order.
    void MovePage(size_t from, size_t to);
    // ORCA: makes the page order match the tab bar. Pages not in the bar now (a plugin page behind
    // the overflow button, a printer-specific tab) keep their place before the page they preceded,
    // or go last when nothing followed them.
    void RememberPageOrder() { m_pageOrder = MergePageOrder(m_pageOrder, m_pageNames); }
    static std::vector<wxString> MergePageOrder(const std::vector<wxString>& order, const std::vector<wxString>& page_names);
    // Where page `id`, asked for at `n` among `page_names`, goes so the pages keep `order`.
    static size_t OrderedPosition(const std::vector<wxString>& order, const std::vector<wxString>& page_names,
                                  const wxString& id, size_t n);

    wxString GetSelectedPageName() const
    {
        const int sel = GetSelection();
        return sel < 0 ? wxString() : GetPageName(static_cast<size_t>(sel));
    }

    void UpdateMode()
    {
        GetBtnsListCtrl()->UpdateMode();
    }

    void Rescale()
    {
        GetBtnsListCtrl()->Rescale();
    }

    void OnNavigationKey(wxNavigationKeyEvent& event)
    {
        if (event.IsWindowChange()) {
            // change pages
            AdvanceSelection(event.GetDirection());
        }
        else {
            // we get this event in 3 cases
            //
            // a) one of our pages might have generated it because the user TABbed
            // out from it in which case we should propagate the event upwards and
            // our parent will take care of setting the focus to prev/next sibling
            //
            // or
            //
            // b) the parent panel wants to give the focus to us so that we
            // forward it to our selected page. We can't deal with this in
            // OnSetFocus() because we don't know which direction the focus came
            // from in this case and so can't choose between setting the focus to
            // first or last panel child
            //
            // or
            //
            // c) we ourselves (see MSWTranslateMessage) generated the event
            //
            wxWindow* const parent = GetParent();

            // the wxObject* casts are required to avoid MinGW GCC 2.95.3 ICE
            const bool isFromParent = event.GetEventObject() == (wxObject*)parent;
            const bool isFromSelf = event.GetEventObject() == (wxObject*)this;
            const bool isForward = event.GetDirection();

            if (isFromSelf && !isForward)
            {
                // focus is currently on notebook tab and should leave
                // it backwards (Shift-TAB)
                event.SetCurrentFocus(this);
                parent->HandleWindowEvent(event);
            }
            else if (isFromParent || isFromSelf)
            {
                // no, it doesn't come from child, case (b) or (c): forward to a
                // page but only if entering notebook page (i.e. direction is
                // backwards (Shift-TAB) comething from out-of-notebook, or
                // direction is forward (TAB) from ourselves),
                if (m_selection != wxNOT_FOUND &&
                    (!event.GetDirection() || isFromSelf))
                {
                    // so that the page knows that the event comes from it's parent
                    // and is being propagated downwards
                    event.SetEventObject(this);

                    wxWindow* page = m_pages[m_selection];
                    if (!page->HandleWindowEvent(event))
                    {
                        page->SetFocus();
                    }
                    //else: page manages focus inside it itself
                }
                else // otherwise set the focus to the notebook itself
                {
                    SetFocus();
                }
            }
            else
            {
                // it comes from our child, case (a), pass to the parent, but only
                // if the direction is forwards. Otherwise set the focus to the
                // notebook itself. The notebook is always the 'first' control of a
                // page.
                if (!isForward)
                {
                    SetFocus();
                }
                else if (parent)
                {
                    event.SetCurrentFocus(this);
                    parent->HandleWindowEvent(event);
                }
            }
        }
    }

protected:
    virtual void UpdateSelectedPage(size_t WXUNUSED(newsel)) override
    {
        // Nothing to do here, but must be overridden to avoid the assert in
        // the base class version.
    }

    virtual wxBookCtrlEvent * CreatePageChangingEvent() const override
    {
        return new wxBookCtrlEvent(wxEVT_BOOKCTRL_PAGE_CHANGING,
                                   GetId());
    }

    virtual void MakeChangedEvent(wxBookCtrlEvent & event) override
    {
        event.SetEventType(wxEVT_BOOKCTRL_PAGE_CHANGED);
    }

    virtual wxWindow * DoRemovePage(size_t page) override
    {
        wxWindow* const win = wxBookCtrlBase::DoRemovePage(page);
        if (win)
        {
            m_pageNames.erase(m_pageNames.begin() + page);
            GetBtnsListCtrl()->RemovePage(page);
            DoSetSelectionAfterRemoval(page);
        }

        return win;
    }

    virtual void DoSize() override
    {
        wxWindow* const page = GetCurrentPage();
        if (page)
            page->SetSize(GetPageRect());
    }

    virtual int DoSetSelection(size_t n, int flags = 0) override
    {
        m_target_page = GetPage(n);
        const int ret = wxBookCtrlBase::DoSetSelection(n, flags);
        m_target_page = nullptr;
        return ret;
    }

    virtual void DoShowPage(wxWindow * page, bool show) override
    {
        if (show)
            page->ShowWithEffect(m_showEffect, m_showTimeout);
        // Tabs may share one page window (Prepare/Preview -> m_plater). Hiding it just to re-show it
        // unmaps and remaps its GL canvases, which triggers a mutter subsurface bug on Wayland.
        else if (page != m_target_page)
            page->HideWithEffect(m_hideEffect, m_hideTimeout);
    }

private:
    void Init();

    wxWindow* m_target_page{nullptr}; // page being selected; DoShowPage must not hide it
    std::vector<wxString> m_pageNames; // index-parallel to wxBookCtrlBase::m_pages
    std::vector<wxString> m_pageOrder; // ORCA

    wxShowEffect m_showEffect,
                 m_hideEffect;

    unsigned m_showTimeout,
             m_hideTimeout;
};
//#endif // _WIN32
#endif // slic3r_Notebook_hpp_
