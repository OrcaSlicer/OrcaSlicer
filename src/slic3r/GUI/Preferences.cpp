#include "Preferences.hpp"
#include "OptionsGroup.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include <wx/language.h>
#include "OG_CustomCtrl.hpp"
#include "wx/graphics.h"
#include <wx/listimpl.cpp>
#include <wx/display.h>
#include "NetworkTestDialog.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/RadioGroup.hpp"

#ifdef __WINDOWS__
#ifdef _MSW_DARK_MODE
#include "dark_mode.hpp"
#endif // _MSW_DARK_MODE
#endif //__WINDOWS__

namespace Slic3r { namespace GUI {

class MyscrolledWindow : public wxScrolledWindow {
public:
    MyscrolledWindow(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxVSCROLL) : wxScrolledWindow(parent, id, pos, size, style) {}

    bool ShouldScrollToChildOnFocus(wxWindow* child) override { return false; }
};

wxBoxSizer *PreferencesDialog::create_item_title(wxString title)
{
    wxBoxSizer *m_sizer_title = new wxBoxSizer(wxHORIZONTAL);

    auto title_ctrl = new StaticLine(m_parent, 0, title);
    title_ctrl->SetFont(Label::Head_14);
    title_ctrl->SetForegroundColour(DESIGN_GRAY900_COLOR);
    m_sizer_title->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN - 10));
    m_sizer_title->Add(title_ctrl, 1, wxEXPAND | wxBOTTOM | wxTOP, FromDIP(6));
    m_sizer_title->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN - 10));

    return m_sizer_title;
}

std::tuple<wxBoxSizer*, ComboBox*> PreferencesDialog::create_item_combobox_base(wxString title, wxString tooltip, std::string param, std::vector<wxString> vlist, unsigned int current_index)
{
    wxBoxSizer *m_sizer_combox = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_combox->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    auto combo_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    combo_title->SetFont(::Label::Body_14);
    combo_title->SetToolTip(tip);
    combo_title->Wrap(DESIGN_TITLE_SIZE.x);
    m_sizer_combox->Add(combo_title, 0, wxALIGN_CENTER);

    auto combobox = new ::ComboBox(m_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_LARGE_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->SetFont(::Label::Body_14);
    combobox->GetDropDown().SetFont(::Label::Body_14);

    std::vector<wxString>::iterator iter;
    for (iter = vlist.begin(); iter != vlist.end(); iter++) {
        combobox->Append(*iter);
    }

    combobox->SetSelection(current_index);

    m_sizer_combox->Add(combobox, 0, wxALIGN_CENTER | wxLEFT, FromDIP(5));

    return {m_sizer_combox, combobox};
}

wxBoxSizer* PreferencesDialog::create_item_combobox(wxString title, wxString tooltip, std::string param, std::vector<wxString> vlist, std::function<void(wxString)> onchange)
{
    unsigned int current_index = 0;

    auto current_setting = app_config->get(param);
    if (!current_setting.empty()) {
        current_index = atoi(current_setting.c_str());
    }

    auto [sizer, combobox] = create_item_combobox_base(title, tooltip, param, vlist, current_index);

    //// save config
    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this, param, onchange](wxCommandEvent& e) {
        app_config->set(param, std::to_string(e.GetSelection()));
        if (onchange)
            onchange(std::to_string(e.GetSelection()));
        e.Skip();
    });

    return sizer;
}

wxBoxSizer *PreferencesDialog::create_item_combobox(wxString title, wxString tooltip, std::string param, std::vector<wxString> vlist, std::vector<std::string> config_name_index)
{
    assert(vlist.size() == config_name_index.size());
    unsigned int current_index = 0;

    auto current_setting = app_config->get(param);
    if (!current_setting.empty()) {
        auto compare  = [current_setting](string possible_setting) { return current_setting == possible_setting; };
        auto iterator = find_if(config_name_index.begin(), config_name_index.end(), compare);
        current_index = iterator - config_name_index.begin();
    }

    auto [sizer, combobox] = create_item_combobox_base(title, tooltip, param, vlist, current_index);

    //// save config
    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this, param, config_name_index](wxCommandEvent& e) {
        app_config->set(param, config_name_index[e.GetSelection()]);
        e.Skip();
    });

    return sizer;
}

wxBoxSizer *PreferencesDialog::create_item_language_combobox(wxString title, wxString tooltip)
{
    wxLanguage supported_languages[]{
        wxLANGUAGE_ENGLISH,
        wxLANGUAGE_CHINESE_SIMPLIFIED,
        wxLANGUAGE_CHINESE,
        wxLANGUAGE_GERMAN,
        wxLANGUAGE_CZECH,
        wxLANGUAGE_FRENCH,
        wxLANGUAGE_SPANISH,
        wxLANGUAGE_SWEDISH,
        wxLANGUAGE_DUTCH,
        wxLANGUAGE_HUNGARIAN,
        wxLANGUAGE_JAPANESE,
        wxLANGUAGE_ITALIAN,
        wxLANGUAGE_KOREAN,
        wxLANGUAGE_RUSSIAN,
        wxLANGUAGE_UKRAINIAN,
        wxLANGUAGE_TURKISH,
        wxLANGUAGE_POLISH,
        wxLANGUAGE_CATALAN,
        wxLANGUAGE_PORTUGUESE_BRAZILIAN,
        wxLANGUAGE_LITHUANIAN,
        wxLANGUAGE_VIETNAMESE
    };

    auto translations = wxTranslations::Get()->GetAvailableTranslations(SLIC3R_APP_KEY);
    std::vector<const wxLanguageInfo *> language_infos;
    language_infos.emplace_back(wxLocale::GetLanguageInfo(wxLANGUAGE_ENGLISH));
    for (size_t i = 0; i < translations.GetCount(); ++i) {
        const wxLanguageInfo *langinfo = wxLocale::FindLanguageInfo(translations[i]);

        if (langinfo == nullptr) continue;
        int language_num = sizeof(supported_languages) / sizeof(supported_languages[0]);
        for (auto si = 0; si < language_num; si++) {
            if (langinfo == wxLocale::GetLanguageInfo(supported_languages[si])) {
                language_infos.emplace_back(langinfo);
            }
        }
        //if (langinfo != nullptr) language_infos.emplace_back(langinfo);
    }
    sort_remove_duplicates(language_infos);
    std::sort(language_infos.begin(), language_infos.end(), [](const wxLanguageInfo *l, const wxLanguageInfo *r) { return l->Description < r->Description; });

    auto vlist = language_infos;
    auto param = "language";

    wxBoxSizer *m_sizer_combox = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_combox->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    auto combo_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    combo_title->SetFont(::Label::Body_14);
    combo_title->SetToolTip(tip);
    combo_title->Wrap(DESIGN_TITLE_SIZE.x);
    m_sizer_combox->Add(combo_title, 0, wxALIGN_CENTER);


    auto combobox = new ::ComboBox(m_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_LARGE_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->SetFont(::Label::Body_14);
    combobox->GetDropDown().SetFont(::Label::Body_14);
    auto language = app_config->get(param);
    m_current_language_selected = -1;
    std::vector<wxString>::iterator iter;
    for (size_t i = 0; i < vlist.size(); ++i) {
        auto language_name = vlist[i]->Description;

        if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CHINESE_SIMPLIFIED)) {
            language_name = wxString::FromUTF8("\xe4\xb8\xad\xe6\x96\x87\x28\xe7\xae\x80\xe4\xbd\x93\x29");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CHINESE)) {
            language_name = wxString::FromUTF8("\xe4\xb8\xad\xe6\x96\x87\x28\xe7\xb9\x81\xe9\xab\x94\x29");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_SPANISH)) {
            language_name = wxString::FromUTF8("\x45\x73\x70\x61\xc3\xb1\x6f\x6c");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_GERMAN)) {
            language_name = wxString::FromUTF8("Deutsch");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CZECH)) {
            language_name = wxString::FromUTF8("Czech");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_SWEDISH)) {
            language_name = wxString::FromUTF8("\x53\x76\x65\x6e\x73\x6b\x61"); //Svenska
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_DUTCH)) {
            language_name = wxString::FromUTF8("Nederlands");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_FRENCH)) {
            language_name = wxString::FromUTF8("\x46\x72\x61\x6E\xC3\xA7\x61\x69\x73");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_HUNGARIAN)) {
            language_name = wxString::FromUTF8("Magyar");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_JAPANESE)) {
            language_name = wxString::FromUTF8("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_ITALIAN)) {
            language_name = wxString::FromUTF8("\x69\x74\x61\x6c\x69\x61\x6e\x6f");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_KOREAN)) {
            language_name = wxString::FromUTF8("\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_RUSSIAN)) {
            language_name = wxString::FromUTF8("\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_UKRAINIAN)) {
            language_name = wxString::FromUTF8("Ukrainian");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_TURKISH)) {
            language_name = wxString::FromUTF8("Turkish");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_POLISH)) {
            language_name = wxString::FromUTF8("Polski");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CATALAN)) {
            language_name = wxString::FromUTF8("Catalan");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_PORTUGUESE_BRAZILIAN)) {
            language_name = wxString::FromUTF8("Português (Brasil)");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_LITHUANIAN)) {
            language_name = wxString::FromUTF8("Lietuvių");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_VIETNAMESE)) {
            language_name = wxString::FromUTF8("Tiếng Việt");
        }

        if (app_config->get(param) == vlist[i]->CanonicalName) {
            m_current_language_selected = i;
        }
        combobox->Append(language_name);
    }
    if (m_current_language_selected == -1 && language.size() >= 5) {
        language = language.substr(0, 2);
        for (size_t i = 0; i < vlist.size(); ++i) {
            if (vlist[i]->CanonicalName.StartsWith(language)) {
                m_current_language_selected = i;
                break;
            }
        }
    }
    combobox->SetSelection(m_current_language_selected);

    m_sizer_combox->Add(combobox, 0, wxALIGN_CENTER | wxLEFT, FromDIP(5));

    combobox->Bind(wxEVT_LEFT_DOWN, [this, combobox](wxMouseEvent &e) {
        m_current_language_selected = combobox->GetSelection();
        e.Skip();
    });

    combobox->Bind(wxEVT_COMBOBOX, [this, param, vlist, combobox](wxCommandEvent &e) {
        if (combobox->GetSelection() == m_current_language_selected)
            return;

        if (e.GetString().mb_str() != app_config->get(param)) {
            {
                //check if the project has changed
                if (wxGetApp().plater()->is_project_dirty()) {
                    auto result = MessageDialog(static_cast<wxWindow*>(this), _L("The current project has unsaved changes, save it before continue?"),
                        wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Save"), wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE).ShowModal();

                    if (result == wxID_YES) {
                        wxGetApp().plater()->save_project();
                    }
                }


                // the dialog needs to be destroyed before the call to switch_language()
                // or sometimes the application crashes into wxDialogBase() destructor
                // so we put it into an inner scope
                MessageDialog msg_wingow(nullptr, _L("Switching the language requires application restart.\n") + "\n" + _L("Do you want to continue?"),
                                         L("Language selection"), wxICON_QUESTION | wxOK | wxCANCEL);
                if (msg_wingow.ShowModal() == wxID_CANCEL) {
                    combobox->SetSelection(m_current_language_selected);
                    return;
                }
            }

            auto check = [this](bool yes_or_no) {
                // if (yes_or_no)
                //    return true;
                int act_btns = ActionButtons::SAVE;
                return wxGetApp().check_and_keep_current_preset_changes(_L("Switching application language"),
                                                                        _L("Switching application language while some presets are modified."), act_btns);
            };

            m_current_language_selected = combobox->GetSelection();
            if (m_current_language_selected >= 0 && m_current_language_selected < vlist.size()) {
                app_config->set(param, vlist[m_current_language_selected]->CanonicalName.ToUTF8().data());

                wxGetApp().load_language(vlist[m_current_language_selected]->CanonicalName, false);
                Close();
                // Reparent(nullptr);
                GetParent()->RemoveChild(this);
                wxGetApp().recreate_GUI(_L("Changing application language"));
            }
        }

        e.Skip();
    });

    return m_sizer_combox;
}

