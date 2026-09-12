#ifndef slic3r_AISlicerDialog_hpp_
#define slic3r_AISlicerDialog_hpp_

#include <string>
#include <vector>

#include "GUI_Utils.hpp"

class Button;
class wxTextCtrl;
class wxStaticText;
class wxNotebook;

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// AISlicerDialog
//
// The user-facing "AI" workspace. Increment 1: a "Generate shape" tab that
// sends the prompt + live machine context (AIPrinterContext) to the configured
// AIProvider, turns the reply into geometry (AIShapeGen), and drops it on the
// plate. A "Search & Import" tab is added in a later increment.
// ---------------------------------------------------------------------------
class AISlicerDialog : public DPIDialog
{
public:
    explicit AISlicerDialog(wxWindow *parent);
    ~AISlicerDialog() override = default;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    wxWindow *build_generate_tab(wxNotebook *nb);
    wxWindow *build_search_tab(wxNotebook *nb);
    void      on_generate();
    // Open the query on a model repository's own search page in the browser.
    void      open_model_search(const std::string &url_prefix, const std::string &url_suffix);
    void      set_status(const wxString &msg, bool error = false);
    void      set_search_status(const wxString &msg, bool error = false);

    // Generate tab
    wxTextCtrl   *m_prompt       { nullptr };
    Button       *m_generate_btn { nullptr };
    wxStaticText *m_status       { nullptr };

    // Search tab
    wxTextCtrl   *m_query         { nullptr };
    wxStaticText *m_search_status { nullptr };
};

}} // namespace Slic3r::GUI

#endif // slic3r_AISlicerDialog_hpp_
