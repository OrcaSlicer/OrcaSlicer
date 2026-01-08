///////////////////////////////////////////////////////////////////////////
// C++ code generated with wxFormBuilder (version 3.10.0-4761b0c)
// http://www.wxformbuilder.org/
//
// PLEASE DO *NOT* EDIT THIS FILE!
///////////////////////////////////////////////////////////////////////////
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "ParamsPanel.hpp"
#include "Tab.hpp"
#include "format.hpp"
#include "MainFrame.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"

#include "Widgets/Label.hpp"
#include "Widgets/SwitchButton.hpp"
#include "Widgets/Button.hpp"
#include "GUI_Factories.hpp"


namespace Slic3r {
namespace GUI {


TipsDialog::TipsDialog(wxWindow *parent, const wxString &title, const wxString &description, std::string app_key, long style,std::map<wxStandardID,wxString> option_map)
    : DPIDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX),
    m_app_key(app_key)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);

    m_top_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_top_line->SetBackgroundColour(wxColour(166, 169, 170));

    m_sizer_main->Add(m_top_line, 0, wxEXPAND, 0);

    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(20));

    m_msg = new wxStaticText(this, wxID_ANY, description, wxDefaultPosition, wxDefaultSize, 0);
    m_msg->Wrap(-1);
    m_msg->SetFont(::Label::Body_13);
    m_msg->SetForegroundColour(wxColour(107, 107, 107));
    m_msg->SetBackgroundColour(wxColour(255, 255, 255));

    m_sizer_main->Add(m_msg, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(40));

    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(5));

    wxBoxSizer *m_sizer_bottom = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *m_sizer_left   = new wxBoxSizer(wxHORIZONTAL);

    auto dont_show_again = create_item_checkbox(_L("Don't show again"), this, _L("Don't show again"), "do_not_show_tips");
    m_sizer_left->Add(dont_show_again, 1, wxALL, FromDIP(5));

    m_sizer_bottom->Add(m_sizer_left, 1, wxEXPAND, FromDIP(5));

    wxBoxSizer *m_sizer_right = new wxBoxSizer(wxHORIZONTAL);

    if (style & wxOK) {
        wxString str = _L("OK");
        if (auto iter = option_map.find(wxID_OK); iter != option_map.end())
            str = iter->second;
        Button* btn = add_button(wxID_OK, str, true);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }
    if (style & wxYES) {
        wxString str = _L("Yes");
        if (auto iter = option_map.find(wxID_YES); iter != option_map.end())
            str = iter->second;
        Button *btn = add_button(wxID_YES, str, true);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }
    if (style & wxNO) {
        wxString str = _L("No");
        if (auto iter = option_map.find(wxID_NO); iter != option_map.end())
            str = iter->second;
        Button *btn = add_button(wxID_NO, str, false);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }
    if (style & wxCANCEL) {
        wxString str = _L("Cancel");
        if (auto iter = option_map.find(wxID_CANCEL); iter != option_map.end())
            str = iter->second;
        Button *btn = add_button(wxID_CANCEL, str, false);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }

    m_sizer_bottom->Add(m_sizer_right, 0, wxEXPAND, FromDIP(5));
    m_sizer_main->Add(m_sizer_bottom, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(40));
    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(20));

    SetSizer(m_sizer_main);
    Layout();
    Fit();
    Centre(wxBOTH);

    wxGetApp().UpdateDlgDarkUI(this);
}

wxBoxSizer *TipsDialog::create_item_checkbox(wxString title, wxWindow *parent, wxString tooltip, std::string param)
{
    wxBoxSizer *m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 5);

    auto checkbox = new ::CheckBox(parent);
    m_sizer_checkbox->Add(checkbox, 0, wxALIGN_CENTER, 0);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 8);

    auto checkbox_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxSize(-1, -1), 0);
    checkbox_title->SetForegroundColour(wxColour(144, 144, 144));
    checkbox_title->SetFont(::Label::Body_13);
    checkbox_title->Wrap(-1);
    m_sizer_checkbox->Add(checkbox_title, 0, wxALIGN_CENTER | wxALL, 3);

    m_show_again = wxGetApp().app_config->has(param);
    checkbox->SetValue(m_show_again);

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox, param](wxCommandEvent &e) {
        m_show_again = m_show_again ? false : true;
        e.Skip();
    });

    return m_sizer_checkbox;
}

