#ifndef slic3r_GUI_LlmPanel_hpp_
#define slic3r_GUI_LlmPanel_hpp_

// The Design tab's assistant chat: a wxAUI pane (dock it, or drag it out as a floating window) that drives the local agent
// CLI through llm_agent_start. docs/CAD/SPEC-llm-panel.md

#include "slic3r/GUI/CAD/LlmAgent.hpp"

#include <wx/panel.h>

#include <functional>
#include <memory>
#include <string>

class wxButton;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r { namespace GUI {

class LlmPanel : public wxPanel
{
public:
    // `undo_depth` / `drop_undo_to` let a turn's edits collapse into ONE undo step (the document's snapshot stack).
    LlmPanel(wxWindow* parent, std::function<size_t()> undo_depth, std::function<void(size_t)> drop_undo_to);
    ~LlmPanel() override;

    void focus_input();

private:
    void send();
    void stop();
    void new_chat();
    void append(const wxString& text, const wxColour& colour, bool bold = false);
    void set_busy(bool busy);

    wxTextCtrl*   m_log    = nullptr;
    wxTextCtrl*   m_input  = nullptr;
    wxButton*     m_send   = nullptr;
    wxButton*     m_stop   = nullptr;
    wxButton*     m_new    = nullptr;
    wxStaticText* m_status = nullptr;

    std::function<size_t()>     m_undo_depth;
    std::function<void(size_t)> m_drop_undo_to;
    std::shared_ptr<LlmAgentRun> m_run;
    std::string  m_session_id;
    size_t       m_undo_at_start = 0;
    bool         m_busy          = false;
    // Callbacks outlive the panel only as long as this flag: a turn that finishes after the panel is gone does nothing.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

}} // namespace Slic3r::GUI

#endif
