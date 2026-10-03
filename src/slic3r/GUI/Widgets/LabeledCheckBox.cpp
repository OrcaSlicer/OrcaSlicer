#include "LabeledCheckBox.hpp"

#include <wx/tglbtn.h> // to keep wxEVT_TOGGLEBUTTON

LabeledCheckBox::LabeledCheckBox(wxWindow *parent, wxString label)
    : wxPanel(parent, wxID_ANY)
    , m_on(           this, "check_on"            , 18)
    , m_half(         this, "check_half"          , 18)
    , m_off(          this, "check_off"           , 18)
    , m_on_disabled(  this, "check_on_disabled"   , 18)
    , m_half_disabled(this, "check_half_disabled" , 18)
    , m_off_disabled( this, "check_off_disabled"  , 18)
    , m_on_focused(   this, "check_on_focused"    , 18) 
    , m_half_focused( this, "check_half_focused"  , 18)
    , m_off_focused(  this, "check_off_focused"   , 18)
    , m_font(Label::Body_14)
    , m_label_color(wxColour("#363636"))
    , m_value(false)
    , m_wrap(-1)
{
    if (parent)
        SetBackgroundColour(parent->GetBackgroundColour());
    if (auto sParent = GetScrollParent(this))
        SetBackgroundColour(sParent->GetBackgroundColour());

    m_label = label;

    m_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_check = new Button(this, "", "check_off", 0, 18);
    m_check->SetPaddingSize(FromDIP(wxSize(0,0)));
    m_check->SetBackgroundColor(GetBackgroundColour());
    m_check->SetCornerRadius(0);
    m_check->SetBorderWidth(0);

    m_check->Bind(wxEVT_SET_FOCUS ,([this](wxFocusEvent e) {UpdateLabelBorder(true); UpdateIcon(); e.Skip();}));
    m_check->Bind(wxEVT_KILL_FOCUS,([this](wxFocusEvent e) {UpdateLabelBorder(false);UpdateIcon(); e.Skip();}));

    m_check_item = m_sizer->Add(m_check, 0, wxALIGN_CENTER_VERTICAL); // Dont add spacing otherwise hover events will break

    if(!label.IsEmpty()){
        m_has_text = true;

        m_text_box = new StaticBox(this);
        m_text_box->SetCornerRadius(0);
        m_text_box->SetBorderColor(GetBackgroundColour());
        m_text_box->SetCanFocus(false);
        m_text_box->DisableFocusFromKeyboard();

        m_text = new wxStaticText(m_text_box, wxID_ANY, label);
        m_text->SetFont(m_font);
        UpdateLabelColor(true);

        wxBoxSizer *label_sizer = new wxBoxSizer(wxHORIZONTAL);
        label_sizer->Add(m_text, 0, wxALL, FromDIP(5));
        m_text_box->SetSizer(label_sizer);

        m_text_item = m_sizer->Add(m_text_box, 0, wxALIGN_CENTER_VERTICAL); // Dont add spacing otherwise hover events will break
    }

    std::vector<wxWindow*> w_list = {m_check};
    if (m_has_text) {
        w_list.push_back(m_text_box);
        w_list.push_back(m_text);
    }
    for (wxWindow* w : w_list) {
        w->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &e) {
            m_hovered = true;
            UpdateIcon();
            e.Skip();
        });
        w->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &e) {
            if(m_has_text){
                wxWindow* next_w = wxFindWindowAtPoint(wxGetMousePosition());
                if (!next_w || !IsDescendant(next_w) || next_w == this)
                    m_hovered = false;
            }
            else
                m_hovered = false;
            UpdateIcon();
            e.Skip();
        });
        w->Bind(wxEVT_LEFT_DOWN  ,[this](wxMouseEvent e) {
            if (!m_enabled || e.LeftDClick()) return;
            OnClick();
            e.Skip();
        });
        w->Bind(wxEVT_LEFT_DCLICK,[this](wxMouseEvent e) {
            if (!m_enabled) return;
            OnClick();
            e.Skip();
        });
    };

    Bind(wxEVT_CHAR_HOOK, ([this](wxKeyEvent&e){
        if(HasFocus() && e.GetKeyCode() == WXK_SPACE)
            SetValue(!m_value);
        else
            e.Skip();
    }));

    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        if (m_has_text && m_wrap > 0) {
            int target = std::min(GetSize().x, m_max_size.x > 0 ? m_max_size.x : GetSize().x);
            ApplyWrap(target);
        }
        e.Skip();
    });

    SetSizerAndFit(m_sizer);
    Layout();

    Refresh();
}

void LabeledCheckBox::Wrap(int width)
{
    if(!m_has_text) return;
    ApplyWrap(width);
    m_sizer->Fit(this);
    m_sizer->SetSizeHints(this);
    Layout();
    Refresh();
}

void LabeledCheckBox::ApplyWrap(int width)
{
    if (!m_has_text) return;
    m_wrap = width;
    int effective = (width > 0) ? std::max(width - m_check->GetSize().x, 0) : width;
    m_text->Wrap(effective);
    UpdateAlignment();
}

void LabeledCheckBox::OnClick()
{
    m_check->SetFocus();
    SetValue(!m_value);
}

void LabeledCheckBox::SetTooltip(wxString label)
{
    m_check->SetToolTip(label);
    if(m_has_text)
        m_text->SetToolTip(label);
}

bool LabeledCheckBox::SetFont(const wxFont& font) {
    m_font = font;
    if(!m_has_text) return false;
    bool result = m_text->SetFont(font);
    if (m_wrap > 0)
        ApplyWrap(m_wrap);
    else
        UpdateAlignment();
    m_sizer->Fit(this);
    m_sizer->SetSizeHints(this);
    Layout();
    Refresh();
    return result;
};

