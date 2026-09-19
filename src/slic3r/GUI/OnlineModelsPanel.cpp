#include "OnlineModelsPanel.hpp"

#include "libslic3r/AppConfig.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/ProgressBar.hpp"
#include "Widgets/PopupWindow.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/WebView.hpp"
#include "wxExtensions.hpp"
#include "format.hpp"

#include <boost/filesystem.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <utility>

#include <wx/filedlg.h>
#include <wx/datetime.h>
#include <wx/filename.h>
#include <wx/listbox.h>
#include <wx/menu.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/uri.h>
#include <wx/utils.h>
#include <wx/webview.h>

namespace Slic3r::GUI {
namespace {

constexpr const char* provider_config_section = "online_models";
constexpr const char* provider_config_key = "providers";

Button* make_icon_button(wxWindow* parent, const wxString& icon, const wxString& label)
{
    auto* button = new Button(parent, wxEmptyString, icon, 0, 16);
    button->SetStyle(ButtonStyle::Regular, ButtonType::Icon);
    button->SetToolTip(label);
    button->SetName(label);
    return button;
}

Button* make_action_button(wxWindow* parent, const wxString& label, const wxString& icon)
{
    auto* button = new Button(parent, label, icon, 0, 16);
    button->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    return button;
}

bool is_safe_external_url(const wxString& url)
{
    const wxURI uri(url);
    const wxString scheme = uri.GetScheme().Lower();
    return (scheme == "https" || scheme == "http") && !uri.GetServer().empty();
}

class SourceEditDialog final : public DPIDialog
{
public:
    SourceEditDialog(wxWindow* parent, OnlineModelsProvider provider, bool adding)
        : DPIDialog(parent, wxID_ANY, adding ? _L("Add source") : _L("Edit source"), wxDefaultPosition,
                    wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
        , m_provider(std::move(provider))
    {
        SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto add_field = [&](const wxString& label, TextInput*& input, const std::string& value) {
            auto* field_label = new ::Label(this, ::Label::Body_14, label);
            field_label->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));
            sizer->Add(field_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
            input = new TextInput(this, wxString::FromUTF8(value), wxEmptyString, wxEmptyString,
                                  wxDefaultPosition, FromDIP(wxSize(440, 32)));
            sizer->Add(input, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
        };
        add_field(_L("Display name"), m_name, m_provider.display_name);
        add_field(_L("Homepage URL"), m_url, m_provider.homepage_url);

        auto* enabled_row = new wxBoxSizer(wxHORIZONTAL);
        m_enabled = new CheckBox(this);
        m_enabled->SetValue(m_provider.enabled);
        enabled_row->Add(m_enabled, 0, wxALIGN_CENTER_VERTICAL);
        auto* enabled_label = new ::Label(this, ::Label::Body_14, _L("Enabled"));
        enabled_label->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));
        enabled_row->Add(enabled_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        sizer->Add(enabled_row, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        actions->AddStretchSpacer();
        auto* cancel = make_action_button(this, _L("Cancel"), "");
        auto* save = make_action_button(this, _L("Save"), "");
        save->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        actions->Add(cancel, 0, wxRIGHT, FromDIP(8));
        actions->Add(save, 0);
        sizer->Add(actions, 0, wxEXPAND | wxALL, FromDIP(20));
        SetSizerAndFit(sizer);
        CentreOnParent();
        wxGetApp().UpdateDarkUI(this);

        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept(); });
    }

    const OnlineModelsProvider& provider() const { return m_provider; }

private:
    void on_dpi_changed(const wxRect&) override
    {
        m_name->Rescale();
        m_url->Rescale();
        m_enabled->Rescale();
        Layout();
        Fit();
    }