Button *TipsDialog::add_button(wxWindowID btn_id, const wxString &label, bool set_focus /*= false*/)
{
    Button* btn = new Button(this, label, "", 0, 0, btn_id);

    if (btn_id == wxID_OK || btn_id == wxID_YES)
        btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);

    if (btn_id == wxID_CANCEL || btn_id == wxID_NO)
        btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

    if (set_focus)
        btn->SetFocus();

    btn->Bind(wxEVT_BUTTON, [this, btn_id](wxCommandEvent &) {
        if (m_show_again) {
            if (!m_app_key.empty()) {
                if (btn_id == wxID_OK || btn_id == wxID_YES) {
                    wxGetApp().app_config->set_bool(m_app_key, true);
                }

                if (btn_id == wxID_NO) {
                    wxGetApp().app_config->set_bool(m_app_key, false);
                }
            }
        }
        EndModal(btn_id);
    });
    return btn;
}

void TipsDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    if (m_confirm) m_confirm->Rescale(); // ORCA
    Fit();
    Refresh();
}

void ParamsPanel::Highlighter::set_timer_owner(wxEvtHandler *owner, int timerid /* = wxID_ANY*/)
{
    m_timer.SetOwner(owner, timerid);
}

void ParamsPanel::Highlighter::init(std::pair<wxWindow *, bool *> params, wxWindow *parent)
    {
    if (m_timer.IsRunning()) invalidate();
    if (!params.first || !params.second) return;

    m_timer.Start(300, false);

    m_bitmap         = params.first;
    m_show_blink_ptr = params.second;
    m_parent         = parent;

    *m_show_blink_ptr = true;
    }

void ParamsPanel::Highlighter::invalidate()
{
    m_timer.Stop();

    if (m_bitmap && m_show_blink_ptr) {
        *m_show_blink_ptr = false;
        m_bitmap->Show(*m_show_blink_ptr);
        if (m_parent) {
            m_parent->Layout();
            m_parent->Refresh();
        }
        m_show_blink_ptr = nullptr;
        m_bitmap         = nullptr;
        m_parent         = nullptr;
    }

    m_blink_counter = 0;
}

void ParamsPanel::Highlighter::blink()
{
    if (m_bitmap && m_show_blink_ptr) {
        *m_show_blink_ptr = !*m_show_blink_ptr;
        m_bitmap->Show(*m_show_blink_ptr);
        if (m_parent) {
            m_parent->Layout();
            m_parent->Refresh();
        }
    } else
        return;

    if ((++m_blink_counter) == 11) invalidate();
}