wxBoxSizer *PreferencesDialog::create_item_region_combobox(wxString title, wxString tooltip)
{

    std::vector<wxString> Regions         = {_L("Asia-Pacific"), _L("China"), _L("Europe"), _L("North America"), _L("Others")};
    std::vector<wxString> local_regions = {"Asia-Pacific", "China", "Europe", "North America", "Others"};

    auto vlist = Regions;

    wxBoxSizer *m_sizer_combox = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_combox->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    auto combo_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    combo_title->SetFont(::Label::Body_14);
    combo_title->SetToolTip(tip);
    combo_title->Wrap(DESIGN_TITLE_SIZE.x);
    m_sizer_combox->Add(combo_title, 0, wxALIGN_CENTER);

    auto combobox = new ::ComboBox(m_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_LARGE_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->SetFont(::Label::Body_14);
    combobox->GetDropDown().SetFont(::Label::Body_14);
    m_sizer_combox->Add(combobox, 0, wxALIGN_CENTER | wxLEFT, FromDIP(5));

    std::vector<wxString>::iterator iter;
    for (iter = vlist.begin(); iter != vlist.end(); iter++) { combobox->Append(*iter); }

    AppConfig * config       = GUI::wxGetApp().app_config;

    int         current_region = 0;
    if (!config->get("region").empty()) {
        std::string country_code = config->get("region");
        for (auto i = 0; i < vlist.size(); i++) {
            if (local_regions[i].ToStdString() == country_code) {
                combobox->SetSelection(i);
                current_region = i;
            }
        }
    }

    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this, combobox, current_region, local_regions](wxCommandEvent &e) {
        auto region_index = e.GetSelection();
        auto region       = local_regions[region_index];

        /*auto area   = "";
        if (region == "CHN" || region == "China")
            area = "CN";
        else if (region == "USA")
            area = "US";
        else if (region == "Asia-Pacific")
            area = "Others";
        else if (region == "Europe")
            area = "US";
        else if (region == "North America")
            area = "US";
        else
            area = "Others";*/
        combobox->SetSelection(region_index);
        NetworkAgent* agent = wxGetApp().getAgent();
        AppConfig* config = GUI::wxGetApp().app_config;
        if (agent) {
            MessageDialog msg_wingow(this, _L("Changing the region will log out your account.\n") + "\n" + _L("Do you want to continue?"), _L("Region selection"),
                                     wxICON_QUESTION | wxOK | wxCANCEL);
            if (msg_wingow.ShowModal() == wxID_CANCEL) {
                combobox->SetSelection(current_region);
                return;
            } else {
                wxGetApp().request_user_logout();
                config->set("region", region.ToStdString());
                auto area = config->get_country_code();
                if (agent) {
                    agent->set_country_code(area);
                }
                EndModal(wxID_CANCEL);
            }
        } else {
            config->set("region", region.ToStdString());
        }

        wxGetApp().update_publish_status();
        e.Skip();
    });

    return m_sizer_combox;
}

wxBoxSizer *PreferencesDialog::create_item_loglevel_combobox(wxString title, wxString tooltip, std::vector<wxString> vlist)
{
    wxBoxSizer *m_sizer_combox = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_combox->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

    auto combo_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    combo_title->SetFont(::Label::Body_14);
    combo_title->SetToolTip(tooltip);
    combo_title->Wrap(DESIGN_TITLE_SIZE.x);
    m_sizer_combox->Add(combo_title, 0, wxALIGN_CENTER);

    auto combobox = new ::ComboBox(m_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->SetFont(::Label::Body_14);
    combobox->GetDropDown().SetFont(::Label::Body_14);

    std::vector<wxString>::iterator iter;
    for (iter = vlist.begin(); iter != vlist.end(); iter++) { combobox->Append(*iter); }

    auto severity_level = app_config->get("log_severity_level");
    if (!severity_level.empty()) { combobox->SetValue(severity_level); }

    m_sizer_combox->Add(combobox, 0, wxALIGN_CENTER | wxLEFT, FromDIP(5));

    //// save config
    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        auto level = Slic3r::get_string_logging_level(e.GetSelection());
        Slic3r::set_logging_level(Slic3r::level_string_to_boost(level));
        app_config->set("log_severity_level",level);
        e.Skip();
     });
    return m_sizer_combox;
}