    void accept()
    {
        OnlineModelsProvider candidate = m_provider;
        candidate.display_name = m_name->GetTextCtrl()->GetValue().utf8_string();
        candidate.homepage_url = m_url->GetTextCtrl()->GetValue().utf8_string();
        candidate.enabled = m_enabled->GetValue();

        bool unencrypted = false;
        std::string normalized;
        if (!online_models_validate_homepage_url(candidate.homepage_url, &unencrypted, &normalized)
            || candidate.display_name.empty()) {
            show_error(this, _L("Enter a display name and a valid HTTP or HTTPS homepage URL."));
            return;
        }
        if (unencrypted && MessageDialog(this,
                _L("This source uses an unencrypted HTTP connection. Continue?"),
                _L("Unencrypted source"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING).ShowModal() != wxID_YES)
            return;
        candidate.homepage_url = normalized;
        m_provider = std::move(candidate);
        EndModal(wxID_OK);
    }

    OnlineModelsProvider m_provider;
    TextInput* m_name { nullptr };
    TextInput* m_url { nullptr };
    CheckBox* m_enabled { nullptr };
};

class SourceManagerDialog final : public DPIDialog
{
public:
    SourceManagerDialog(wxWindow* parent, const std::vector<OnlineModelsProvider>& providers)
        : DPIDialog(parent, wxID_ANY, _L("Manage sources"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
        , m_providers(providers)
    {
        SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(520, 260)));
        sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

        auto* tools = new wxBoxSizer(wxHORIZONTAL);
        m_add = make_action_button(this, _L("Add"), "add");
        m_edit = make_action_button(this, _L("Edit"), "edit");
        m_remove = make_action_button(this, _L("Remove"), "delete");
        m_up = make_icon_button(this, "page_up", _L("Move up"));
        m_down = make_icon_button(this, "page_down", _L("Move down"));
        for (Button* button : {m_add, m_edit, m_remove, m_up, m_down})
            tools->Add(button, 0, wxRIGHT, FromDIP(8));
        tools->AddStretchSpacer();
        m_restore = make_action_button(this, _L("Restore defaults"), "settings");
        tools->Add(m_restore, 0);
        sizer->Add(tools, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        actions->AddStretchSpacer();
        auto* cancel = make_action_button(this, _L("Cancel"), "");
        auto* save = make_action_button(this, _L("Save"), "");
        save->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        actions->Add(cancel, 0, wxRIGHT, FromDIP(8));
        actions->Add(save, 0);
        sizer->Add(actions, 0, wxEXPAND | wxALL, FromDIP(20));
        SetSizerAndFit(sizer);
        CentreOnParent();
        wxGetApp().UpdateDarkUI(this);

        m_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add(); });
        m_edit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { edit(); });
        m_remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { remove(); });
        m_up->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { move(-1); });
        m_down->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { move(1); });
        m_restore->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            online_models_restore_default_providers(m_providers);
            refresh();
        });
        m_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { update_buttons(); });
        m_list->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { edit(); });
        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        refresh();
    }

    const std::vector<OnlineModelsProvider>& providers() const { return m_providers; }

private:
    void on_dpi_changed(const wxRect&) override
    {
        for (Button* button : {m_add, m_edit, m_remove, m_up, m_down, m_restore})
            button->Rescale();
        Layout();
        Fit();
    }

    int selection() const { return m_list->GetSelection(); }

    void refresh(int selection = wxNOT_FOUND)
    {
        m_list->Clear();
        for (const OnlineModelsProvider& provider : m_providers) {
            wxString label = wxString::FromUTF8(provider.display_name);
            if (!provider.enabled)
                label += " (" + _L("Disabled") + ")";
            m_list->Append(label);
        }
        if (selection != wxNOT_FOUND && selection < static_cast<int>(m_providers.size()))
            m_list->SetSelection(selection);
        update_buttons();
    }

    void update_buttons()
    {
        const int selected = selection();
        const bool valid = selected != wxNOT_FOUND;
        m_edit->Enable(valid);
        m_remove->Enable(valid);
        m_up->Enable(valid && selected > 0);
        m_down->Enable(valid && selected + 1 < static_cast<int>(m_providers.size()));
    }

    void add()
    {
        OnlineModelsProvider provider;
        provider.id = "custom-" + boost::uuids::to_string(boost::uuids::random_generator()());
        provider.homepage_url = "https://";
        SourceEditDialog dialog(this, provider, true);
        if (dialog.ShowModal() == wxID_OK) {
            m_providers.push_back(dialog.provider());
            refresh(static_cast<int>(m_providers.size()) - 1);
        }
    }

    void edit()
    {
        const int selected = selection();
        if (selected == wxNOT_FOUND)
            return;
        SourceEditDialog dialog(this, m_providers[selected], false);
        if (dialog.ShowModal() == wxID_OK) {
            m_providers[selected] = dialog.provider();
            refresh(selected);
        }
    }

    void remove()
    {
        const int selected = selection();
        if (selected == wxNOT_FOUND)
            return;
        m_providers.erase(m_providers.begin() + selected);
        refresh(std::min(selected, static_cast<int>(m_providers.size()) - 1));
    }

    void move(int delta)
    {
        const int selected = selection();
        const int target = selected + delta;
        if (selected == wxNOT_FOUND || target < 0 || target >= static_cast<int>(m_providers.size()))
            return;
        std::swap(m_providers[selected], m_providers[target]);
        refresh(target);
    }

    std::vector<OnlineModelsProvider> m_providers;
    wxListBox* m_list { nullptr };
    Button* m_add { nullptr };
    Button* m_edit { nullptr };
    Button* m_remove { nullptr };
    Button* m_up { nullptr };
    Button* m_down { nullptr };
    Button* m_restore { nullptr };
};

} // namespace