ParamsPanel::ParamsPanel( wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name )
    : wxPanel( parent, id, pos, size, style, name )
{
    // BBS: new layout
    SetBackgroundColour(*wxWHITE);
#if __WXOSX__
    m_top_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_top_sizer->SetSizeHints(this);
    this->SetSizer(m_top_sizer);

    // Create additional panel to Fit() it from OnActivate()
    // It's needed for tooltip showing on OSX
    m_tmp_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    auto  sizer = new wxBoxSizer(wxHORIZONTAL);
    m_tmp_panel->SetSizer(sizer);
    m_tmp_panel->Layout();

#else
    ParamsPanel*panel = this;
    m_top_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_top_sizer->SetSizeHints(panel);
    panel->SetSizer(m_top_sizer);
#endif //__WXOSX__

    if (dynamic_cast<Notebook*>(parent)) {
        // BBS: new layout
        m_top_panel = new StaticBox(this, wxID_ANY, wxDefaultPosition);
        m_top_panel->SetBackgroundColor(0xF8F8F8);
        m_top_panel->SetBackgroundColor2(0xF1F1F1);

        m_process_icon = new ScalableButton(m_top_panel, wxID_ANY, "process");

        m_title_label = new Label(m_top_panel, _L("Process"));

        //int width, height;
        // BBS: new layout
        m_mode_region = new SwitchButton(m_top_panel);
        m_mode_region->SetMaxSize({em_unit(this) * 12, -1});
        m_mode_region->SetLabels(_L("Global"), _L("Objects"));
        //m_mode_region->GetSize(&width, &height);
        m_tips_arrow = new ScalableButton(m_top_panel, wxID_ANY, "tips_arrow");
        m_tips_arrow->Hide();

        m_mode_icon = new ScalableButton(m_top_panel, wxID_ANY, "advanced"); // ORCA
        m_mode_icon->Bind(wxEVT_BUTTON, [this](wxCommandEvent e) {
            m_mode_view->SetValue(!m_mode_view->GetValue());
            wxCommandEvent evt(wxEVT_TOGGLEBUTTON, m_mode_view->GetId()); // ParamsPanel::OnToggled(evt)
            evt.SetEventObject(m_mode_view);
            m_mode_view->wxEvtHandler::ProcessEvent(evt);
        });
        m_mode_icon->SetToolTip(_L("Show/Hide advanced parameters"));
        m_mode_view = new SwitchButton(m_top_panel, wxID_ABOUT);
        m_mode_view->SetToolTip(_L("Show/Hide advanced parameters"));

        // BBS: new layout
        //m_search_btn = new ScalableButton(m_top_panel, wxID_ANY, "search", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
        //m_search_btn->SetToolTip(format_wxstr(_L("Search in settings [%1%]"), _L("Ctrl+") + "F");
        //m_search_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().plater()->search(false); });

        m_compare_btn = new ScalableButton(m_top_panel, wxID_ANY, "compare", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
        m_compare_btn->SetToolTip(_L("Compare presets"));
        m_compare_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { wxGetApp().mainframe->diff_dialog.show(); }));

        m_setting_btn = new ScalableButton(m_top_panel, wxID_ANY, "table", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
        m_setting_btn->SetToolTip(_L("View all object's settings"));
        m_setting_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().plater()->PopupObjectTable(-1, -1, {0, 0}); });

        m_highlighter.set_timer_owner(this, 0);
        this->Bind(wxEVT_TIMER, [this](wxTimerEvent &)
        {
            m_highlighter.blink();
        });
    }



    //m_export_to_file = new Button( this, _L("Export To File"), "");
    //m_import_from_file = new Button( this, _L("Import From File") );

    // Initialize the page.
#if __WXOSX__
    auto page_parent = m_tmp_panel;
#else
    auto page_parent = this;