wxBoxSizer *PreferencesDialog::create_item_multiple_combobox(
    wxString title, wxString tooltip, std::string param, std::vector<wxString> vlista, std::vector<wxString> vlistb)
{
    std::vector<wxString> params;
    Split(app_config->get(param), "/", params);

    std::vector<wxString>::iterator iter;

   wxBoxSizer *m_sizer_tcombox= new wxBoxSizer(wxHORIZONTAL);
   m_sizer_tcombox->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

   auto combo_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
   combo_title->SetToolTip(tooltip);
   combo_title->Wrap(DESIGN_TITLE_SIZE.x);
   combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
   combo_title->SetFont(::Label::Body_14);
   m_sizer_tcombox->Add(combo_title, 0, wxALIGN_CENTER);

   auto combobox_left = new ::ComboBox(m_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
   combobox_left->SetFont(::Label::Body_14);
   combobox_left->GetDropDown().SetFont(::Label::Body_14);


   for (iter = vlista.begin(); iter != vlista.end(); iter++) { combobox_left->Append(*iter); }
   combobox_left->SetValue(std::string(params[0].mb_str()));
   m_sizer_tcombox->Add(combobox_left, 0, wxALIGN_CENTER | wxLEFT, FromDIP(5));

   auto combo_title_add = new wxStaticText(m_parent, wxID_ANY, wxT("+"), wxDefaultPosition, wxDefaultSize, 0);
   combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
   combo_title->SetFont(::Label::Body_14);
   combo_title_add->Wrap(-1);
   m_sizer_tcombox->Add(combo_title_add, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(5));

   auto combobox_right = new ::ComboBox(m_parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
   combobox_right->SetFont(::Label::Body_14);
   combobox_right->GetDropDown().SetFont(::Label::Body_14);

   for (iter = vlistb.begin(); iter != vlistb.end(); iter++) { combobox_right->Append(*iter); }
   combobox_right->SetValue(std::string(params[1].mb_str()));
   m_sizer_tcombox->Add(combobox_right, 0, wxALIGN_CENTER | wxLEFT, FromDIP(5));

    // save config
    combobox_left->GetDropDown().Bind(wxEVT_COMBOBOX, [this, param, combobox_right](wxCommandEvent &e) {
        auto config = e.GetString() + wxString("/") + combobox_right->GetValue();
        app_config->set(param, std::string(config.mb_str()));
        e.Skip();
    });

    combobox_right->GetDropDown().Bind(wxEVT_COMBOBOX, [this, param, combobox_left](wxCommandEvent &e) {
        auto config = combobox_left->GetValue() + wxString("/") + e.GetString();
        app_config->set(param, std::string(config.mb_str()));
        e.Skip();
    });

    return m_sizer_tcombox;
}

wxBoxSizer *PreferencesDialog::create_item_input(wxString title, wxString title2, wxString tooltip, std::string param, std::function<void(wxString)> onchange)
{
    wxBoxSizer *sizer_input = new wxBoxSizer(wxHORIZONTAL);
    auto        input_title   = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    input_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    input_title->SetFont(::Label::Body_14);
    input_title->SetToolTip(tooltip);
    input_title->Wrap(DESIGN_TITLE_SIZE.x);

    auto       input = new ::TextInput(m_parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    wxTextValidator validator(wxFILTER_DIGITS);
    input->SetToolTip(tooltip);
    input->GetTextCtrl()->SetValidator(validator);

    auto second_title = new wxStaticText(m_parent, wxID_ANY, title2, wxDefaultPosition, wxDefaultSize, 0);
    second_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    second_title->SetFont(::Label::Body_14);
    second_title->SetToolTip(tooltip);
    second_title->Wrap(-1);

    sizer_input->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));
    sizer_input->Add(input_title , 0, wxALIGN_CENTER_VERTICAL);
    sizer_input->Add(input       , 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));
    sizer_input->Add(second_title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this, param, input, onchange](wxCommandEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        app_config->set(param, std::string(value.mb_str()));
        app_config->save();
        onchange(value);
        e.Skip();
    });

    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this, param, input, onchange](wxFocusEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        app_config->set(param, std::string(value.mb_str()));
        onchange(value);
        e.Skip();
    });

    return sizer_input;
}

wxBoxSizer *PreferencesDialog::create_camera_orbit_mult_input(wxString title, wxString tooltip)
{
    wxBoxSizer *sizer_input = new wxBoxSizer(wxHORIZONTAL);
    auto        input_title   = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    input_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    input_title->SetFont(::Label::Body_14);
    input_title->SetToolTip(tooltip);
    input_title->Wrap(DESIGN_TITLE_SIZE.x);
    auto param = "camera_orbit_mult";

    auto       input = new ::TextInput(m_parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    wxTextValidator validator(wxFILTER_NUMERIC);
    input->SetToolTip(tooltip);
    input->GetTextCtrl()->SetValidator(validator);

    sizer_input->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));
    sizer_input->Add(input_title, 0, wxALIGN_CENTER_VERTICAL);
    sizer_input->Add(input      , 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    const double min = 0.05;
    const double max = 2.0;

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this, param, input, min, max](wxCommandEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        double conv = 1.0;
        if (value.ToCDouble(&conv)) {
            conv = conv < min ? min : conv > max ? max : conv;
            auto strval = std::string(wxString::FromCDouble(conv, 2).mb_str());
            input->GetTextCtrl()->SetValue(strval);
            app_config->set(param, strval);
            app_config->save();
        }
        e.Skip();
    });

    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this, param, input, min, max](wxFocusEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        double conv = 1.0;
        if (value.ToCDouble(&conv)) {
            conv = conv < min ? min : conv > max ? max : conv;
            auto strval = std::string(wxString::FromCDouble(conv, 2).mb_str());
            input->GetTextCtrl()->SetValue(strval);
            app_config->set(param, strval);
        }
        e.Skip();
    });

    return sizer_input;
}

wxBoxSizer *PreferencesDialog::create_item_backup(wxString title, wxString tooltip)
{
    wxBoxSizer *m_sizer_input = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_input->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

    auto checkbox_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    checkbox_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    checkbox_title->SetFont(::Label::Body_14);
    checkbox_title->Wrap(DESIGN_TITLE_SIZE.x);
    checkbox_title->SetToolTip(tooltip);

    auto checkbox = new ::CheckBox(m_parent);
    checkbox->SetValue(app_config->get_bool("backup_switch"));
    checkbox->SetToolTip(tooltip);

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox](wxCommandEvent &e) {
        app_config->set_bool("backup_switch", checkbox->GetValue());
        app_config->save();
        bool pbool = app_config->get("backup_switch") == "true" ? true : false;
        std::string backup_interval = "10";
        app_config->get("backup_interval", backup_interval);
        Slic3r::set_backup_interval(pbool ? boost::lexical_cast<long>(backup_interval) : 0);
        if (m_backup_interval_textinput != nullptr) { m_backup_interval_textinput->Enable(pbool); }
        e.Skip();
    });

    m_backup_interval_time = app_config->get("backup_interval");

    auto input = new ::TextInput(m_parent, wxEmptyString, _L("sec"), "loop", wxDefaultPosition, wxSize(FromDIP(97), -1), wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(m_backup_interval_time);
    wxTextValidator validator(wxFILTER_DIGITS);
    input->SetToolTip(_L("The period of backup in seconds."));
    input->GetTextCtrl()->SetValidator(validator);

    m_sizer_input->Add(checkbox_title, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(3));
    m_sizer_input->Add(checkbox      , 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(5));
    m_sizer_input->Add(input         , 0, wxALIGN_CENTER_VERTICAL);

    input->GetTextCtrl()->Bind(wxEVT_COMMAND_TEXT_UPDATED, [this, input](wxCommandEvent &e) {
        m_backup_interval_time = input->GetTextCtrl()->GetValue();
        e.Skip();
    });

    std::function<void()> backup_interval = [this, input]() {
        m_backup_interval_time = input->GetTextCtrl()->GetValue();
        app_config->set("backup_interval", std::string(m_backup_interval_time.mb_str()));
        app_config->save();
        long backup_interval = 0;
        m_backup_interval_time.ToLong(&backup_interval);
        Slic3r::set_backup_interval(backup_interval);
    };

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [backup_interval](wxCommandEvent &e) {
        backup_interval();
        e.Skip();
    });

     input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [backup_interval](wxFocusEvent &e) {
        backup_interval();
        e.Skip();
    });

    input->Enable(app_config->get("backup_switch") == "true");
    input->Refresh();

    m_backup_interval_textinput = input;
    return m_sizer_input;
}

wxBoxSizer *PreferencesDialog::create_item_auto_reslice(wxString title, wxString checkbox_tooltip, wxString delay_tooltip)
{
    wxBoxSizer *sizer_row = new wxBoxSizer(wxHORIZONTAL);

    sizer_row->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

    auto checkbox_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    checkbox_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    checkbox_title->SetFont(::Label::Body_14);
    checkbox_title->Wrap(DESIGN_TITLE_SIZE.x);
    checkbox_title->SetToolTip(checkbox_tooltip);

    auto checkbox = new ::CheckBox(m_parent);
    checkbox->SetValue(app_config->get_bool("auto_slice_after_change"));
    checkbox->SetToolTip(checkbox_tooltip);

    wxString delay_value = app_config->get("auto_slice_change_delay_seconds");
    if (delay_value.empty())
        delay_value = "0";

    auto input = new ::TextInput(m_parent, wxEmptyString, _L("sec"), wxEmptyString, wxDefaultPosition, wxSize(FromDIP(97), -1), wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(delay_value);
    wxTextValidator validator(wxFILTER_DIGITS);
    input->SetToolTip(delay_tooltip);
    input->GetTextCtrl()->SetValidator(validator);

    sizer_row->Add(checkbox_title, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(3));
    sizer_row->Add(checkbox, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(5));
    sizer_row->Add(input, 0, wxALIGN_CENTER_VERTICAL);

    auto commit_delay = [this, input]() {
        wxString value = input->GetTextCtrl()->GetValue();
        long seconds = 0;
        if (!value.ToLong(&seconds) || seconds < 0)
            seconds = 0;
        wxString sanitized = wxString::Format("%ld", seconds);
        input->GetTextCtrl()->SetValue(sanitized);
        app_config->set("auto_slice_change_delay_seconds", std::string(sanitized.mb_str()));
        app_config->save();
    };

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [commit_delay](wxCommandEvent &e) {
        commit_delay();
        e.Skip();
    });

    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [commit_delay](wxFocusEvent &e) {
        commit_delay();
        e.Skip();
    });

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox, input](wxCommandEvent &e) {
        const bool enabled = checkbox->GetValue();
        app_config->set_bool("auto_slice_after_change", enabled);
        app_config->save();
        input->Enable(enabled);
        input->Refresh();
        e.Skip();
    });

    input->Enable(checkbox->GetValue());
    input->Refresh();

    return sizer_row;
}

