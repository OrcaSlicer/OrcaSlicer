#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <wx/panel.h>

#include "libslic3r/OnlineModels.hpp"

class Button;
class ComboBox;
class Label;
class wxWebView;
class wxBoxSizer;
class wxScrolledWindow;
class PopupWindow;
class WebViewDownloadSubscription;
struct WebViewDownloadRequest;
struct WebViewDownloadUpdate;

namespace Slic3r::GUI {

class OnlineModelsPanel : public wxPanel
{
public:
    OnlineModelsPanel(wxWindow* parent, std::function<void()> on_back);
    ~OnlineModelsPanel() override;

    void msw_rescale();
    void on_sys_color_changed();
    void on_page_shown();

private:
    void create_ui();
    void apply_theme();
    void load_provider_registry();
    void save_provider_registry();
    void refresh_provider_controls(const std::string& preferred_id = {});
    void load_provider(size_t index);
    void on_manage_sources();
    void update_navigation_buttons();
    void update_toolbar_for_width(int width);
    void show_overflow_menu();
    void show_download_popup();
    void update_download_popup_size();
    bool ensure_download_dir();
    void on_import_downloaded_file();
    void on_show_download_folder();
    void on_open_external();
    void initialize_downloads();
    std::optional<wxString> select_download_destination(const WebViewDownloadRequest& request);
    void on_download_started(const WebViewDownloadRequest& request, const WebViewDownloadUpdate& update);
    void on_download_updated(const WebViewDownloadUpdate& update);

    std::function<void()>                    m_on_back;
    std::vector<OnlineModelsProvider>        m_registry;
    std::vector<OnlineModelsProvider>        m_providers;
    size_t                                   m_current_provider { 0 };
    ComboBox*                                m_source_choice { nullptr };
    wxPanel*                                 m_toolbar { nullptr };
    PopupWindow*                             m_download_popup { nullptr };
    wxScrolledWindow*                        m_download_queue { nullptr };
    wxBoxSizer*                              m_download_queue_sizer { nullptr };
    Label*                                   m_download_title { nullptr };
    Label*                                   m_download_empty { nullptr };
    wxWebView*                               m_browser { nullptr };
    wxPanel*                                 m_empty_state { nullptr };
    Label*                                   m_empty_title { nullptr };
    Button*                                  m_manage_sources_button { nullptr };
    Button*                                  m_downloads_button { nullptr };
    Button*                                  m_show_folder_button { nullptr };
    Button*                                  m_import_button { nullptr };
    Button*                                  m_overflow_button { nullptr };
    Button*                                  m_back_button { nullptr };
    Button*                                  m_forward_button { nullptr };
    std::vector<Button*>                     m_buttons;
    struct DownloadRow;
    std::map<std::string, DownloadRow*>       m_download_rows;
    std::map<std::string, OnlineModelsProvider> m_download_providers;
    std::set<std::string>                     m_reserved_download_paths;
    std::unique_ptr<WebViewDownloadSubscription> m_download_subscription;
};

} // namespace Slic3r::GUI