#endif

    // BBS: fix scroll to tip view
    class PageScrolledWindow : public wxScrolledWindow
    {
    public:
        PageScrolledWindow(wxWindow *parent)
            : wxScrolledWindow(parent,
                               wxID_ANY,
                               wxDefaultPosition,
                               wxDefaultSize,
                               wxVSCROLL) // hide hori-bar will cause hidden field mis-position
        {
            // ShowScrollBar(GetHandle(), SB_BOTH, FALSE);
            Bind(wxEVT_SCROLL_CHANGED, [this](auto &e) {
                wxWindow *child = dynamic_cast<wxWindow *>(e.GetEventObject());
                if (child != this)
                    EnsureVisible(child);
            });
        }
        virtual bool ShouldScrollToChildOnFocus(wxWindow *child)
        {
            EnsureVisible(child);
            return false;
        }
        void EnsureVisible(wxWindow* win)
        {
            const wxRect viewRect(m_targetWindow->GetClientRect());
            const wxRect winRect(m_targetWindow->ScreenToClient(win->GetScreenPosition()), win->GetSize());
            if (viewRect.Contains(winRect)) {
                return;
            }
            if (winRect.GetWidth() > viewRect.GetWidth() || winRect.GetHeight() > viewRect.GetHeight()) {
                return;
            }
            int stepx, stepy;
            GetScrollPixelsPerUnit(&stepx, &stepy);

            int startx, starty;
            GetViewStart(&startx, &starty);
            // first in vertical direction:
            if (stepy > 0) {
                int diff = 0;

                if (winRect.GetTop() < 0) {
                    diff = winRect.GetTop();
                } else if (winRect.GetBottom() > viewRect.GetHeight()) {
                    diff = winRect.GetBottom() - viewRect.GetHeight() + 1;
                    // round up to next scroll step if we can't get exact position,
                    // so that the window is fully visible:
                    diff += stepy - 1;
                }
                starty = (starty * stepy + diff) / stepy;
            }
            // then horizontal:
            if (stepx > 0) {
                int diff = 0;
                if (winRect.GetLeft() < 0) {
                    diff = winRect.GetLeft();
                } else if (winRect.GetRight() > viewRect.GetWidth()) {
                    diff = winRect.GetRight() - viewRect.GetWidth() + 1;
                    // round up to next scroll step if we can't get exact position,
                    // so that the window is fully visible:
                    diff += stepx - 1;
                }
                startx = (startx * stepx + diff) / stepx;
            }
            Scroll(startx, starty);
        }
    };

    m_page_view = new PageScrolledWindow(page_parent);
    m_page_view->SetBackgroundColour(*wxWHITE);
    m_page_sizer = new wxBoxSizer(wxVERTICAL);

    m_page_view->SetSizer(m_page_sizer);
    m_page_view->SetScrollbars(1, 20, 1, 2);
    //m_page_view->SetScrollRate( 5, 5 );

    if (m_mode_region)
        m_mode_region->Bind(wxEVT_TOGGLEBUTTON, &ParamsPanel::OnToggled, this);
    if (m_mode_view)
        m_mode_view->Bind(wxEVT_TOGGLEBUTTON, &ParamsPanel::OnToggled, this);
    Bind(wxEVT_TOGGLEBUTTON, &ParamsPanel::OnToggled, this); // For Tab's mode switch
    //Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().plater()->search(false); }, wxID_FIND);
    //m_export_to_file->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().mainframe->export_config(); });
    //m_import_from_file->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().mainframe->load_config_file(); });
}

