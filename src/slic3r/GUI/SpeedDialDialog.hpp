#ifndef slic3r_GUI_SpeedDialDialog_hpp_
#define slic3r_GUI_SpeedDialDialog_hpp_

#include <slic3r/GUI/Widgets/WebViewHostDialog.hpp>
#include <nlohmann/json_fwd.hpp>
#include <atomic>
#include <memory>
#include <string>

#include <wx/bitmap.h>

namespace Slic3r { namespace GUI {

class SpeedDialWebDialog : public WebViewHostDialog
{
public:
    explicit SpeedDialWebDialog(wxWindow* parent);
    ~SpeedDialWebDialog() override;
    void request_show();

private:
    void add_user_scripts() override;
    void on_script_message(const nlohmann::json& payload) override;
    void handle_web_command(const nlohmann::json& payload);
    void resize_to_content(int height);
    void run_action(const std::string& id, const std::string& title, const std::string& param = "");
    void open_wiki(const std::string& id);
    void send_actions();
    void search_tabs();
    void apply_rounded_shape();
    void on_dpi_changed(const wxRect& suggested_rect) override;

    bool m_page_ready{false};
    // Rounded corners via a window shape region, since the webview itself is opaque.
    int m_corner_radius{7};
    wxBitmap m_shape_bmp;
    // Guards the CallAfter in on_script_message across dialog destruction, same as
    // PluginsDialog::m_alive (PluginsDialog.hpp:249).
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};

}}

#endif