struct OnlineModelsPanel::DownloadRow {
    wxPanel* panel { nullptr };
    ::Label* name { nullptr };
    ::Label* source { nullptr };
    ::Label* status { nullptr };
    ProgressBar* progress { nullptr };
    Button* cancel { nullptr };
    Button* open { nullptr };
    Button* show { nullptr };
    Button* remove { nullptr };
    boost::filesystem::path destination;
};

OnlineModelsPanel::OnlineModelsPanel(wxWindow* parent, std::function<void()> on_back)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
    , m_on_back(std::move(on_back))
{
    load_provider_registry();
    create_ui();
    apply_theme();
    ensure_download_dir();
    refresh_provider_controls();
    initialize_downloads();
}

OnlineModelsPanel::~OnlineModelsPanel()
{
    // Detach native callbacks before wxWidgets starts destroying child controls.
    m_download_subscription.reset();
    for (const auto& item : m_download_rows)
        delete item.second;
    m_download_rows.clear();
    if (m_download_popup)
        m_download_popup->Destroy();
}

void OnlineModelsPanel::create_ui()
{
    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    m_toolbar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    auto* toolbar_sizer = new wxBoxSizer(wxHORIZONTAL);
    const int toolbar_padding = FromDIP(8);
    const int control_gap = FromDIP(6);

    auto* home = make_icon_button(m_toolbar, "tab_home_active", _L("Home"));
    m_buttons.push_back(home);
    toolbar_sizer->Add(home, 0, wxALIGN_CENTER_VERTICAL);

    m_source_choice = new ComboBox(m_toolbar, wxID_ANY, wxEmptyString, wxDefaultPosition,
        FromDIP(wxSize(190, 32)), 0, nullptr, wxCB_READONLY);
    toolbar_sizer->Add(m_source_choice, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, control_gap);

    const wxString back_label = _L_CONTEXT("Back", "Navigation");
    const wxString forward_label = _L("Forward");
    m_back_button = make_icon_button(m_toolbar, "mall_control_back", back_label);
    m_forward_button = make_icon_button(m_toolbar, "mall_control_forward", forward_label);
    auto* reload = make_icon_button(m_toolbar, "mall_control_refresh", _L("Reload"));
    auto* external = make_icon_button(m_toolbar, "open_in_browser", _L("Open in external browser"));
    m_back_button->EnableTooltipEvenDisabled();
    m_forward_button->EnableTooltipEvenDisabled();
    m_buttons.insert(m_buttons.end(), {m_back_button, m_forward_button, reload, external});

    toolbar_sizer->Add(m_back_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    toolbar_sizer->Add(m_forward_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, control_gap);
    toolbar_sizer->Add(reload, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, control_gap);
    toolbar_sizer->Add(external, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, control_gap);
    m_manage_sources_button = make_icon_button(m_toolbar, "settings", _L("Manage sources"));
    m_buttons.push_back(m_manage_sources_button);
    toolbar_sizer->Add(m_manage_sources_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, control_gap);
    toolbar_sizer->AddStretchSpacer();

    m_downloads_button = make_icon_button(m_toolbar, "monitor_tasklist_print", _L("Downloads"));
    m_show_folder_button = make_icon_button(m_toolbar, "folder-closed", _L("Show Online Models folder"));
    m_import_button = make_icon_button(m_toolbar, "menu_load", _L("Import downloaded file"));
    m_overflow_button = make_icon_button(m_toolbar, "canvas_menu", _L("More actions"));
    m_overflow_button->Hide();
    m_buttons.insert(m_buttons.end(),
        {m_downloads_button, m_show_folder_button, m_import_button, m_overflow_button});
    toolbar_sizer->Add(m_downloads_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, control_gap);
    toolbar_sizer->Add(m_show_folder_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, control_gap);
    toolbar_sizer->Add(m_import_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, control_gap);
    toolbar_sizer->Add(m_overflow_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, control_gap);

    m_toolbar->SetSizer(toolbar_sizer);
    main_sizer->Add(m_toolbar, 0, wxEXPAND | wxALL, toolbar_padding);

    m_browser = WebView::CreateWebView(this, wxEmptyString);
    if (m_browser)
        main_sizer->Add(m_browser, 1, wxEXPAND);

    m_empty_state = new wxPanel(this, wxID_ANY);
    auto* empty_sizer = new wxBoxSizer(wxVERTICAL);
    m_empty_title = new ::Label(m_empty_state, ::Label::Head_16, _L("No model sources are enabled"));
    auto* empty_add = make_action_button(m_empty_state, _L("Add source"), "add");
    m_buttons.push_back(empty_add);
    empty_sizer->AddStretchSpacer();
    empty_sizer->Add(m_empty_title, 0, wxALIGN_CENTER_HORIZONTAL);
    empty_sizer->Add(empty_add, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(16));
    empty_sizer->AddStretchSpacer();
    m_empty_state->SetSizer(empty_sizer);
    main_sizer->Add(m_empty_state, 1, wxEXPAND);
    m_empty_state->Hide();

    m_download_popup = new PopupWindow(this, wxBORDER_SIMPLE | wxPU_CONTAINS_CONTROLS);
    auto* popup_sizer = new wxBoxSizer(wxVERTICAL);
    m_download_queue = new wxScrolledWindow(m_download_popup, wxID_ANY, wxDefaultPosition,
                                             wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL);
    m_download_queue->SetScrollRate(0, FromDIP(10));
    m_download_queue_sizer = new wxBoxSizer(wxVERTICAL);
    m_download_title = new ::Label(m_download_queue, ::Label::Head_16, _L("Downloads"));
    m_download_queue_sizer->Add(m_download_title, 0, wxALL, FromDIP(16));
    m_download_empty = new ::Label(m_download_queue, ::Label::Body_13, _L("No downloads this session"));
    m_download_queue_sizer->Add(m_download_empty, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_download_queue->SetSizer(m_download_queue_sizer);
    popup_sizer->Add(m_download_queue, 1, wxEXPAND);
    m_download_popup->SetSizer(popup_sizer);
    update_download_popup_size();

    SetSizer(main_sizer);

    home->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_on_back)
            m_on_back();
    });
    m_source_choice->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        const int selection = m_source_choice->GetSelection();
        if (selection != wxNOT_FOUND)
            load_provider(static_cast<size_t>(selection));
    });
    m_back_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_browser && m_browser->CanGoBack())
            m_browser->GoBack();
    });
    m_forward_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_browser && m_browser->CanGoForward())
            m_browser->GoForward();
    });
    reload->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_browser)
            m_browser->Reload();
    });
    external->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_open_external(); });
    m_manage_sources_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_manage_sources(); });
    empty_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_manage_sources(); });
    m_downloads_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_download_popup(); });
    m_show_folder_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_show_download_folder(); });
    m_import_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_import_downloaded_file(); });
    m_overflow_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_overflow_menu(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        update_toolbar_for_width(event.GetSize().x);
        event.Skip();
    });

    if (m_browser) {
        auto update_navigation = [this](wxWebViewEvent& event) {
            update_navigation_buttons();
            event.Skip();
        };
        m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING, update_navigation);
        m_browser->Bind(wxEVT_WEBVIEW_NAVIGATED, update_navigation);
        m_browser->Bind(wxEVT_WEBVIEW_LOADED, update_navigation);
        m_browser->Bind(wxEVT_WEBVIEW_ERROR, update_navigation);
        m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, [this](wxWebViewEvent& event) {
            const wxString url = event.GetURL();
            event.Veto();
            if (!url.empty() && is_safe_external_url(url))
                m_browser->LoadURL(url);
        });
    }
    update_navigation_buttons();
    update_toolbar_for_width(GetClientSize().x);
}

