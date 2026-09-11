#ifndef slic3r_GUI_SpeedDialDialog_hpp_
#define slic3r_GUI_SpeedDialDialog_hpp_

#include <slic3r/GUI/Widgets/WebViewHostDialog.hpp>
#include <nlohmann/json_fwd.hpp>
#include <atomic>
#include <memory>
#include <string>

namespace Slic3r { namespace GUI {

class SpeedDialWebDialog : public WebViewHostDialog
{
public:
    explicit SpeedDialWebDialog(wxWindow* parent);
    ~SpeedDialWebDialog() override;
    void request_show();

private:
    void on_script_message(const nlohmann::json& payload) override;
    void handle_web_command(const nlohmann::json& payload);
    void resize_to_content(int height);
    void run_action(const std::string& id, const std::string& title);
    void send_actions();

    bool m_page_ready{false};
    // Guards the CallAfter in on_script_message across dialog destruction, same as
    // PluginsDialog::m_alive (PluginsDialog.hpp:249).
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};

}}

#endif