wxBoxSizer* PreferencesDialog::create_item_darkmode(wxString title,wxString tooltip, std::string param)
{
    wxBoxSizer* m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_checkbox->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

    auto checkbox = new ::CheckBox(m_parent);
    checkbox->SetValue((app_config->get(param) == "1") ? true : false);
    m_dark_mode_ckeckbox = checkbox;

    auto checkbox_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    checkbox_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    checkbox_title->SetFont(::Label::Body_14);
    checkbox_title->Wrap(DESIGN_TITLE_SIZE.x);

    m_sizer_checkbox->Add(checkbox_title, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(3));
    m_sizer_checkbox->Add(checkbox      , 0, wxALIGN_CENTER | wxRIGHT | wxLEFT, FromDIP(5));

    //// save config
    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox, param](wxCommandEvent& e) {
        app_config->set(param, checkbox->GetValue() ? "1" : "0");
        app_config->save();
        wxGetApp().Update_dark_mode_flag();

        //dark mode
#ifdef _MSW_DARK_MODE
        wxGetApp().force_colors_update();
        wxGetApp().update_ui_from_settings();
        set_dark_mode();
#endif
        SimpleEvent evt = SimpleEvent(EVT_GLCANVAS_COLOR_MODE_CHANGED);
        wxPostEvent(wxGetApp().plater(), evt);
        e.Skip();
        });

    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty
    checkbox_title->SetToolTip(tip);
    checkbox->SetToolTip(tip);
    return m_sizer_checkbox;
}

void PreferencesDialog::set_dark_mode()
{
#ifdef __WINDOWS__
#ifdef _MSW_DARK_MODE
    NppDarkMode::SetDarkExplorerTheme(this->GetHWND());
    NppDarkMode::SetDarkTitleBar(this->GetHWND());
    wxGetApp().UpdateDlgDarkUI(this);
    SetActiveWindow(wxGetApp().mainframe->GetHWND());
    SetActiveWindow(GetHWND());
#endif
#endif
}

wxBoxSizer *PreferencesDialog::create_item_checkbox(wxString title, wxString tooltip, std::string param, const wxString secondary_title)
{
    wxBoxSizer *m_sizer_checkbox  = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_checkbox->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));
    
    auto tip = tooltip.IsEmpty() ? title : tooltip; // auto fill tooltips with title if its empty

    auto checkbox_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    checkbox_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    checkbox_title->SetFont(::Label::Body_14);
    checkbox_title->Wrap(DESIGN_TITLE_SIZE.x);
    checkbox_title->SetToolTip(tip);

    auto checkbox = new ::CheckBox(m_parent);
    checkbox->SetValue(app_config->get_bool(param));
    checkbox->SetToolTip(tip);

    m_sizer_checkbox->Add(checkbox_title, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(3));
    m_sizer_checkbox->Add(checkbox      , 0, wxALIGN_CENTER | wxRIGHT | wxLEFT, FromDIP(5));

    if(!secondary_title.IsEmpty()){
        auto sec_title = new wxStaticText(m_parent, wxID_ANY, secondary_title);
        sec_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
        sec_title->SetFont(::Label::Body_14);
        sec_title->Wrap(-1);
        sec_title->SetToolTip(tip);
        m_sizer_checkbox->Add(sec_title, 0, wxALIGN_CENTER);
    }

     //// save config
    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox, param](wxCommandEvent &e) {
        app_config->set_bool(param, checkbox->GetValue());
        app_config->save();

        // if (param == "staff_pick_switch") {
        //     bool pbool = app_config->get("staff_pick_switch") == "true";
        //     wxGetApp().switch_staff_pick(pbool);
        // }

        if (param == "sync_user_preset") {
            bool sync = app_config->get("sync_user_preset") == "true" ? true : false;
            if (sync) {
                wxGetApp().start_sync_user_preset();
            } else {
                wxGetApp().stop_sync_user_preset();
            }
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " sync_user_preset: " << (sync ? "true" : "false");
        }

        #ifdef __WXMSW__
        if (param == "associate_3mf") {
             bool pbool = app_config->get("associate_3mf") == "true" ? true : false;
             if (pbool) {
                 wxGetApp().associate_files(L"3mf");
             } else {
                 wxGetApp().disassociate_files(L"3mf");
             }
        }

        if (param == "associate_stl") {
            bool pbool = app_config->get("associate_stl") == "true" ? true : false;
            if (pbool) {
                wxGetApp().associate_files(L"stl");
            } else {
                wxGetApp().disassociate_files(L"stl");
            }
        }

        if (param == "associate_step") {
            bool pbool = app_config->get("associate_step") == "true" ? true : false;
            if (pbool) {
                wxGetApp().associate_files(L"step");
            } else {
                wxGetApp().disassociate_files(L"step");
            }
        }

        if (param == "installed_networking") {
            bool pbool = app_config->get_bool("installed_networking");
            if (pbool) {
                GUI::wxGetApp().CallAfter([] { GUI::wxGetApp().ShowDownNetPluginDlg(); });
            }
            if (m_legacy_networking_ckeckbox != nullptr) { m_legacy_networking_ckeckbox->Enable(pbool); }
        }

#endif // __WXMSW__

        if (param == "developer_mode") {
            m_developer_mode_def = app_config->get("developer_mode");
            if (m_developer_mode_def == "true") {
                Slic3r::GUI::wxGetApp().save_mode(comDevelop);
            } else {
                Slic3r::GUI::wxGetApp().save_mode(comAdvanced);
            }
        }

        // webview  dump_vedio
        if (param == "internal_developer_mode") {
            m_internal_developer_mode_def = app_config->get("internal_developer_mode");
            if (m_internal_developer_mode_def == "true") {
                Slic3r::GUI::wxGetApp().update_internal_development();
                Slic3r::GUI::wxGetApp().mainframe->show_log_window();
            } else {
                Slic3r::GUI::wxGetApp().update_internal_development();
            }
        }

        if (param == "enable_high_low_temp_mixed_printing") {
            if (checkbox->GetValue()) {
                const wxString warning_title = _L("Bed Temperature Difference Warning");
                const wxString warning_message = 
                    _L("Using filaments with significantly different temperatures may cause:\n"
                        "• Extruder clogging\n"
                        "• Nozzle damage\n"
                        "• Layer adhesion issues\n\n"
                        "Continue with enabling this feature?");
                std::function<void(const wxString&)> link_callback = [](const wxString&) {
                            const std::string lang_code = wxGetApp().app_config->get("language");
                            const wxString region = (lang_code.find("zh") != std::string::npos) ? L"zh" : L"en";
                            const wxString wiki_url = wxString::Format(
                                L"https://wiki.bambulab.com/%s/filament-acc/filament/h2d-filament-config-limit",
                                region
                            );
                            wxGetApp().open_browser_with_warning_dialog(wiki_url);
                            };

                MessageDialog msg_dialog(
                    nullptr,
                    warning_message,
                    warning_title,
                    wxICON_WARNING | wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE,
                    wxEmptyString,
                    _L("Click Wiki for help."),
                    link_callback
                );

                if (msg_dialog.ShowModal() != wxID_YES) {
                    checkbox->SetValue(false);
                    app_config->set_bool(param, false);
                    app_config->save();
                }
            }
        }

        e.Skip();
    });

    //// for debug mode
    if (param == "developer_mode") { m_developer_mode_ckeckbox = checkbox; }
    if (param == "internal_developer_mode") { m_internal_developer_mode_ckeckbox = checkbox; }
    if (param == "legacy_networking") { 
        m_legacy_networking_ckeckbox = checkbox;
        bool pbool = app_config->get_bool("installed_networking");
        checkbox->Enable(pbool);
    }

    return m_sizer_checkbox;
}

wxBoxSizer* PreferencesDialog::create_item_button(wxString title, wxString title2, wxString tooltip, wxString tooltip2, std::function<void()> onclick)
{
    wxBoxSizer *m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_checkbox->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));
    auto m_staticTextPath = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, wxST_NO_AUTORESIZE);
    m_staticTextPath->SetForegroundColour(DESIGN_GRAY900_COLOR);
    m_staticTextPath->SetFont(::Label::Body_14);
    m_staticTextPath->Wrap(DESIGN_TITLE_SIZE.x);
    
    m_staticTextPath->SetToolTip(tooltip.IsEmpty() ? tooltip2 : tooltip); // use button tooltip if label tooltip empty

    auto m_button_download = new Button(m_parent, title2);
    m_button_download->SetStyle(title2 == _L("Clear") ? ButtonStyle::Alert : ButtonStyle::Regular, ButtonType::Parameter);
    m_button_download->SetToolTip(tooltip2.IsEmpty() ? tooltip : tooltip2); // use label tooltip if button tooltip empty

    m_button_download->Bind(wxEVT_BUTTON, [this, onclick](auto &e) { onclick(); });

    m_sizer_checkbox->Add(m_staticTextPath , 0, wxALIGN_CENTER_VERTICAL);
    m_sizer_checkbox->Add(m_button_download, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    return m_sizer_checkbox;
}