void OnlineModelsPanel::load_provider_registry()
{
    const AppConfig* config = wxGetApp().app_config;
    if (config && config->has(provider_config_section, provider_config_key)) {
        if (auto parsed = online_models_deserialize_providers(config->get(provider_config_section, provider_config_key))) {
            m_registry = std::move(*parsed);
            m_providers = online_models_enabled_providers(m_registry);
            return;
        }
    }
    m_registry = online_models_default_providers();
    m_providers = online_models_enabled_providers(m_registry);
}

void OnlineModelsPanel::save_provider_registry()
{
    if (!wxGetApp().app_config)
        return;
    wxGetApp().app_config->set(provider_config_section, provider_config_key,
                               online_models_serialize_providers(m_registry));
    wxGetApp().app_config->save();
}

void OnlineModelsPanel::refresh_provider_controls(const std::string& preferred_id)
{
    std::string active_id = preferred_id;
    if (active_id.empty() && m_current_provider < m_providers.size())
        active_id = m_providers[m_current_provider].id;
    m_providers = online_models_enabled_providers(m_registry);

    m_source_choice->Clear();
    for (const OnlineModelsProvider& provider : m_providers)
        m_source_choice->Append(wxString::FromUTF8(provider.display_name));

    const bool available = !m_providers.empty();
    m_source_choice->Enable(available);
    if (m_browser)
        m_browser->Show(available);
    if (m_empty_state)
        m_empty_state->Show(!available);
    if (!available) {
        m_current_provider = 0;
        update_navigation_buttons();
        Layout();
        return;
    }

    auto selected = std::find_if(m_providers.begin(), m_providers.end(), [&](const OnlineModelsProvider& provider) {
        return provider.id == active_id;
    });
    load_provider(selected == m_providers.end() ? 0 : static_cast<size_t>(selected - m_providers.begin()));
    Layout();
}

