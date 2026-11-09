#ifndef slic3r_GUI_LlmPanel_hpp_
#define slic3r_GUI_LlmPanel_hpp_

// The Design tab's assistant chat: a wxAUI pane (dock it, or drag it out as a floating window) that drives the local agent
// CLI through llm_agent_start. docs/CAD/SPEC-llm-panel.md

#include "slic3r/GUI/CAD/LlmAgent.hpp"

#include <wx/panel.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <utility>
#include <wx/colour.h>
#include <wx/string.h>

class wxButton;
class wxCheckBox;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r { namespace GUI {

class LlmPanel : public wxPanel
{
public:
    // `undo_depth` / `drop_undo_to` let a turn's edits collapse into ONE undo step (the document's snapshot stack).
    // `undo_turn` takes back the last undo step, which after a turn is the whole turn.
    LlmPanel(wxWindow* parent, std::function<size_t()> undo_depth, std::function<void(size_t)> drop_undo_to, std::function<void()> undo_turn);
    ~LlmPanel() override;

    void focus_input();
    // The chat as JSON for the project file (empty when the user chose not to save it), and the way back in.
    void set_chat_sink(std::function<void(const std::string&)> sink) { m_chat_sink = std::move(sink); }
    void load_chat(const std::string& json);

private:
    void send();
    void stop();
    void new_chat();
    void append(const wxString& text, const wxColour& colour, bool bold = false);
    void set_busy(bool busy);
    void add_files(const std::vector<std::string>& paths);
    void paste_image();
    void refresh_attachments();
    void refresh_header();   // agent + model, ready dot, setup hint
    void show_examples(bool on);
    void on_tool(const std::string& raw);
    void save_chat();

    wxTextCtrl*   m_log    = nullptr;
    wxTextCtrl*   m_input  = nullptr;
    wxButton*     m_send   = nullptr;
    wxButton*     m_stop   = nullptr;
    wxButton*     m_new    = nullptr;
    wxButton*     m_attach = nullptr;
    wxCheckBox*   m_view   = nullptr;
    wxStaticText* m_files_lbl = nullptr;
    std::vector<std::string> m_files;   // attached files for the next message
    wxStaticText* m_status = nullptr;
    wxStaticText* m_agent_lbl = nullptr;   // header: "opencode · model"
    wxStaticText* m_setup_lbl = nullptr;   // header: why the agent is not ready
    wxButton*     m_gear      = nullptr;
    wxCheckBox*   m_raw       = nullptr;   // show the raw tool call under each action
    wxPanel*      m_examples  = nullptr;   // empty state: click-to-fill prompts
    wxPanel*      m_result    = nullptr;   // end of turn: "N edits" + Undo this turn
    wxStaticText* m_result_lbl = nullptr;
    int           m_turn_edits = 0;
    struct Line { wxString text; unsigned long rgb; bool bold; };
    std::vector<Line> m_hist;            // what the log shows, so it can be saved and restored
    bool          m_restoring = false;
    std::string   m_chat_json;           // last chat handed to / received from the project
    std::function<void(const std::string&)> m_chat_sink;

    std::function<size_t()>     m_undo_depth;
    std::function<void(size_t)> m_drop_undo_to;
    std::function<void()>       m_undo_turn;
    std::shared_ptr<LlmAgentRun> m_run;
    std::string  m_session_id;
    size_t       m_undo_at_start = 0;
    bool         m_busy          = false;
    // Callbacks outlive the panel only as long as this flag: a turn that finishes after the panel is gone does nothing.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

}} // namespace Slic3r::GUI

#endif
