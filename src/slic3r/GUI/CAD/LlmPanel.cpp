#include "slic3r/GUI/CAD/LlmPanel.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace Slic3r { namespace GUI {

LlmPanel::LlmPanel(wxWindow* parent, std::function<size_t()> undo_depth, std::function<void(size_t)> drop_undo_to)
    : wxPanel(parent, wxID_ANY)
    , m_undo_depth(std::move(undo_depth))
    , m_drop_undo_to(std::move(drop_undo_to))
{
    m_log = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition, wxSize(FromDIP(320), FromDIP(200)),
                           wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_WORDWRAP | wxBORDER_SIMPLE);
    m_input = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition, wxSize(-1, FromDIP(64)),
                             wxTE_MULTILINE | wxTE_PROCESS_ENTER | wxTE_WORDWRAP);
    m_input->SetHint(_L("Ask the assistant to build or change the design. Enter sends, Shift+Enter adds a line."));
    m_send = new wxButton(this, wxID_ANY, _L("Send"));
    m_stop = new wxButton(this, wxID_ANY, _L("Stop"));
    m_new  = new wxButton(this, wxID_ANY, _L("New chat"));
    m_status = new wxStaticText(this, wxID_ANY, wxString());
    m_stop->Hide();

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(m_new, 0, wxRIGHT, FromDIP(6));
    row->Add(m_status, 1, wxALIGN_CENTER_VERTICAL);
    row->Add(m_stop, 0, wxRIGHT, FromDIP(6));
    row->Add(m_send, 0);
    auto* col = new wxBoxSizer(wxVERTICAL);
    col->Add(m_log, 1, wxEXPAND | wxALL, FromDIP(6));
    col->Add(m_input, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
    col->Add(row, 0, wxEXPAND | wxALL, FromDIP(6));
    SetSizer(col);

    m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(); });
    m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { stop(); });
    m_new->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { new_chat(); });
    m_input->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        if ((e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER) && !e.ShiftDown()) { send(); return; }
        e.Skip();
    });
}

LlmPanel::~LlmPanel()
{
    *m_alive = false;
    if (m_run) m_run->cancel();
}

void LlmPanel::focus_input() { m_input->SetFocus(); }

void LlmPanel::append(const wxString& text, const wxColour& colour, bool bold)
{
    wxTextAttr attr;
    attr.SetTextColour(colour);
    wxFont f = m_log->GetFont();
    if (bold) f.SetWeight(wxFONTWEIGHT_BOLD);
    attr.SetFont(f);
    m_log->SetDefaultStyle(attr);
    m_log->AppendText(text);
    m_log->ShowPosition(m_log->GetLastPosition());
}

void LlmPanel::set_busy(bool busy)
{
    m_busy = busy;
    m_send->Show(!busy);
    m_stop->Show(busy);
    m_input->Enable(!busy);
    m_new->Enable(!busy);
    m_status->SetLabel(busy ? _L("Working...") : wxString());
    Layout();
}

void LlmPanel::new_chat()
{
    m_session_id.clear();
    m_log->Clear();
}

void LlmPanel::stop()
{
    if (m_run) m_run->cancel();
}

void LlmPanel::send()
{
    if (m_busy) return;
    const wxString text = m_input->GetValue().Trim().Trim(false);
    if (text.IsEmpty()) return;
    m_input->Clear();
    append(_L("You") + ": ", wxColour(0, 122, 116), true);
    append(text + "\n", m_log->GetForegroundColour());
    m_undo_at_start = m_undo_depth();
    set_busy(true);

    std::weak_ptr<bool> alive = m_alive;
    m_run = llm_agent_start(std::string(text.ToUTF8()), m_session_id,
        [this, alive](const LlmEvent& e) {
            if (alive.expired() || !*alive.lock()) return;
            switch (e.kind) {
            case LlmEvent::Text:   append(wxString::FromUTF8(e.text) + "\n", m_log->GetForegroundColour()); break;
            case LlmEvent::Tool:   append(wxString("  > ") + wxString::FromUTF8(e.text) + "\n", wxColour(110, 110, 110)); break;
            case LlmEvent::Result:
                if (!e.session_id.empty()) m_session_id = e.session_id;
                if (e.error) append(wxString::FromUTF8(e.text) + "\n", wxColour(190, 40, 40));
                break;
            }
        },
        [this, alive](bool ok, const std::string& err) {
            if (alive.expired() || !*alive.lock()) return;
            // One turn = one undo step: keep the snapshot taken before the turn's first edit, drop the rest.
            if (m_undo_depth() > m_undo_at_start + 1) m_drop_undo_to(m_undo_at_start + 1);
            if (!ok) append(wxString::FromUTF8(err) + "\n", wxColour(190, 40, 40));
            append("\n", m_log->GetForegroundColour());
            m_run.reset();
            set_busy(false);
            m_input->SetFocus();
        });
}

}} // namespace Slic3r::GUI