void OnlineModelsPanel::update_toolbar_for_width(int width)
{
    if (!m_toolbar || width <= 0)
        return;
    const bool compact = width < FromDIP(680);
    m_manage_sources_button->Show(!compact);
    m_show_folder_button->Show(!compact);
    m_import_button->Show(!compact);
    m_overflow_button->Show(compact);
    m_toolbar->Layout();
}

void OnlineModelsPanel::show_overflow_menu()
{
    wxMenu menu;
    const int manage_id = wxWindow::NewControlId();
    const int folder_id = wxWindow::NewControlId();
    const int import_id = wxWindow::NewControlId();
    menu.Append(manage_id, _L("Manage sources"));
    menu.Append(folder_id, _L("Show Online Models folder"));
    menu.Append(import_id, _L("Import downloaded file..."));
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { on_manage_sources(); }, manage_id);
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { on_show_download_folder(); }, folder_id);
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { on_import_downloaded_file(); }, import_id);
    m_toolbar->PopupMenu(&menu, m_overflow_button->GetPosition() + wxPoint(0, m_overflow_button->GetSize().y));
}

void OnlineModelsPanel::show_download_popup()
{
    if (!m_download_popup || !m_downloads_button)
        return;
    update_download_popup_size();
    m_download_popup->Position(m_downloads_button->ClientToScreen(wxPoint(0, 0)),
                               m_downloads_button->GetSize());
    m_download_popup->Popup(m_downloads_button);
}

void OnlineModelsPanel::update_download_popup_size()
{
    if (!m_download_popup || !m_download_queue)
        return;
    const int rows = static_cast<int>(m_download_rows.size());
    const int height = std::min(FromDIP(520), FromDIP(96 + rows * 112));
    m_download_popup->SetClientSize(FromDIP(wxSize(540, std::max(150, height))));
    m_download_queue->FitInside();
    m_download_popup->Layout();
}

void OnlineModelsPanel::on_page_shown()
{
    const bool available = !m_providers.empty();
    if (m_browser)
        m_browser->Show(available);
    if (m_empty_state)
        m_empty_state->Show(!available);
    Layout();
    if (GetParent())
        GetParent()->Layout();
    CallAfter([this] {
        if (!IsShownOnScreen())
            return;
        Layout();
        if (m_browser)
            m_browser->Refresh();
    });
}

void OnlineModelsPanel::on_manage_sources()
{
    const std::string active_id = m_current_provider < m_providers.size() ? m_providers[m_current_provider].id : std::string();
    SourceManagerDialog dialog(this, m_registry);
    if (dialog.ShowModal() != wxID_OK)
        return;
    m_registry = dialog.providers();
    save_provider_registry();
    refresh_provider_controls(active_id);
}