void ParamsPanel::create_layout()
{
#ifdef __WINDOWS__
    this->SetDoubleBuffered(true);
    m_page_view->SetDoubleBuffered(true);
#endif //__WINDOWS__

    m_left_sizer = new wxBoxSizer( wxVERTICAL );
    // BBS: new layout
    m_left_sizer->SetMinSize( wxSize(40 * em_unit(this), -1 ) );

    if (m_top_panel) {
        m_mode_sizer = new wxBoxSizer( wxHORIZONTAL );
        m_mode_sizer->Add(m_process_icon, 0, wxALIGN_CENTER | wxLEFT , FromDIP(SidebarProps::TitlebarMargin()));
        m_mode_sizer->Add(m_title_label , 0, wxALIGN_CENTER | wxLEFT , FromDIP(SidebarProps::ElementSpacing()));
        m_mode_sizer->Add(m_mode_region , 0, wxALIGN_CENTER | wxLEFT , FromDIP(SidebarProps::WideSpacing()));
        m_mode_sizer->Add(m_tips_arrow  , 0, wxALIGN_CENTER | wxLEFT , FromDIP(SidebarProps::ElementSpacing()));
        m_mode_sizer->AddSpacer(FromDIP(SidebarProps::IconSpacing())); // ensure there is spacing after control when sidebar has less width
        m_mode_sizer->AddStretchSpacer();
        m_mode_sizer->Add(m_mode_icon   , 0, wxALIGN_CENTER | wxRIGHT, FromDIP(SidebarProps::ElementSpacing()));
        m_mode_sizer->Add(m_mode_view   , 0, wxALIGN_CENTER | wxRIGHT, FromDIP(SidebarProps::WideSpacing()));
        m_mode_sizer->Add(m_setting_btn , 0, wxALIGN_CENTER | wxRIGHT, FromDIP(SidebarProps::WideSpacing()));
        m_mode_sizer->Add(m_compare_btn , 0, wxALIGN_CENTER | wxRIGHT, FromDIP(SidebarProps::TitlebarMargin()));
        //m_mode_sizer->Add( m_search_btn, 0, wxALIGN_CENTER );
        //m_mode_sizer->AddSpacer(16);
        m_mode_sizer->SetMinSize(-1, FromDIP(30));
        m_top_panel->SetSizer(m_mode_sizer);
        //m_left_sizer->Add( m_top_panel, 0, wxEXPAND );
    }

    if (m_tab_print) {
        //m_print_sizer = new wxBoxSizer( wxHORIZONTAL );
        //m_print_sizer->Add( m_tab_print, 1, wxEXPAND | wxALL, 5 );
        //m_left_sizer->Add( m_print_sizer, 1, wxEXPAND, 5 );
        m_left_sizer->Add( m_tab_print, 0, wxEXPAND );
    }

    if (m_tab_print_plate) {
        m_left_sizer->Add(m_tab_print_plate, 0, wxEXPAND);
    }

    if (m_tab_print_object) {
        m_left_sizer->Add( m_tab_print_object, 0, wxEXPAND );
    }

    if (m_tab_print_part) {
        m_left_sizer->Add( m_tab_print_part, 0, wxEXPAND );
    }

    if (m_tab_print_layer) {
        m_left_sizer->Add(m_tab_print_layer, 0, wxEXPAND);
    }

    if (m_tab_filament) {
        //m_filament_sizer = new wxBoxSizer( wxVERTICAL );
        //m_filament_sizer->Add( m_tab_filament, 1, wxEXPAND | wxALL, 5 );
       // m_left_sizer->Add( m_filament_sizer, 1, wxEXPAND, 5 );
        m_left_sizer->Add( m_tab_filament, 0, wxEXPAND );
    }

    if (m_tab_printer) {
        //m_printer_sizer = new wxBoxSizer( wxVERTICAL );
        //m_printer_sizer->Add( m_tab_printer, 1, wxEXPAND | wxALL, 5 );
        m_left_sizer->Add( m_tab_printer, 0, wxEXPAND );
    }

    //m_left_sizer->Add( m_printer_sizer, 1, wxEXPAND, 1 );

    //m_button_sizer = new wxBoxSizer( wxHORIZONTAL );

    //m_button_sizer->Add( m_export_to_file, 0, wxALL, 5 );

    //m_button_sizer->Add( m_import_from_file, 0, wxALL, 5 );

    //m_left_sizer->Add( m_button_sizer, 0, wxALIGN_CENTER, 5 );

    m_top_sizer->Add(m_left_sizer, 1, wxEXPAND);

    //m_right_sizer = new wxBoxSizer( wxVERTICAL );

    //m_right_sizer->Add( m_page_view, 1, wxEXPAND | wxALL, 5 );

    //m_top_sizer->Add( m_right_sizer, 1, wxEXPAND, 5 );
    // BBS: new layout
    m_left_sizer->AddSpacer(6 * em_unit(this) / 10);
#if __WXOSX__
    m_left_sizer->Add(m_tmp_panel, 1, wxEXPAND | wxALL, 0);
    m_tmp_panel->GetSizer()->Add( m_page_view, 1, wxEXPAND );
#else
    m_left_sizer->Add( m_page_view, 1, wxEXPAND );
#endif

    //this->SetSizer( m_top_sizer );
    this->Layout();
}