bool LabeledCheckBox::Enable(bool enable) {
    m_enabled = enable;
    bool result = m_check->Enable(enable);
    UpdateLabelColor(enable);
    UpdateIcon();
    Refresh();
    return result;
};

bool LabeledCheckBox::HasFocus() const {
    return m_check->HasFocus();
}

void LabeledCheckBox::SetLabelColor(wxColour color) {
    if(m_has_text){
        m_label_color = color;
        UpdateLabelColor(m_enabled);
    }
}

void LabeledCheckBox::SetLabel(const wxString& label) {
    wxPanel::SetLabel(label);
    if(m_has_text){
        m_text->SetLabel(label);
        if (m_wrap > 0)
            ApplyWrap(m_wrap);
        else
            UpdateAlignment();
        // layout might be changed if wrap triggered
        m_sizer->Fit(this);
        m_sizer->SetSizeHints(this);
        Layout();
        Refresh();
    }
}

void LabeledCheckBox::SetValue(bool value){
    if (m_value == value)
        return;
    m_value = value;
    m_half_checked = false;
    UpdateIcon();

    // just in case support both event to prevent crash. wxCheckbox (wxEVT_CHECKBOX) CheckBox (wxEVT_TOGGLEBUTTON)
    for (wxEventType type : {wxEVT_CHECKBOX, wxEVT_TOGGLEBUTTON}) {
        wxCommandEvent evt(type, GetId());
        evt.SetEventObject(this);
        evt.SetInt(value ? 1 : 0);
        GetEventHandler()->ProcessEvent(evt);
    }

    Refresh();
}

void LabeledCheckBox::UpdateIcon()
{
    ScalableBitmap icon;
    bool focus = HasFocus();
    icon = (!m_enabled        ) ? (m_half_checked ? m_half_disabled : m_value ? m_on_disabled : m_off_disabled )
         : (m_hovered || focus) ? (m_half_checked ? m_half_focused  : m_value ? m_on_focused  : m_off_focused  ) 
         :                        (m_half_checked ? m_half          : m_value ? m_on          : m_off          );
    m_check->SetIcon(icon.name());
    m_check->Refresh();
}

void LabeledCheckBox::UpdateLabelColor(bool enabled) {
    if(m_has_text)  // just changes its color to prevent unwanted effect on windows
        m_text->SetForegroundColour(StateColor::darkModeColorFor(enabled ? m_label_color : wxColour("#6B6A6A")));
};

void LabeledCheckBox::UpdateLabelBorder(bool focused) {
    if(m_has_text)
        m_text_box->SetBorderColor(focused ? wxColour("#009688") : GetBackgroundColour());
};

void LabeledCheckBox::UpdateAlignment()
{
    if (!m_has_text || !m_check_item || !m_text_item)
        return;

    int line_count = m_text->GetLabel().Freq('\n') + 1;

    if (line_count > 1) { // Multi-line: align both to top
        // match the text box's internal top padding (wxALL, FromDIP(5))
        m_check_item->SetFlag(wxALIGN_TOP | wxTOP);
        m_check_item->SetBorder(FromDIP(5));
        m_text_item->SetFlag(wxALIGN_TOP);
        m_text_item->SetBorder(0);
    } 
    else { // Single line: revert to simple vertical centering
        m_check_item->SetFlag(wxALIGN_CENTER_VERTICAL);
        m_check_item->SetBorder(0);
        m_text_item->SetFlag(wxALIGN_CENTER_VERTICAL);
        
    }
    m_text_item->SetBorder(0);

    m_sizer->Layout();
}

wxWindow* LabeledCheckBox::GetScrollParent(wxWindow *pWindow)
{
    wxWindow *pWin = pWindow;
    while (pWin->GetParent()) {
        auto pWin2 = pWin->GetParent();
        if (auto top = dynamic_cast<wxScrollHelper *>(pWin2))
            return dynamic_cast<wxWindow *>(pWin);
        pWin = pWin2;
    }
    return nullptr;
}

void LabeledCheckBox::SetMaxSize(const wxSize& size)
{
    wxPanel::SetMaxSize(size);

    m_max_size = size;

    if (!m_has_text) return;

    long style = m_text->GetWindowStyleFlag();
    if (m_wrap > 0) {
        style &= ~(wxST_ELLIPSIZE_START | wxST_ELLIPSIZE_MIDDLE | wxST_ELLIPSIZE_END);
        m_text->SetWindowStyleFlag(style);
        Wrap(size.x > 0 ? size.x : -1);
    }
    else {
        style &= ~(wxST_ELLIPSIZE_START | wxST_ELLIPSIZE_MIDDLE);
        style |= (wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        m_text->SetWindowStyleFlag(style);

        if (size.x > 0) {
            int targetWidth = size.x; 
            if (m_check)
                targetWidth -= (m_check->GetSize().x);
            if (targetWidth > 0)
                m_text->SetMaxSize(wxSize(targetWidth, size.y));
        }
        m_text->SetLabel(m_text->GetLabel());
        m_sizer->Fit(this);
        m_sizer->SetSizeHints(this);
        Layout();
    }

}

void LabeledCheckBox::Rescale(){
    m_on.msw_rescale();
    m_half.msw_rescale();
    m_off.msw_rescale();
    m_on_disabled.msw_rescale();
    m_half_disabled.msw_rescale();
    m_off_disabled.msw_rescale();
    m_on_focused.msw_rescale();
    m_half_focused.msw_rescale();
    m_off_focused.msw_rescale();

    m_check->Rescale();

    if (m_wrap > 0)
        ApplyWrap(m_wrap); 

    m_sizer->Fit(this);
    m_sizer->SetSizeHints(this);
    Layout();
    Refresh();
}