#include "slic3r/GUI/CAD/LlmPanel.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/stdpaths.h>
#include <boost/filesystem.hpp>
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
    m_attach = new wxButton(this, wxID_ANY, _L("Attach"));
    m_attach->SetToolTip(_L("Attach images, PDFs, text or CAD files (STEP, SolidWorks, Fusion...). You can also drop files here or paste an image."));
    m_view = new wxCheckBox(this, wxID_ANY, _L("Look at the viewport"));
    m_files_lbl = new wxStaticText(this, wxID_ANY, wxString());

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(m_new, 0, wxRIGHT, FromDIP(6));
    row->Add(m_status, 1, wxALIGN_CENTER_VERTICAL);
    row->Add(m_stop, 0, wxRIGHT, FromDIP(6));
    row->Add(m_send, 0);
    auto* col = new wxBoxSizer(wxVERTICAL);
    col->Add(m_log, 1, wxEXPAND | wxALL, FromDIP(6));
    col->Add(m_input, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
    auto* att = new wxBoxSizer(wxHORIZONTAL);
    att->Add(m_attach, 0, wxRIGHT, FromDIP(6));
    att->Add(m_view, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    att->Add(m_files_lbl, 1, wxALIGN_CENTER_VERTICAL);
    col->Add(att, 0, wxEXPAND | wxALL, FromDIP(6));
    col->Add(row, 0, wxEXPAND | wxALL, FromDIP(6));
    SetSizer(col);

    m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(); });
    m_files_lbl->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { m_files.clear(); refresh_attachments(); });
    m_attach->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dlg(this, _L("Attach files"), wxEmptyString, wxEmptyString, _L("All files") + "|*",
                         wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) return;
        wxArrayString paths; dlg.GetPaths(paths);
        std::vector<std::string> v;
        for (const wxString& p : paths) v.push_back(std::string(p.ToUTF8()));
        add_files(v);
    });
    struct Drop : wxFileDropTarget {
        LlmPanel* p;
        explicit Drop(LlmPanel* panel) : p(panel) {}
        bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& names) override {
            std::vector<std::string> v;
            for (const wxString& n : names) v.push_back(std::string(n.ToUTF8()));
            p->add_files(v);
            return true;
        }
    };
    SetDropTarget(new Drop(this));
    m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { stop(); });
    m_new->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { new_chat(); });
    m_input->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        if ((e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER) && !e.ShiftDown()) { send(); return; }
        if (e.GetKeyCode() == 'V' && e.ControlDown()) { paste_image(); }   // an image on the clipboard becomes an attachment; text still pastes
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
    m_attach->Enable(!busy);
    m_status->SetLabel(busy ? _L("Working...") : wxString());
    Layout();
}

void LlmPanel::add_files(const std::vector<std::string>& paths)
{
    for (const std::string& p : paths)
        if (std::find(m_files.begin(), m_files.end(), p) == m_files.end()) m_files.push_back(p);
    refresh_attachments();
}

void LlmPanel::refresh_attachments()
{
    wxString s;
    for (const std::string& f : m_files) s += (s.empty() ? "" : ", ") + wxString::FromUTF8(boost::filesystem::path(f).filename().string());
    m_files_lbl->SetLabel(s.empty() ? wxString() : s + "  (click to remove)");
    m_files_lbl->SetToolTip(_L("Click to remove the attachments"));
    Layout();
}

void LlmPanel::paste_image()
{
    if (!wxTheClipboard->Open()) return;
    wxBitmapDataObject bmp;
    wxTextDataObject   txt;
    const bool has_text = wxTheClipboard->IsSupported(wxDF_UNICODETEXT) || wxTheClipboard->IsSupported(wxDF_TEXT);
    const bool got = !has_text && wxTheClipboard->IsSupported(wxDF_BITMAP) && wxTheClipboard->GetData(bmp);
    wxTheClipboard->Close();
    if (!got) return;
    namespace fs = boost::filesystem;
    const fs::path dir = fs::temp_directory_path() / "orca-llm-attach";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / fs::unique_path("paste-%%%%-%%%%.png");
    if (bmp.GetBitmap().ConvertToImage().SaveFile(wxString::FromUTF8(file.string()), wxBITMAP_TYPE_PNG))
        add_files({file.string()});
}

void LlmPanel::new_chat()
{
    m_session_id.clear();
    m_files.clear();
    refresh_attachments();
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
    // Attachments ride in the prompt as paths: the agent reads them (images, PDFs, text) or imports them (CAD files).
    std::string prompt(text.ToUTF8());
    std::vector<std::string> dirs;
    if (!m_files.empty()) {
        prompt += "\n\nAttached files. Read images, PDFs and text with the Read tool; a CAD file (STEP, SolidWorks, Fusion, Inventor, FreeCAD, Rhino, IGES...) is imported with the orca-cad import_step tool, giving it the path:";
        for (const std::string& f : m_files) {
            prompt += "\n- " + f;
            const std::string d = boost::filesystem::path(f).parent_path().string();
            if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
        }
        append("  [" + wxString::Format(_L("%d attached"), int(m_files.size())) + "]\n", wxColour(110, 110, 110));
    }
    if (m_view->GetValue()) prompt += "\n\nFirst look at the current viewport with the orca-cad screenshot tool.";
    m_files.clear();
    refresh_attachments();

    std::weak_ptr<bool> alive = m_alive;
    m_run = llm_agent_start(prompt, m_session_id, dirs,
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