void ParamsPanel::rebuild_panels()
{
    refresh_tabs();
    free_sizers();
    create_layout();
}

void ParamsPanel::refresh_tabs()
{
    auto& tabs_list = wxGetApp().tabs_list;
    auto print_tech = wxGetApp().preset_bundle->printers.get_selected_preset().printer_technology();
    for (auto tab : tabs_list)
        if (tab->supports_printer_technology(print_tech))
        {
            if (tab->GetParent() != this) continue;
            switch (tab->type())
            {
                case Preset::TYPE_PRINT:
                case Preset::TYPE_SLA_PRINT:
                    m_tab_print = tab;
                    break;

                case Preset::TYPE_FILAMENT:
                case Preset::TYPE_SLA_MATERIAL:
                    m_tab_filament = tab;
                    break;

                case Preset::TYPE_PRINTER:
                    m_tab_printer = tab;
                    break;
                default:
                    break;
            }
        }
    if (m_top_panel) {
        m_tab_print_plate = wxGetApp().get_plate_tab();
        m_tab_print_object = wxGetApp().get_model_tab();
        m_tab_print_part = wxGetApp().get_model_tab(true);
        m_tab_print_layer = wxGetApp().get_layer_tab();
    }
    return;
}

void ParamsPanel::clear_page()
{
    if (m_page_sizer)
        m_page_sizer->Clear(true);
}


void ParamsPanel::OnActivate()
{
    if (m_current_tab == NULL)
    {
        //the first time
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": first time opened, set current tab to print");
        // BBS: open/close tab
        //m_current_tab = m_tab_print;
        set_active_tab(m_tab_print ? m_tab_print : m_tab_filament);
    }
    Tab* cur_tab = dynamic_cast<Tab *> (m_current_tab);
    if (cur_tab)
        cur_tab->OnActivate();
}

void ParamsPanel::OnToggled(wxCommandEvent& event)
{
    if (m_mode_region && m_mode_region->GetId() == event.GetId()) {
        wxWindowUpdateLocker locker(GetParent());
        set_active_tab(nullptr);
        event.Skip();
        return;
    }

    if (wxID_ABOUT != event.GetId()) {
        return;
    }

    // this is from tab's mode switch
    bool value = dynamic_cast<SwitchButton*>(event.GetEventObject())->GetValue();
    int mode_id;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": Advanced mode toogle to %1%") % value;

    if (value)
    {
        //m_mode_region->SetBitmap(m_toggle_on_icon);
        mode_id = comAdvanced;
    }
    else
    {
        //m_mode_region->SetBitmap(m_toggle_off_icon);
        mode_id = comSimple;
    }

    Slic3r::GUI::wxGetApp().save_mode(mode_id);
    event.Skip();
}

