#ifndef slic3r_AISettingsDialog_hpp_
#define slic3r_AISettingsDialog_hpp_

#include "GUI_Utils.hpp"

class ComboBox;
class Button;
class wxTextCtrl;
class wxStaticText;

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// AISettingsDialog
//
// Registers the AI provider + gateway/API key/model that the AIProvider factory
// reads back from the "ai_slicer" AppConfig section. Lets the user Test the
// connection and Fetch the available model list before saving.
// ---------------------------------------------------------------------------
class AISettingsDialog : public DPIDialog
{
public:
    explicit AISettingsDialog(wxWindow *parent);
    ~AISettingsDialog() override = default;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void load_from_config();
    void save_to_config();
    void on_provider_changed();
    void on_test();
    void on_fetch_models();
    void set_status(const wxString &msg, bool error = false);

    ComboBox     *m_provider    { nullptr };
    wxTextCtrl   *m_api_key     { nullptr };
    wxTextCtrl   *m_gateway_url { nullptr };
    ComboBox     *m_model       { nullptr };
    Button       *m_test_btn    { nullptr };
    Button       *m_fetch_btn   { nullptr };
    wxStaticText *m_status      { nullptr };
    wxStaticText *m_gateway_hint{ nullptr };
};

}} // namespace Slic3r::GUI

#endif // slic3r_AISettingsDialog_hpp_