void OnlineModelsPanel::initialize_downloads()
{
    if (!m_browser)
        return;
#ifdef __WIN32__
    // wxWebViewEdge creates its ICoreWebView2 asynchronously. Register only after
    // wxEVT_WEBVIEW_CREATED when the native backend is not ready yet.
    if (!m_browser->GetNativeBackend()) {
        m_browser->Bind(wxEVT_WEBVIEW_CREATED, [this](wxWebViewEvent& event) {
            initialize_downloads();
            event.Skip();
        });
        return;
    }
#endif
    if (m_download_subscription)
        return;
    WebViewDownloadCallbacks callbacks;
    callbacks.select_destination = [this](const WebViewDownloadRequest& request) {
        return select_download_destination(request);
    };
    callbacks.on_started = [this](const WebViewDownloadRequest& request, const WebViewDownloadUpdate& update) {
        on_download_started(request, update);
    };
    callbacks.on_updated = [this](const WebViewDownloadUpdate& update) {
        on_download_updated(update);
    };
    callbacks.on_capability_unavailable = [this](const wxString&) {
        if (m_downloads_button)
            m_downloads_button->SetToolTip(
                _L("Automatic download capture is unavailable. Use the website's download controls and import the saved file."));
    };
    m_download_subscription = WebView::EnableDownloads(m_browser, std::move(callbacks));
}

std::optional<wxString> OnlineModelsPanel::select_download_destination(const WebViewDownloadRequest& request)
{
    if (m_current_provider >= m_providers.size())
        return std::nullopt;
    const OnlineModelsProvider provider = m_providers[m_current_provider];
    const std::string date = wxDateTime::Now().FormatISODate().utf8_string();
    const boost::filesystem::path directory =
        online_models_download_date_dir(online_models_download_dir(), provider, date);
    boost::system::error_code ec;
    boost::filesystem::create_directories(directory, ec);
    if (ec) {
        show_error(this, format_wxstr(_L("Failed to create the download folder: %1%"), ec.message()));
        return std::nullopt;
    }

    std::string filename = request.suggested_filename.utf8_string();
    if (filename.empty())
        filename = "download";
    boost::filesystem::path destination = online_models_unique_download_path(directory, filename);
    if (m_reserved_download_paths.count(destination.string()) != 0) {
        const boost::filesystem::path clean_name(online_models_sanitize_filename(filename));
        for (unsigned int suffix = 2;; ++suffix) {
            destination = directory / (clean_name.stem().string() + " (" + std::to_string(suffix) + ")"
                                       + clean_name.extension().string());
            if (!boost::filesystem::exists(destination) && m_reserved_download_paths.count(destination.string()) == 0)
                break;
        }
    }
    m_reserved_download_paths.insert(destination.string());
    m_download_providers[request.id] = provider;
    return from_path(destination);
}