// This is special, DO NOT call it from outer except from Tab
void ParamsPanel::set_active_tab(wxPanel* tab)
{
    Tab* cur_tab = dynamic_cast<Tab *> (tab);

    if (cur_tab == nullptr) {
        if (!m_mode_region->GetValue()) {
            cur_tab = (Tab*) m_tab_print;
        } else if (m_tab_print_part && ((TabPrintModel*) m_tab_print_part)->has_model_config()) {
            cur_tab = (Tab*) m_tab_print_part;
        } else if (m_tab_print_layer && ((TabPrintModel*)m_tab_print_layer)->has_model_config()) {
            cur_tab = (Tab*)m_tab_print_layer;
        } else if (m_tab_print_object && ((TabPrintModel*) m_tab_print_object)->has_model_config()) {
            cur_tab = (Tab*) m_tab_print_object;
        } else if (m_tab_print_plate && ((TabPrintPlate*)m_tab_print_plate)->has_model_config()) {
            cur_tab = (Tab*)m_tab_print_plate;
        }
        Show(cur_tab != nullptr);
        wxGetApp().sidebar().show_object_list(m_mode_region->GetValue());
        if (m_current_tab == cur_tab)
            return;
        if (cur_tab)
            cur_tab->restore_last_select_item();
        return;
    }

    m_current_tab = tab;
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": set current to %1%, type=%2%") % cur_tab % cur_tab?cur_tab->type():-1;
    update_mode();

    // BBS: open/close tab
    for (auto t : std::vector<std::pair<wxPanel*, wxStaticLine*>>({
            {m_tab_print, m_staticline_print},
            {m_tab_print_object, m_staticline_print_object},
            {m_tab_print_part, m_staticline_print_part},
            {m_tab_print_layer, nullptr},
            {m_tab_print_plate, nullptr},
            {m_tab_filament, m_staticline_filament},
            {m_tab_printer, m_staticline_printer}})) {
        if (!t.first) continue;
        t.first->Show(tab == t.first);
        if (!t.second) continue;
        t.second->Show(tab == t.first);
        //m_left_sizer->GetItem(t)->SetProportion(tab == t ? 1 : 0);
    }
    m_left_sizer->Layout();
    if (auto dialog = dynamic_cast<wxDialog*>(GetParent())) {
        wxString title = cur_tab->type() == Preset::TYPE_FILAMENT ? _L("Material settings") : _L("Printer settings");
        dialog->SetTitle(title);
    }

    auto tab_print = dynamic_cast<Tab *>(m_tab_print);
    if (cur_tab == m_tab_print) {
        if (tab_print)
            tab_print->toggle_line("print_flow_ratio", false);
    } else {
        if (tab_print)
            tab_print->toggle_line("print_flow_ratio", false);
    }
}

bool ParamsPanel::is_active_and_shown_tab(wxPanel* tab)
{
    if (m_current_tab == tab)
        return true;
    else
        return false;
}

void ParamsPanel::update_mode()
{
    int app_mode = Slic3r::GUI::wxGetApp().get_mode();
    SwitchButton * mode_view = m_current_tab ? dynamic_cast<Tab*>(m_current_tab)->m_mode_view : nullptr;
    if (mode_view == nullptr) mode_view = m_mode_view;
    if (mode_view == nullptr) return;

    //BBS: disable the mode tab and return directly when enable develop mode
    if (app_mode == comDevelop)
    {
        mode_view->Disable();
        return;
    }
    if (!mode_view->IsEnabled())
        mode_view->Enable();

    if (app_mode == comAdvanced)
    {
        mode_view->SetValue(true);
    }
    else
    {
        mode_view->SetValue(false);
    }
}

void ParamsPanel::msw_rescale()
{
    if (m_process_icon) m_process_icon->msw_rescale();
    if (m_setting_btn) m_setting_btn->msw_rescale();
    if (m_search_btn) m_search_btn->msw_rescale();
    if (m_compare_btn) m_compare_btn->msw_rescale();
    if (m_tips_arrow) m_tips_arrow->msw_rescale();
    m_left_sizer->SetMinSize(wxSize(40 * em_unit(this), -1));
    if (m_mode_sizer)
        m_mode_sizer->SetMinSize(-1, 3 * em_unit(this));
    if (m_mode_region)
        ((SwitchButton* )m_mode_region)->Rescale();
    if (m_mode_icon) m_mode_icon->msw_rescale();
    if (m_mode_view)
        ((SwitchButton* )m_mode_view)->Rescale();
    for (auto tab : {m_tab_print, m_tab_print_plate, m_tab_print_object, m_tab_print_part, m_tab_print_layer, m_tab_filament, m_tab_printer}) {
        if (tab) dynamic_cast<Tab*>(tab)->msw_rescale();
    }
    //((Button*)m_export_to_file)->Rescale();
    //((Button*)m_import_from_file)->Rescale();
}

void ParamsPanel::switch_to_global()
{
    m_mode_region->SetValue(false);
    set_active_tab(nullptr);
}