wxBoxSizer* PreferencesDialog::create_item_downloads(wxString title, wxString tooltip)
{
    wxString download_path = wxString::FromUTF8(app_config->get("download_path"));

    wxBoxSizer* m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);
    wxPanel*    label_panel = new wxPanel(m_parent);
    wxBoxSizer* label_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_checkbox->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

    auto downloads_folder = new wxStaticText(label_panel, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    downloads_folder->SetForegroundColour(DESIGN_GRAY900_COLOR);
    downloads_folder->SetFont(::Label::Body_14);
    downloads_folder->SetToolTip(tooltip);
    downloads_folder->Wrap(-1);

    auto m_staticTextPath = new wxStaticText(label_panel, wxID_ANY, download_path, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_staticTextPath->SetForegroundColour(DESIGN_GRAY600_COLOR);
    m_staticTextPath->SetFont(::Label::Body_14);
    m_staticTextPath->Wrap(-1);
    m_staticTextPath->SetToolTip(download_path);

    label_sizer->Add(downloads_folder , 0, wxALIGN_CENTER_VERTICAL);
    label_sizer->Add(m_staticTextPath , 0, wxALIGN_CENTER_VERTICAL);
    label_panel->SetSize(   wxSize(DESIGN_TITLE_SIZE.x, -1));
    label_panel->SetMinSize(wxSize(DESIGN_TITLE_SIZE.x, -1));
    label_panel->SetMaxSize(wxSize(DESIGN_TITLE_SIZE.x, -1));
    label_panel->SetSizer(label_sizer);
    label_panel->Layout();

    auto m_button_download = new Button(m_parent, _L("Browse") + " " + dots);
    m_button_download->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    m_button_download->SetToolTip(_L("Choose folder for downloaded items"));

    m_button_download->Bind(wxEVT_BUTTON, [this, m_staticTextPath, m_sizer_checkbox](auto& e) {
        wxString defaultPath = wxT("/");
        wxDirDialog dialog(this, _L("Choose Download Directory"), defaultPath, wxDD_NEW_DIR_BUTTON);

        if (dialog.ShowModal() == wxID_OK) {
            wxString download_path = dialog.GetPath();
            std::string download_path_str = download_path.ToUTF8().data();
            app_config->set("download_path", download_path_str);
            m_staticTextPath->SetLabelText(download_path);
            m_staticTextPath->SetToolTip(download_path);
            m_sizer_checkbox->Layout();
        }
        });

    m_sizer_checkbox->Add(label_panel      , 0, wxALIGN_CENTER_VERTICAL);
    m_sizer_checkbox->Add(m_button_download, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    return m_sizer_checkbox;
}

#ifdef WIN32
wxBoxSizer* PreferencesDialog::create_item_link_association( wxString url_prefix, wxString website_name)
{
    wxString title = _L("Associate") + (boost::format(" %1%://") % url_prefix.c_str()).str();
    wxString tooltip = _L("Associate") + " " + url_prefix + ":// " + _L("with OrcaSlicer so that Orca can open models from") + " " + website_name;

    std::wstring registered_bin; // not used, just here to provide a ref to check fn
    bool reg_to_current_instance = wxGetApp().check_url_association(url_prefix.ToStdWstring(), registered_bin);

    auto* h_sizer = new wxBoxSizer(wxHORIZONTAL); // contains checkbox and other elements on the first line
    h_sizer->AddSpacer(FromDIP(DESIGN_LEFT_MARGIN));

    // build checkbox
    auto checkbox = new ::CheckBox(m_parent);
    checkbox->SetToolTip(tooltip);
    checkbox->SetValue(reg_to_current_instance); // If registered to the current instance, checkbox should be checked
    checkbox->Enable(!reg_to_current_instance); // Since unregistering isn't supported, checkbox is disabled when checked

    // build text next to checkbox
    auto checkbox_title = new wxStaticText(m_parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE);
    checkbox_title->SetToolTip(tooltip);
    checkbox_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    checkbox_title->SetFont(::Label::Body_14);
    checkbox_title->Wrap(-1);

    h_sizer->Add(checkbox_title, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(3));
    h_sizer->Add(checkbox      , 0, wxALIGN_CENTER | wxLEFT          , FromDIP(5));

    auto* v_sizer = new wxBoxSizer(wxVERTICAL);
    v_sizer->Add(h_sizer);

    // build text below checkbox that indicates the instance currently registered to handle the link type
    auto* registered_instance_title = new wxStaticText(m_parent, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    registered_instance_title->SetForegroundColour(DESIGN_GRAY600_COLOR);
    registered_instance_title->SetFont(::Label::Body_14);
    registered_instance_title->Wrap(-1);

    // update the text below checkbox
    auto update_current_association_str = [=, &reg_to_current_instance](){
        // get registered binary for given link type
        std::wstring registered_bin;
        reg_to_current_instance = wxGetApp().check_url_association(url_prefix.wc_str(), registered_bin);

        // format registered binary to get only the path and remove excess chars
        if (!registered_bin.empty())
            // skip idx 0 because it is the first quotation mark
            registered_bin = registered_bin.substr(1, registered_bin.find(L'\"', 1) - 1);

        wxString current_association_str = _L("Current Association: ");
        if (reg_to_current_instance) {
            current_association_str += _L("Current Instance");
            registered_instance_title->SetToolTip(_L("Current Instance Path: ") + registered_bin);
        } else if (registered_bin.empty())
            current_association_str += _L("None");
        else{
            current_association_str += registered_bin;
            registered_instance_title->SetToolTip(current_association_str);
        }

        registered_instance_title->SetLabel(current_association_str);
        registered_instance_title->SetMaxSize(wxSize(DESIGN_WINDOW_SIZE.x - FromDIP(DESIGN_LEFT_MARGIN) - FromDIP(40), -1)); // prevent horizontal scroll
    };
    update_current_association_str();

    v_sizer->Add(registered_instance_title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(DESIGN_LEFT_MARGIN));

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [=](wxCommandEvent& e) {
        wxGetApp().associate_url(url_prefix.ToStdWstring());
        checkbox->Disable();
        update_current_association_str();
        e.Skip();
    });

    return v_sizer;
}
#endif // WIN32

PreferencesDialog::PreferencesDialog(wxWindow *parent, wxWindowID id, const wxString &title, const wxPoint &pos, const wxSize &size, long style)
    : DPIDialog(parent, id, _L("Preferences"), pos, size, style)
{
    SetBackgroundColour(*wxWHITE);
    SetMinSize(DESIGN_WINDOW_SIZE);
    create();
    wxGetApp().UpdateDlgDarkUI(this);
}

void PreferencesDialog::create()
{
    app_config = get_app_config();

    m_parent = new MyscrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_parent->SetScrollRate(5, 5);
    m_parent->SetBackgroundColour(*wxWHITE);

    m_sizer_body = new wxBoxSizer(wxVERTICAL);

    m_pref_tabs = new TabCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_NO_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES | wxBORDER_NONE | wxWANTS_CHARS | wxTR_FULL_ROW_HIGHLIGHT);
    m_pref_tabs->Bind(wxEVT_RIGHT_DOWN, [this](auto &e) {}); // disable right select
    m_pref_tabs->SetFont(Label::Body_14);

    create_items();

    m_pref_tabs->Bind(wxEVT_TAB_SEL_CHANGED, [this](wxCommandEvent& e) {
        Freeze();
        #ifdef __linux__
            m_pref_tabs->SetFocus();
        #endif
        int selection = e.GetSelection();
        for (size_t i = 0; i < m_pref_tabs->GetCount(); ++i){
            m_pref_tabs->SetItemBold(i, i == selection);
            f_sizers[i]->Show(i == selection);
        }
        Layout();
        Thaw();
    });

    auto item_color = StateColor(
        std::make_pair(wxColour("#6B6B6C"), (int) StateColor::NotChecked),
        std::make_pair(wxColour("#363636"), (int) StateColor::Normal)
    );

    for (size_t i = 0; i < m_pref_tabs->GetCount(); ++i)
        m_pref_tabs->SetItemTextColour(i, item_color);

    m_pref_tabs->SelectItem(0);

    m_sizer_body->Add(m_pref_tabs, 0, wxEXPAND | wxBOTTOM | wxTOP, FromDIP(5));
    m_sizer_body->Add(m_parent, 1, wxEXPAND);

    SetSizer(m_sizer_body);
    Layout();
    Fit();
    CenterOnParent();
}

PreferencesDialog::~PreferencesDialog()
{
}

void PreferencesDialog::on_dpi_changed(const wxRect &suggested_rect) { this->Refresh(); }

void PreferencesDialog::Split(const std::string &src, const std::string &separator, std::vector<wxString> &dest)
{
    std::string            str = src;
    std::string            substring;
    std::string::size_type start = 0, index;
    dest.clear();
    index = str.find_first_of(separator, start);
    do {
        if (index != std::string::npos) {
            substring = str.substr(start, index - start);
            dest.push_back(substring);
            start = index + separator.size();
            index = str.find(separator, start);
            if (start == std::string::npos) break;
        }
    } while (index != std::string::npos);

    substring = str.substr(start);
    dest.push_back(substring);
}

void PreferencesDialog::create_items()
{
    // ORCA
    // Window focus follows item creation order. so below code has to be in same order with UI
    // Create functions for custom controls to keep list clean
    // Tooltips added automatically from related title if its empty

    wxBoxSizer*sizer_page = new wxBoxSizer(wxVERTICAL);
    wxFlexGridSizer* g_sizer; // use same name on all sizers to make easier to ordering without renaming
    auto v_gap = FromDIP(4);

    //////////////////////////
    //// GENERAL TAB 
    /////////////////////////////////////
    m_pref_tabs->AppendItem(_L("General"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// GENERAL > Settings
    g_sizer->Add(create_item_title(_L("Settings")), 1, wxEXPAND);

    auto item_language         = create_item_language_combobox(_L("Language"), "");
    g_sizer->Add(item_language);

    std::vector<wxString>Units = {_L("Metric") + " (mm, g)", _L("Imperial") + " (in, oz)"};
    auto item_currency         = create_item_combobox(_L("Units"), "", "use_inches", Units);
    g_sizer->Add(item_currency);

    std::vector<wxString> DefaultPage = {_L("Home"), _L("Prepare")};
    auto item_default_page     = create_item_combobox(_L("Default page"), _L("Set the page opened on startup."), "default_page", DefaultPage);
    g_sizer->Add(item_default_page);

#ifdef _WIN32
    auto item_darkmode         = create_item_darkmode(_L("Enable dark mode"), "", "dark_color_mode");
    g_sizer->Add(item_darkmode);
#endif

    auto item_single_instance  = create_item_checkbox(_L("Allow only one OrcaSlicer instance"),
    #if __APPLE__
            _L("On OSX there is always only one instance of app running by default. However it is allowed to run multiple instances "
                "of same app from the command line. In such case this settings will allow only one instance."),
    #else
            _L("If this is enabled, when starting OrcaSlicer and another instance of the same OrcaSlicer is already running, that instance will be reactivated instead."),
    #endif
            "single_instance");
    g_sizer->Add(item_single_instance);

    auto item_show_splash_scr  = create_item_checkbox(_L("Show splash screen"), _L("Show the splash screen during startup."), "show_splash_screen");
    g_sizer->Add(item_show_splash_scr);

    //auto item_hints            = create_item_checkbox(_L("Show \"Daily Tips\" after start"), page, _L("If enabled, useful hints are displayed at startup."), "show_daily_tips");
    //g_sizer->Add(item_hints);

    auto item_downloads        = create_item_downloads(_L("Downloads folder") + ": ", _L("Target folder for downloaded items"));
    g_sizer->Add(item_downloads);

    //// GENERAL > Project
    g_sizer->Add(create_item_title(_L("Project")), 1, wxEXPAND);

    std::vector<wxString> projectLoadSettingsBehaviourOptions = {_L("Load All"), _L("Ask When Relevant"), _L("Always Ask"), _L("Load Geometry Only")};
    std::vector<string>   projectLoadSettingsConfigOptions    = { OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_ALL, OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT, OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK, OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY };
    auto item_project_load     = create_item_combobox(_L("Load behaviour"), _L("Should printer/filament/process settings be loaded when opening a 3MF file?"), SETTING_PROJECT_LOAD_BEHAVIOUR, projectLoadSettingsBehaviourOptions, projectLoadSettingsConfigOptions);
    g_sizer->Add(item_project_load);

    auto item_max_recent_count = create_item_input(_L("Maximum recent files"), "", _L("Maximum count of recent files"), "max_recent_count", [](wxString value) {
        long max = 0;
        if (value.ToLong(&max))
            wxGetApp().mainframe->set_max_recent_count(max);
    });
    g_sizer->Add(item_max_recent_count);

    auto item_recent_models    = create_item_checkbox(_L("Add STL/STEP files to recent files list"), "", "recent_models");
    g_sizer->Add(item_recent_models);

    auto item_gcodes_warning   = create_item_checkbox(_L("Don't warn when loading 3MF with modified G-code"), "", "no_warn_when_modified_gcodes");
    g_sizer->Add(item_gcodes_warning);

    auto item_step_dialog      = create_item_checkbox(_L("Show options when importing STEP file"), _L("If enabled, a parameter settings dialog will appear during STEP file import."), "enable_step_mesh_setting");
    g_sizer->Add(item_step_dialog);

    auto item_backup           = create_item_backup(_L("Auto backup"), _L("Backup your project periodically for restoring from the occasional crash."));
    g_sizer->Add(item_backup); 

    //// GENERAL > Preset
    g_sizer->Add(create_item_title(_L("Preset")), 1, wxEXPAND);

    auto item_remember_printer = create_item_checkbox(_L("Remember printer configuration"), _L("If enabled, Orca will remember and switch filament/process configuration for each printer automatically."), "remember_printer_config");
    g_sizer->Add(item_remember_printer);

    auto item_filament_preset_grouping = create_item_combobox(_L("Group user filament presets"), _L("Group user filament presets based on selection"),
        "group_filament_presets", {_L("All"), _L("None"), _L("By type"), _L("By vendor")}, [](wxString value) {wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_FILAMENT);});
    g_sizer->Add(item_filament_preset_grouping);

    //// GENERAL > Features
    g_sizer->Add(create_item_title(_L("Features")), 1, wxEXPAND);

    auto item_multi_machine    = create_item_checkbox(_L("Multi device management"), _L("With this option enabled, you can send a task to multiple devices at the same time and manage multiple devices."), "enable_multi_machine", _L("(Requires restart)"));
    g_sizer->Add(item_multi_machine);

#if 0
    g_sizer->Add(create_item_title(_L("Filament Grouping")), 1, wxEXPAND);
    //temporarily disable it
    //auto item_ignore_ext_filament = create_item_checkbox(_L("Ignore ext filament when auto grouping"), _L("Ignore ext filament when auto grouping"), 50, "ignore_ext_filament_when_group");
    auto item_pop_up_filament_map_dialog = create_item_checkbox(_L("Pop up to select filament grouping mode"), _L("Pop up to select filament grouping mode"), 50, "pop_up_filament_map_dialog");
    g_sizer->Add(item_pop_up_filament_map_dialog);
#endif

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);

    //////////////////////////
    //// CONTROL TAB 
    /////////////////////////////////////
    m_pref_tabs->AppendItem(_L("Control"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// CONTROL > Behaviour
    g_sizer->Add(create_item_title(_L("Behaviour")), 1, wxEXPAND);

    std::vector<wxString> FlushOptionLabels = {_L("All"),_L("Color"),_L("None")};
    std::vector<std::string> FlushOptionValues = { "all","color change","disabled" };
    auto item_auto_flush = create_item_combobox(_L("Auto flush after changing..."), _L("Auto calculate flushing volumes when selected values changed"), "auto_calculate_flush", FlushOptionLabels, FlushOptionValues);
    g_sizer->Add(item_auto_flush);

    auto item_auto_arrange     = create_item_checkbox(_L("Auto arrange plate after cloning"), "", "auto_arrange");
    g_sizer->Add(item_auto_arrange);

    auto item_auto_reslice = create_item_auto_reslice(
        _L("Auto slice after changes"),
        _L("If enabled, OrcaSlicer will re-slice automatically whenever slicing-related settings change."),
        _L("Delay in seconds before auto slicing starts, allowing multiple edits to be grouped. Use 0 to slice immediately."));
    g_sizer->Add(item_auto_reslice);
 
    //// CONTROL > Camera
    g_sizer->Add(create_item_title(_L("Camera")), 1, wxEXPAND);

    std::vector<wxString> CameraNavStyle = {_L("Default"), _L("Touchpad")};
    auto item_camera_nav_style = create_item_combobox(_L("Camera style"), _L("Select camera navigation style.\nDefault: LMB+move for rotation, RMB/MMB+move for panning.\nTouchpad: Alt+move for rotation, Shift+move for panning."), "camera_navigation_style", CameraNavStyle);
    g_sizer->Add(item_camera_nav_style);

    auto camera_orbit_mult     = create_camera_orbit_mult_input(_L("Orbit speed multiplier"), _L("Multiplies the orbit speed for finer or coarser camera movement."));
    g_sizer->Add(camera_orbit_mult);

    auto item_zoom_to_mouse    = create_item_checkbox(_L("Zoom to mouse position"), _L("Zoom in towards the mouse pointer's position in the 3D view, rather than the 2D window center."), "zoom_to_mouse");
    g_sizer->Add(item_zoom_to_mouse);

    auto item_use_free_camera  = create_item_checkbox(_L("Use free camera"), _L("If enabled, use free camera. If not enabled, use constrained camera."), "use_free_camera");
    g_sizer->Add(item_use_free_camera);

    auto swap_pan_rotate       = create_item_checkbox(_L("Swap pan and rotate mouse buttons"), _L("If enabled, swaps the left and right mouse buttons pan and rotate functions."), "swap_mouse_buttons");
    g_sizer->Add(swap_pan_rotate);

    auto reverse_mouse_zoom    = create_item_checkbox(_L("Reverse mouse zoom"), _L("If enabled, reverses the direction of zoom with mouse wheel."), "reverse_mouse_wheel_zoom");
    g_sizer->Add(reverse_mouse_zoom);

    //// CONTROL > Clear my choice on ...
    g_sizer->Add(create_item_title(_L("Clear my choice on...")), 1, wxEXPAND);

    auto item_save_choise      = create_item_button(_L("Unsaved projects"), _L("Clear"), "", _L("Clear my choice on the unsaved projects."), []() {
        wxGetApp().app_config->set("save_project_choise", "");
    });
    g_sizer->Add(item_save_choise);

    auto item_save_presets     = create_item_button(_L("Unsaved presets"), _L("Clear"), "", _L("Clear my choice on the unsaved presets."), []() {
        wxGetApp().app_config->set("save_preset_choise", "");
    });
    g_sizer->Add(item_save_presets);

    auto item_restore_hide_pop_ups = create_item_button(_L("Synchronizing printer preset"), _L("Clear"), L"", _L("Clear my choice for synchronizing printer preset after loading the file."), []() {
        wxGetApp().app_config->erase("app", "sync_after_load_file_show_flag");
    });
    g_sizer->Add(item_restore_hide_pop_ups);

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);

    //////////////////////////
    //// ONLINE TAB 
    /////////////////////////////////////
    m_pref_tabs->AppendItem(_L("Online"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// ONLINE > Connection
    g_sizer->Add(create_item_title(_L("Connection")), 1, wxEXPAND);

    auto item_region           = create_item_region_combobox(_L("Login region"), "");
    g_sizer->Add(item_region);
 
    auto item_stealth_mode     = create_item_checkbox(_L("Stealth mode"), _L("This stops the transmission of data to Bambu's cloud services. Users who don't use BBL machines or use LAN mode only can safely turn on this function."), "stealth_mode");
    g_sizer->Add(item_stealth_mode);

    auto item_network_test     = create_item_button(_L("Network test"), _L("Test") + " " + dots, "", _L("Open Network Test"), []() {
        NetworkTestDialog dlg(wxGetApp().mainframe);
        dlg.ShowModal();
    });
    g_sizer->Add(item_network_test);

    //// ONLINE > Update & sync
    g_sizer->Add(create_item_title(_L("Update & sync")), 1, wxEXPAND);

    auto item_stable_updates   = create_item_checkbox(_L("Check for stable updates only"), "", "check_stable_update_only");
    g_sizer->Add(item_stable_updates);

    auto item_user_sync        = create_item_checkbox(_L("Auto sync user presets (Printer/Filament/Process)"), "", "sync_user_preset");
    g_sizer->Add(item_user_sync);

    auto item_system_sync      = create_item_checkbox(_L("Update built-in Presets automatically."), "", "sync_system_preset");
    g_sizer->Add(item_system_sync);

    //// ONLINE > Network plugin
    g_sizer->Add(create_item_title(_L("Network plugin")), 1, wxEXPAND);

    auto item_enable_plugin    = create_item_checkbox(_L("Enable network plugin"), "", "installed_networking");
    g_sizer->Add(item_enable_plugin);
    
    auto item_legacy_network   = create_item_checkbox(_L("Use legacy network plugin"), _L("Disable to use latest network plugin that supports new BambuLab firmwares."), "legacy_networking", _L("(Requires restart)"));
    g_sizer->Add(item_legacy_network);

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);

    //////////////////////////
    //// ASSOCIATE TAB 
    /////////////////////////////////////
#ifdef _WIN32
    m_pref_tabs->AppendItem(_L("Associate"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// ASSOCIATE > Extensions
    g_sizer->Add(create_item_title(_L("Associate files to OrcaSlicer")), 1, wxEXPAND);

    auto item_associate_3mf    = create_item_checkbox(_L("Associate 3MF files to OrcaSlicer"), _L("If enabled, sets OrcaSlicer as default application to open 3MF files.") , "associate_3mf");
    g_sizer->Add(item_associate_3mf);

    auto item_associate_stl    = create_item_checkbox(_L("Associate STL files to OrcaSlicer"), _L("If enabled, sets OrcaSlicer as default application to open STL files.") , "associate_stl");
    g_sizer->Add(item_associate_stl);

    auto item_associate_step   = create_item_checkbox(_L("Associate STEP files to OrcaSlicer"), _L("If enabled, sets OrcaSlicer as default application to open STEP files."), "associate_step");
    g_sizer->Add(item_associate_step);

    //// ASSOCIATE > WebLinks
    g_sizer->Add(create_item_title(_L("Associate web links to OrcaSlicer")), 1, wxEXPAND);

    auto associate_url_prusa   = create_item_link_association(L"prusaslicer", "Printables.com");
    g_sizer->Add(associate_url_prusa);

    auto associate_url_bambu   = create_item_link_association(L"bambustudio", "Makerworld.com");
    g_sizer->Add(associate_url_bambu);

    auto associate_url_cura    = create_item_link_association(L"cura", "Thingiverse.com");
    g_sizer->Add(associate_url_cura);

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);
#endif // _WIN32

    //////////////////////////
    //// DEVELOPER TAB
    /////////////////////////////////////
    m_pref_tabs->AppendItem(_L("Developer"));
    f_sizers.push_back(new wxFlexGridSizer(1, 1, v_gap, 0));
    g_sizer = f_sizers.back();
    g_sizer->AddGrowableCol(0, 1);

    //// DEVELOPER > Settings
    g_sizer->Add(create_item_title(_L("Settings")), 1, wxEXPAND);

    auto item_develop_mode     = create_item_checkbox(_L("Develop mode"), "", "developer_mode");
    g_sizer->Add(item_develop_mode);

    auto item_ams_blacklist    = create_item_checkbox(_L("Skip AMS blacklist check"), "", "skip_ams_blacklist_check");
    g_sizer->Add(item_ams_blacklist);

    auto item_mix_print_high_low_temperature = create_item_checkbox(_L("Remove mixed temperature restriction"), _L("With this option enabled, you can print materials with a large temperature difference together."), "enable_high_low_temp_mixed_printing");
    g_sizer->Add(item_mix_print_high_low_temperature);

    g_sizer->Add(create_item_title(_L("Storage")), 1, wxEXPAND);
    auto item_allow_abnormal_storage = create_item_checkbox(_L("Allow Abnormal Storage"), _L("This allows the use of Storage that is marked as abnormal by the Printer.\nUse at your own risk, can cause issues!"), "allow_abnormal_storage");
    g_sizer->Add(item_allow_abnormal_storage);

    g_sizer->Add(create_item_title(_L("Log Level")), 1, wxEXPAND);
    auto log_level_list  = std::vector<wxString>{_L("fatal"), _L("error"), _L("warning"), _L("info"), _L("debug"), _L("trace")};
    auto loglevel_combox = create_item_loglevel_combobox(_L("Log Level"), _L("Log Level"), log_level_list);
    g_sizer->Add(loglevel_combox);

    //// DEVELOPER > Debug
#if !BBL_RELEASE_TO_PUBLIC
    g_sizer->Add(create_item_title(_L("Debug")), 1, wxEXPAND);
    auto debug_page            = create_debug_page();
    g_sizer->Add(debug_page, 1, wxEXPAND);
#endif

    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);

    /////////////////////////////////////
    //////////////////////////

    g_sizer = nullptr;

    // Hide all tabs instead first one
    for (size_t i = 1; i < f_sizers.size(); ++i)
        f_sizers[i]->Show(false);

    /////////////////////////////////////
    //////////////////////////

    m_parent->SetSizer(sizer_page);
    m_parent->Layout();
    sizer_page->Fit(m_parent);
}

void PreferencesDialog::create_sync_page()
{
    auto page = new wxWindow(this, wxID_ANY);
    wxBoxSizer *sizer_page = new wxBoxSizer(wxVERTICAL);

    auto title_sync_settingy   = create_item_title(_L("Sync settings"));
    auto item_user_sync        = create_item_checkbox(_L("User sync"), _L("User sync"), "user_sync_switch");
    auto item_preset_sync      = create_item_checkbox(_L("Preset sync"), _L("Preset sync"), "preset_sync_switch");
    auto item_preferences_sync = create_item_checkbox(_L("Preferences sync"), _L("Preferences sync"), "preferences_sync_switch");

    sizer_page->Add(title_sync_settingy, 0, wxTOP, 26);
    sizer_page->Add(item_user_sync, 0, wxTOP, 6);
    sizer_page->Add(item_preset_sync, 0, wxTOP, 6);
    sizer_page->Add(item_preferences_sync, 0, wxTOP, 6);

    page->SetSizer(sizer_page);
    page->Layout();
    sizer_page->Fit(page);
}

void PreferencesDialog::create_shortcuts_page()
{
    auto page = new wxWindow(this, wxID_ANY);
    wxBoxSizer *sizer_page = new wxBoxSizer(wxVERTICAL);

    auto title_view_control = create_item_title(_L("View control settings"));
    std::vector<wxString> keyboard_supported;
    Split(app_config->get("keyboard_supported"), "/", keyboard_supported);

    std::vector<wxString> mouse_supported;
    Split(app_config->get("mouse_supported"), "/", mouse_supported);

    auto item_rotate_view = create_item_multiple_combobox(_L("Rotate of view"), _L("Rotate of view"), "rotate_view", keyboard_supported,
                                                               mouse_supported);
    auto item_move_view   = create_item_multiple_combobox(_L("Move of view"), _L("Move of view"), "move_view", keyboard_supported, mouse_supported);
    auto item_zoom_view   = create_item_multiple_combobox(_L("Zoom of view"), _L("Zoom of view"), "rotate_view", keyboard_supported, mouse_supported);

    auto title_other = create_item_title(_L("Other"));
    auto item_other  = create_item_checkbox(_L("Mouse wheel reverses when zooming"), _L("Mouse wheel reverses when zooming"), "mouse_wheel");

    sizer_page->Add(title_view_control, 0, wxTOP, 26);
    sizer_page->Add(item_rotate_view, 0, wxTOP, 8);
    sizer_page->Add(item_move_view, 0, wxTOP, 8);
    sizer_page->Add(item_zoom_view, 0, wxTOP, 8);
    // sizer_page->Add(item_precise_control, 0, wxTOP, 0);
    sizer_page->Add(title_other, 0, wxTOP, 20);
    sizer_page->Add(item_other, 0, wxTOP, 5);

    page->SetSizer(sizer_page);
    page->Layout();
    sizer_page->Fit(page);
}

wxBoxSizer* PreferencesDialog::create_debug_page()
{
    m_internal_developer_mode_def = app_config->get("internal_developer_mode");
    m_backup_interval_def = app_config->get("backup_interval");
    m_iot_environment_def = app_config->get("iot_environment");

    wxBoxSizer *bSizer = new wxBoxSizer(wxVERTICAL);

    auto enable_ssl_for_mqtt = create_item_checkbox(_L("Enable SSL(MQTT)"), _L("Enable SSL(MQTT)"), "enable_ssl_for_mqtt");
    auto enable_ssl_for_ftp = create_item_checkbox(_L("Enable SSL(FTP)"), _L("Enable SSL(MQTT)"), "enable_ssl_for_ftp");
    auto item_internal_developer = create_item_checkbox(_L("Internal developer mode"), _L("Internal developer mode"), "internal_developer_mode");

    auto title_host = create_item_title(_L("Host Setting"));
    // ORCA RadioGroup
    auto radio_group = new RadioGroup(m_parent, {
        _L("DEV host: api-dev.bambu-lab.com/v1"), // 0
        _L("QA  host: api-qa.bambu-lab.com/v1"),  // 1
        _L("PRE host: api-pre.bambu-lab.com/v1"), // 2
        _L("Product host")                        // 3
    }, wxVERTICAL);

    radio_group->SetRadioTooltip(0, "dev_host");
    radio_group->SetRadioTooltip(1, "qa_host");
    radio_group->SetRadioTooltip(2, "pre_host");
    radio_group->SetRadioTooltip(3, "product_host");

    if (m_iot_environment_def == ENV_DEV_HOST) {
        radio_group->SetSelection(0);
    } else if (m_iot_environment_def == ENV_QAT_HOST) {
        radio_group->SetSelection(1);
    } else if (m_iot_environment_def == ENV_PRE_HOST) {
        radio_group->SetSelection(2);
    } else if (m_iot_environment_def == ENV_PRODUCT_HOST) {
        radio_group->SetSelection(3);
    }

    Button* debug_button = new Button(m_parent, _L("debug save button"));
    debug_button->SetStyle(ButtonStyle::Confirm, ButtonType::Window);

    debug_button->Bind(wxEVT_LEFT_DOWN, [this, radio_group](wxMouseEvent &e) {
        // success message box
        MessageDialog dialog(this, _L("save debug settings"), _L("DEBUG settings have been saved successfully!"), wxNO_DEFAULT | wxYES_NO | wxICON_INFORMATION);
        dialog.SetSize(400,-1);
        switch (dialog.ShowModal()) {
        case wxID_NO: {
            //if (m_developer_mode_def != app_config->get("developer_mode")) {
            //    app_config->set_bool("developer_mode", m_developer_mode_def == "true" ? true : false);
            //    m_developer_mode_ckeckbox->SetValue(m_developer_mode_def == "true" ? true : false);
            //}
            //if (m_internal_developer_mode_def != app_config->get("internal_developer_mode")) {
            //    app_config->set_bool("internal_developer_mode", m_internal_developer_mode_def == "true" ? true : false);
            //    m_internal_developer_mode_ckeckbox->SetValue(m_internal_developer_mode_def == "true" ? true : false);
            //}

            if (m_backup_interval_def != m_backup_interval_time) { m_backup_interval_textinput->GetTextCtrl()->SetValue(m_backup_interval_def); }

            if (m_iot_environment_def == ENV_DEV_HOST) {
                radio_group->SetSelection(0);
            } else if (m_iot_environment_def == ENV_QAT_HOST) {
                radio_group->SetSelection(1);
            } else if (m_iot_environment_def == ENV_PRE_HOST) {
                radio_group->SetSelection(2);
            } else if (m_iot_environment_def == ENV_PRODUCT_HOST) {
                radio_group->SetSelection(3);
            }

            break;
        }

        case wxID_YES: {
            // bbs  domain changed
            auto param = radio_group->GetSelection();

            std::map<wxString, wxString> iot_environment_map;
            iot_environment_map["dev_host"] = ENV_DEV_HOST;
            iot_environment_map["qa_host"]  = ENV_QAT_HOST;
            iot_environment_map["pre_host"] = ENV_PRE_HOST;
            iot_environment_map["product_host"] = ENV_PRODUCT_HOST;

            //if (iot_environment_map[param] != m_iot_environment_def) {
            if (true) {
                NetworkAgent* agent = wxGetApp().getAgent();
                if      (param == 0) { // "dev_host"
                    app_config->set("iot_environment", ENV_DEV_HOST);
                }
                else if (param == 1) { // "qa_host"
                    app_config->set("iot_environment", ENV_QAT_HOST);
                }
                else if (param == 2) { // "pre_host"
                    app_config->set("iot_environment", ENV_PRE_HOST);
                }
                else if (param == 3) { // "product_host"
                    app_config->set("iot_environment", ENV_PRODUCT_HOST);
                }

                AppConfig* config = GUI::wxGetApp().app_config;
                std::string country_code = config->get_country_code();
                if (agent) {
                    wxGetApp().request_user_logout();
                    agent->set_country_code(country_code);
                }
                ConfirmBeforeSendDialog confirm_dlg(this, wxID_ANY, _L("Warning"), ConfirmBeforeSendDialog::VisibleButtons::ONLY_CONFIRM);  // ORCA VisibleButtons instead ButtonStyle 
                confirm_dlg.update_text(_L("Cloud environment switched, please login again!"));
                confirm_dlg.on_show();
            }

            // bbs  backup
            //app_config->set("backup_interval", std::string(m_backup_interval_time.mb_str()));
            app_config->save();
            Slic3r::set_backup_interval(boost::lexical_cast<long>(app_config->get("backup_interval")));

            this->Close();
            break;
        }
        }
    });

    bSizer->Add(enable_ssl_for_mqtt, 0, wxTOP, FromDIP(3));
    bSizer->Add(enable_ssl_for_ftp, 0, wxTOP, FromDIP(3));
    bSizer->Add(item_internal_developer, 0, wxTOP, FromDIP(3));
    bSizer->Add(title_host, 0, wxEXPAND);
    bSizer->Add(radio_group, 0, wxEXPAND | wxLEFT, FromDIP(DESIGN_LEFT_MARGIN));
    bSizer->Add(debug_button, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(15));

    return bSizer;
}

}} // namespace Slic3r::GUI