void OnlineModelsPanel::on_download_started(const WebViewDownloadRequest& request, const WebViewDownloadUpdate& update)
{
    const auto provider = m_download_providers.find(request.id);
    if (provider == m_download_providers.end())
        return;

    auto* row = new DownloadRow;
    row->panel = new wxPanel(m_download_queue, wxID_ANY);
    row->panel->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    auto* row_sizer = new wxBoxSizer(wxVERTICAL);
    auto* heading = new wxBoxSizer(wxHORIZONTAL);
    row->name = new ::Label(row->panel, ::Label::Body_14,
        request.suggested_filename.empty() ? _L("Download") : request.suggested_filename);
    row->source = new ::Label(row->panel, ::Label::Body_13,
        wxString::FromUTF8(provider->second.display_name));
    heading->Add(row->name, 1, wxALIGN_CENTER_VERTICAL);
    heading->Add(row->source, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    row_sizer->Add(heading, 0, wxEXPAND);

    row->progress = new ProgressBar(row->panel, wxID_ANY, 100, wxDefaultPosition, FromDIP(wxSize(-1, 8)));
    row_sizer->Add(row->progress, 0, wxEXPAND | wxTOP, FromDIP(8));
    auto* lower = new wxBoxSizer(wxHORIZONTAL);
    row->status = new ::Label(row->panel, ::Label::Body_13, _L("Starting download..."));
    lower->Add(row->status, 1, wxALIGN_CENTER_VERTICAL);
    row->cancel = make_icon_button(row->panel, "notification_cancel", _L("Cancel"));
    row->open = make_icon_button(row->panel, "menu_load", _L("Open in OrcaSlicer"));
    row->show = make_icon_button(row->panel, "folder-closed", _L("Show in folder"));
    row->remove = make_icon_button(row->panel, "notification_close", _L("Remove from queue"));
    row->open->Hide();
    row->show->Hide();
    row->remove->Hide();
    for (Button* button : {row->cancel, row->open, row->show, row->remove}) {
        lower->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        m_buttons.push_back(button);
    }
    row_sizer->Add(lower, 0, wxEXPAND | wxTOP, FromDIP(8));
    row->panel->SetSizer(row_sizer);
    row->destination = into_path(update.destination);
    m_download_queue_sizer->Add(row->panel, 0, wxEXPAND | wxALL, FromDIP(16));
    m_download_rows[request.id] = row;
    m_download_empty->Hide();

    row->cancel->Bind(wxEVT_BUTTON, [this, id = request.id](wxCommandEvent&) {
        if (m_download_subscription)
            m_download_subscription->cancel(id);
    });
    row->open->Bind(wxEVT_BUTTON, [this, row](wxCommandEvent&) {
        wxArrayString paths;
        paths.Add(from_path(row->destination));
        wxGetApp().plater()->load_files(paths);
    });
    row->show->Bind(wxEVT_BUTTON, [row](wxCommandEvent&) {
        desktop_open_any_folder(row->destination.parent_path().string());
    });
    row->remove->Bind(wxEVT_BUTTON, [this, id = request.id, row](wxCommandEvent&) {
        m_download_rows.erase(id);
        m_download_providers.erase(id);
        m_reserved_download_paths.erase(row->destination.string());
        m_buttons.erase(std::remove(m_buttons.begin(), m_buttons.end(), row->cancel), m_buttons.end());
        m_buttons.erase(std::remove(m_buttons.begin(), m_buttons.end(), row->open), m_buttons.end());
        m_buttons.erase(std::remove(m_buttons.begin(), m_buttons.end(), row->show), m_buttons.end());
        m_buttons.erase(std::remove(m_buttons.begin(), m_buttons.end(), row->remove), m_buttons.end());
        row->panel->Destroy();
        delete row;
        m_download_empty->Show(m_download_rows.empty());
        m_downloads_button->SetToolTip(m_download_rows.empty()
            ? _L("Downloads")
            : format_wxstr(_L("Downloads (%1%)"), m_download_rows.size()));
        update_download_popup_size();
    });

    m_downloads_button->SetToolTip(format_wxstr(_L("Downloads (%1%)"), m_download_rows.size()));
    apply_theme();
    update_download_popup_size();
    m_download_queue->Layout();
}

void OnlineModelsPanel::on_download_updated(const WebViewDownloadUpdate& update)
{
    const auto found = m_download_rows.find(update.id);
    if (found == m_download_rows.end())
        return;
    DownloadRow* row = found->second;
    row->destination = into_path(update.destination);
    if (update.total_bytes > 0) {
        const int percentage = static_cast<int>(std::min<std::int64_t>(
            100, update.bytes_received * 100 / update.total_bytes));
        row->progress->SetValue(percentage);
    }

    const bool finished = update.state == WebViewDownloadState::Completed
        || update.state == WebViewDownloadState::Failed
        || update.state == WebViewDownloadState::Cancelled;
    row->cancel->Show(!finished);
    row->remove->Show(finished);
    row->show->Show(finished && boost::filesystem::exists(row->destination));
    row->open->Show(update.state == WebViewDownloadState::Completed
                    && online_models_is_importable_file(row->destination));

    switch (update.state) {
    case WebViewDownloadState::Started:
        row->status->SetLabel(_L("Starting download..."));
        break;
    case WebViewDownloadState::Downloading:
        row->status->SetLabel(update.total_bytes > 0
            ? format_wxstr(_L("Downloading... %1%%%"), update.bytes_received * 100 / update.total_bytes)
            : _L("Downloading..."));
        break;
    case WebViewDownloadState::Completed:
        row->status->SetLabel(online_models_is_importable_file(row->destination)
            ? _L("Download completed")
            : _L("Download completed. This file type cannot be opened in OrcaSlicer."));
        break;
    case WebViewDownloadState::Failed:
        row->status->SetLabel(update.error.empty()
            ? _L("Download failed")
            : format_wxstr(_L("Download failed: %1%"), update.error));
        break;
    case WebViewDownloadState::Cancelled:
        row->status->SetLabel(_L("Download cancelled"));
        break;
    }
    row->panel->Layout();
    m_download_queue->Layout();
    m_download_queue->FitInside();
}

void OnlineModelsPanel::apply_theme()
{
    const wxColour background = StateColor::darkModeColorFor(*wxWHITE);
    SetBackgroundColour(background);
    if (m_toolbar)
        m_toolbar->SetBackgroundColour(background);
    if (m_empty_state)
        m_empty_state->SetBackgroundColour(background);
    const wxColour popup_background = StateColor::darkModeColorFor(*wxWHITE);
    if (m_download_popup)
        m_download_popup->SetBackgroundColour(popup_background);
    if (m_download_queue)
        m_download_queue->SetBackgroundColour(popup_background);

    const wxColour primary = StateColor::darkModeColorFor(*wxBLACK);
    const wxColour secondary = StateColor::darkModeColorFor(wxColour("#6B6A6A"));
    if (m_download_empty) {
        m_download_empty->SetBackgroundColour(popup_background);
        m_download_empty->SetForegroundColour(secondary);
    }
    if (m_download_title) {
        m_download_title->SetBackgroundColour(popup_background);
        m_download_title->SetForegroundColour(primary);
    }
    if (m_empty_title)
        m_empty_title->SetForegroundColour(primary);
    for (const auto& item : m_download_rows) {
        DownloadRow* row = item.second;
        row->panel->SetBackgroundColour(popup_background);
        row->name->SetBackgroundColour(popup_background);
        row->name->SetForegroundColour(primary);
        row->source->SetBackgroundColour(popup_background);
        row->source->SetForegroundColour(secondary);
        row->status->SetBackgroundColour(popup_background);
        row->status->SetForegroundColour(secondary);
    }
    for (Button* button : m_buttons)
        button->Rescale();
    if (m_source_choice)
        m_source_choice->Rescale();
    if (m_browser)
        m_browser->SetBackgroundColour(background);
    if (m_download_popup)
        m_download_popup->Refresh();
    Refresh();
}

void OnlineModelsPanel::msw_rescale()
{
    for (Button* button : m_buttons)
        button->Rescale();
    if (m_source_choice)
        m_source_choice->Rescale();
    update_toolbar_for_width(GetClientSize().x);
    update_download_popup_size();
    Layout();
}

void OnlineModelsPanel::on_sys_color_changed()
{
    apply_theme();
    Layout();
}

void OnlineModelsPanel::load_provider(size_t index)
{
    if (index >= m_providers.size() || !m_browser)
        return;

    m_current_provider = index;
    if (m_source_choice)
        m_source_choice->SetSelection(static_cast<int>(index));
    m_back_button->Enable(false);
    m_forward_button->Enable(false);
    WebView::LoadUrl(m_browser, wxString::FromUTF8(m_providers[index].homepage_url));
}

void OnlineModelsPanel::update_navigation_buttons()
{
    m_back_button->Enable(m_browser && m_browser->CanGoBack());
    m_forward_button->Enable(m_browser && m_browser->CanGoForward());
}

bool OnlineModelsPanel::ensure_download_dir()
{
    boost::system::error_code ec;
    boost::filesystem::create_directories(Slic3r::online_models_download_dir(), ec);
    if (ec) {
        show_error(this, format_wxstr(_L("Failed to create the Online Models folder: %1%"), ec.message()));
        return false;
    }
    return true;
}

void OnlineModelsPanel::on_import_downloaded_file()
{
    if (!ensure_download_dir())
        return;
    const boost::filesystem::path download_dir = Slic3r::online_models_download_dir();
    const wxString wildcard = file_wildcards(FT_MODEL) + "|" + file_wildcards(FT_ZIP);
    wxFileDialog dialog(this, _L("Import downloaded model"), from_path(download_dir), wxEmptyString,
        wildcard, wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);

    if (dialog.ShowModal() != wxID_OK)
        return;

    wxArrayString paths;
    dialog.GetPaths(paths);
    if (!paths.empty())
        wxGetApp().plater()->load_files(paths);
}

void OnlineModelsPanel::on_show_download_folder()
{
    if (!ensure_download_dir())
        return;
    desktop_open_any_folder(Slic3r::online_models_download_dir().string());
}

void OnlineModelsPanel::on_open_external()
{
    if (!m_browser || m_current_provider >= m_providers.size())
        return;

    wxString url = m_browser->GetCurrentURL();
    if (url.empty())
        url = wxString::FromUTF8(m_providers[m_current_provider].homepage_url);
    if (!is_safe_external_url(url)) {
        show_error(this, _L("Only HTTP and HTTPS pages can be opened in an external browser."));
        return;
    }
    if (!wxLaunchDefaultBrowser(url, wxBROWSER_NEW_WINDOW))
        show_error(this, _L("Failed to open the page in an external browser."));
}

} // namespace Slic3r::GUI