void ParamsPanel::switch_to_object(bool with_tips)
{
    m_mode_region->SetValue(true);
    set_active_tab(nullptr);
    if (with_tips) {
        m_highlighter.init(std::pair(m_tips_arrow, &m_tips_arror_blink), m_top_panel);
        m_highlighter.blink();
    }
}

void ParamsPanel::notify_object_config_changed()
{
    auto & model = wxGetApp().model();
    bool has_config = false;
    for (auto obj : model.objects) {
        if (!obj->config.empty()) {
            SettingsFactory::Bundle cat_options = SettingsFactory::get_bundle(&obj->config.get(), true);
            if (cat_options.size() > 0) {
                has_config = true;
                break;
            }
        }
        for (auto volume : obj->volumes) {
            if (!volume->config.empty()) {
                SettingsFactory::Bundle cat_options = SettingsFactory::get_bundle(&volume->config.get(), true);
                if (cat_options.size() > 0) {
                    has_config = true;
                    break;
                }
            }
        }
        if (has_config) break;
    }
    if (has_config == m_has_object_config) return;
    m_has_object_config = has_config;
    if (has_config)
        m_mode_region->SetTextColor2(StateColor(std::pair{0xfffffe, (int) StateColor::Checked}, std::pair{wxGetApp().get_label_clr_modified(), 0}));
    else
        m_mode_region->SetTextColor2(StateColor());
    m_mode_region->Rescale();
}

void ParamsPanel::switch_to_object_if_has_object_configs()
{
    if (m_has_object_config)
        m_mode_region->SetValue(true);
    set_active_tab(nullptr);
}

void ParamsPanel::free_sizers()
{
    if (m_top_sizer)
    {
        m_top_sizer->Clear(false);
        //m_top_sizer = nullptr;
    }

    m_left_sizer = nullptr;
    //m_right_sizer = nullptr;
    m_mode_sizer = nullptr;
    //m_print_sizer = nullptr;
    //m_filament_sizer = nullptr;
    //m_printer_sizer = nullptr;
    m_button_sizer = nullptr;
}

void ParamsPanel::delete_subwindows()
{
    if (m_title_label)
    {
        delete m_title_label;
        m_title_label = nullptr;
    }

    if (m_mode_region)
    {
        delete m_mode_region;
        m_mode_region = nullptr;
    }

    if (m_mode_view)
    {
        delete m_mode_view;
        m_mode_view = nullptr;
    }

    if (m_mode_icon) // ORCA m_title_view replacement
    {
        delete m_mode_icon;
        m_mode_icon = nullptr;
    }

    if (m_search_btn)
    {
        delete m_search_btn;
        m_search_btn = nullptr;
    }

    if (m_staticline_print)
    {
        delete m_staticline_print;
        m_staticline_print = nullptr;
    }

    if (m_staticline_print_part)
    {
        delete m_staticline_print_part;
        m_staticline_print_part = nullptr;
    }

    if (m_staticline_print_object)
    {
        delete m_staticline_print_object;
        m_staticline_print_object = nullptr;
    }

    if (m_staticline_filament)
    {
        delete m_staticline_filament;
        m_staticline_filament = nullptr;
    }

    if (m_staticline_printer)
    {
        delete m_staticline_printer;
        m_staticline_printer = nullptr;
    }

    if (m_export_to_file)
    {
        delete m_export_to_file;
        m_export_to_file = nullptr;
    }

    if (m_import_from_file)
    {
        delete m_import_from_file;
        m_import_from_file = nullptr;
    }

    if (m_page_view)
    {
        delete m_page_view;
        m_page_view = nullptr;
    }
}

ParamsPanel::~ParamsPanel()
{
#if 0
    free_sizers();
    delete m_top_sizer;

    delete_subwindows();
#endif
    // BBS: fix double destruct of OG_CustomCtrl
    Tab* cur_tab = dynamic_cast<Tab*> (m_current_tab);
    if (cur_tab)
        cur_tab->clear_pages();
}

} // GUI
} // Slic3r
