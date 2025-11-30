#include "CheckBox.hpp"

/*
Elipsize end on limited size when no wrapping
Text on left
on dark mode changed
*/

CheckBox::CheckBox(wxWindow *parent, wxString label)
    : wxPanel(parent, wxID_ANY)
    , m_on(           this, "check_on"                  , 18)
    , m_half(         this, "check_half"                , 18)
    , m_off(          this, "check_off"                 , 18)
    , m_on_disabled(  this, "check_on_disabled"         , 18)
    , m_half_disabled(this, "check_half_disabled"       , 18)
    , m_off_disabled( this, "check_off_disabled"        , 18)
    , m_on_focused(   this, "check_on_focused"          , 18) 
    , m_half_focused( this, "check_half_focused"        , 18)
    , m_off_focused(  this, "check_off_focused"         , 18)
    , m_on_hover(     this, "check_on_hovered"          , 18) 
    , m_half_hover(   this, "check_half_hovered"        , 18)
    , m_off_hover(    this, "check_off_hovered"         , 18)
    , m_on_hvrfcs(    this, "check_on_focused_hovered"  , 18) 
    , m_half_hvrfcs(  this, "check_half_focused_hovered", 18)
    , m_off_hvrfcs(   this, "check_off_focused_hovered" , 18)
    , m_font(Label::Body_14)
    , m_value(false)
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

    m_check->Bind(wxEVT_SET_FOCUS ,([this](wxFocusEvent e) {
        if(m_has_text)
            m_text_box->SetBorderColor(wxColour("#009688"));
        UpdateIcon();
        e.Skip();
    }));
    m_check->Bind(wxEVT_KILL_FOCUS,([this](wxFocusEvent e) {
        if(m_has_text)
            m_text_box->SetBorderColor(GetBackgroundColour());
        UpdateIcon();
        e.Skip(); 
    }));

    m_sizer->Add(m_check, 0, wxALIGN_CENTER_VERTICAL); // Dont add spacing otherwise hover events will break

    if(!label.IsEmpty()){
        m_has_text = true;

        m_text_box = new StaticBox(this);
        m_text_box->SetCornerRadius(0);
        m_text_box->SetBorderColor(GetBackgroundColour());
        m_text_box->SetCanFocus(false);
        m_text_box->DisableFocusFromKeyboard();

        // using wxStaticText allows wrapping without hustle but requires custom disable / enable since it has unwanted effect on text
        m_text = new wxStaticText(m_text_box, wxID_ANY, label);
        m_text->SetFont(m_font);
        m_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636"))); // disabled color "#6B6A6A"

        wxBoxSizer *label_sizer = new wxBoxSizer(wxHORIZONTAL);
        label_sizer->Add(m_text, 0, wxALL, FromDIP(5));
        m_text_box->SetSizer(label_sizer);

        m_sizer->Add(m_text_box, 0, wxALIGN_CENTER_VERTICAL); // Dont add spacing otherwise hover events will break
    }

    auto w_list = m_has_text ? std::initializer_list<wxWindow*>{m_text_box, m_text, m_check} : std::initializer_list<wxWindow*>{m_check};
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
            }else
                m_hovered = false;
            UpdateIcon();
            e.Skip();
        });
        w->Bind(wxEVT_LEFT_DOWN  ,[this](wxMouseEvent e) {
            if (!m_enabled || e.GetEventType() == wxEVT_LEFT_DCLICK) return;
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

    SetSizerAndFit(m_sizer);
    Layout();

    Refresh();
}

void CheckBox::Wrap(int width)
{
    if(!m_has_text) return;
    if(width > 0){
        if(width > m_check->GetSize().x)
            m_text->Wrap(width - m_check->GetSize().x);
        else
            m_text->Wrap(width);
    }else
        m_text->Wrap(width);

    m_sizer->Fit(this);
    m_sizer->SetSizeHints(this);
    Layout();
    Refresh();
}

void CheckBox::Rescale()
{
    auto i_list = std::vector<ScalableBitmap>{
        m_on         , m_half         , m_off,
        m_on_disabled, m_half_disabled, m_off_disabled,
        m_on_focused , m_half_focused , m_off_focused,
        m_on_hover   , m_half_hover   , m_off_hover, 
        m_on_hvrfcs  , m_half_hvrfcs  , m_off_hvrfcs
    };
    for (ScalableBitmap i : i_list)
        i.msw_rescale();
    m_check->SetSize(m_on.GetBmpSize());
    m_check->Rescale();
    m_sizer->Fit(this);
    m_sizer->SetSizeHints(this);
    UpdateIcon();
    Layout();
    Refresh();
}

void CheckBox::OnClick()
{
    m_check->SetFocus();
    SetValue(!m_value);
}

void CheckBox::SetTooltip(wxString label)
{
    m_check->SetToolTip(label);
    if(m_has_text)
        m_text->SetToolTip(label);
}

void CheckBox::UpdateIcon()
{
    ScalableBitmap icon;
    bool focus = HasFocus();
    icon = (!m_enabled         ) ? (m_half_checked ? m_half_disabled : m_value ? m_on_disabled : m_off_disabled )
         : (m_hovered && focus ) ? (m_half_checked ? m_half_hvrfcs   : m_value ? m_on_hvrfcs   : m_off_hvrfcs   )
         : (m_hovered && !focus) ? (m_half_checked ? m_half_hover    : m_value ? m_on_hover    : m_off_hover    )
         : (!m_hovered && focus) ? (m_half_checked ? m_half_focused  : m_value ? m_on_focused  : m_off_focused  ) 
         :                         (m_half_checked ? m_half          : m_value ? m_on          : m_off          );
    m_check->SetIcon(icon.name());
    m_check->Refresh();
}

wxWindow* CheckBox::GetScrollParent(wxWindow *pWindow)
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

void CheckBox::SetValue(bool value){
    if (m_value == value)
        return;
    m_value = value;
    m_half_checked = false;
    UpdateIcon();

    // temporary solution to handle both events. all events should be unify in wxEVT_CHECKBOX
    wxCommandEvent evt(wxEVT_CHECKBOX, GetId());
    evt.SetEventObject(this);
    evt.SetInt(value ? 1 : 0);
    GetEventHandler()->ProcessEvent(evt);

    evt.SetEventType(wxEVT_TOGGLEBUTTON);
    GetEventHandler()->ProcessEvent(evt);

    Refresh();
}