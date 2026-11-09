#include "slic3r/GUI/CAD/LlmPanel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/CAD/LlmApi.hpp"
#include "slic3r/GUI/Preferences.hpp"
#include "libslic3r/AppConfig.hpp"
#include <regex>
#include <nlohmann/json.hpp>

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

namespace {
// One line per tool call, in words: "create_box" -> "Create box". opencode wraps calls in JavaScript, so the verb is read out of
// `tools["orca-cad"].verb(`; Claude Code sends "verb  {json}". Empty = plumbing (catalogue lookups), not shown.
std::vector<std::string> action_names(const std::string& raw)
{
    std::vector<std::string> v;
    if (raw.rfind("execute", 0) == 0 || raw.rfind("search", 0) == 0) {
        static const std::regex re("orca-cad\"\\]\\.([A-Za-z0-9_]+)\\(");
        for (std::sregex_iterator it(raw.begin(), raw.end(), re), end; it != end; ++it) v.push_back((*it)[1]);
    } else if (const size_t sp = raw.find("  "); !raw.empty()) v.push_back(raw.substr(0, sp));
    return v;
}
wxString words(std::string n)
{
    std::replace(n.begin(), n.end(), '_', ' ');
    if (!n.empty()) n[0] = char(std::toupper((unsigned char) n[0]));
    return wxString::FromUTF8(n);
}
bool is_read_only(const std::string& n)
{
    for (const char* p : {"inspect", "describe", "list", "get_", "screenshot", "measure", "mass_", "query", "find", "check", "ui_"})
        if (n.rfind(p, 0) == 0) return true;
    return false;
}
} // namespace

LlmPanel::LlmPanel(wxWindow* parent, std::function<size_t()> undo_depth, std::function<void(size_t)> drop_undo_to, std::function<void()> undo_turn)
    : wxPanel(parent, wxID_ANY)
    , m_undo_depth(std::move(undo_depth))
    , m_drop_undo_to(std::move(drop_undo_to))
    , m_undo_turn(std::move(undo_turn))
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
    m_agent_lbl = new wxStaticText(this, wxID_ANY, wxString());
    m_agent_lbl->SetFont(m_agent_lbl->GetFont().Bold());
    m_setup_lbl = new wxStaticText(this, wxID_ANY, wxString());
    m_setup_lbl->SetForegroundColour(wxColour(190, 40, 40));
    m_gear = new wxButton(this, wxID_ANY, _L("Settings"));
    m_gear->SetToolTip(_L("Assistant settings (Preferences > LLM / MCP)"));
    m_raw = new wxCheckBox(this, wxID_ANY, _L("Raw calls"));
    m_raw->SetToolTip(_L("Show the exact tool call under each action"));
    // Empty state: three prompts that work, one click to fill the box.
    m_examples = new wxPanel(this);
    auto* ex = new wxBoxSizer(wxVERTICAL);
    ex->Add(new wxStaticText(m_examples, wxID_ANY, _L("Try asking:")), 0, wxBOTTOM, FromDIP(4));
    for (const wxString& t : {_L("Make a 20 mm cube and fillet the top edges by 2 mm"),
                              _L("What is the volume and the bounding box of this part?"),
                              _L("Add a 6 mm hole through the centre of the top face")}) {
        auto* b = new wxButton(m_examples, wxID_ANY, t, wxDefaultPosition, wxDefaultSize, wxBU_LEFT);
        b->Bind(wxEVT_BUTTON, [this, t](wxCommandEvent&) { m_input->SetValue(t); m_input->SetFocus(); m_input->SetInsertionPointEnd(); });
        ex->Add(b, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    }
    m_examples->SetSizer(ex);
    // End of turn: how many edits it made, and one button to take them all back.
    m_result = new wxPanel(this);
    auto* rs = new wxBoxSizer(wxHORIZONTAL);
    m_result_lbl = new wxStaticText(m_result, wxID_ANY, wxString());
    auto* undo = new wxButton(m_result, wxID_ANY, _L("Undo this turn"));
    rs->Add(m_result_lbl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    rs->Add(undo, 0);
    m_result->SetSizer(rs);
    m_result->Hide();
    undo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_result->Hide(); if (m_undo_turn) m_undo_turn(); Layout(); });

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(m_new, 0, wxRIGHT, FromDIP(6));
    row->Add(m_status, 1, wxALIGN_CENTER_VERTICAL);
    row->Add(m_stop, 0, wxRIGHT, FromDIP(6));
    row->Add(m_send, 0);
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    head->Add(m_raw, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    head->Add(m_gear, 0);
    auto* col = new wxBoxSizer(wxVERTICAL);
    col->Add(m_agent_lbl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    col->Add(head, 0, wxEXPAND | wxALL, FromDIP(6));
    col->Add(m_setup_lbl, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
    col->Add(m_examples, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    col->Add(m_log, 1, wxEXPAND | wxALL, FromDIP(6));
    col->Add(m_result, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
    col->Add(m_input, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
    auto* att = new wxBoxSizer(wxHORIZONTAL);
    att->Add(m_attach, 0, wxRIGHT, FromDIP(6));
    att->Add(m_view, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    att->Add(m_files_lbl, 1, wxALIGN_CENTER_VERTICAL);
    col->Add(att, 0, wxEXPAND | wxALL, FromDIP(6));
    col->Add(row, 0, wxEXPAND | wxALL, FromDIP(6));
    SetSizer(col);

    m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(); });
    m_gear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { wxGetApp().open_preferences(PreferencesTab::Llm); refresh_header(); });
    Bind(wxEVT_SHOW, [this](wxShowEvent& e) { if (e.IsShown()) refresh_header(); e.Skip(); });
    refresh_header();
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
    if (!m_restoring) m_hist.push_back({text, colour.GetRGB(), bold});
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

void LlmPanel::refresh_header()
{
    AppConfig* c = wxGetApp().app_config;
    const std::string agent = c ? c->get("llm_agent") : std::string();
    const bool oc = agent == "opencode";
    const bool ag = agent == "agy";
    const std::string model = c ? c->get("llm_model") : std::string();
    m_agent_lbl->SetLabel(wxString::FromUTF8(agent == "anthropic" ? "Anthropic API" : agent == "openai" ? "OpenAI-compatible API" : agent == "ollama" ? "Ollama" : oc ? "opencode" : ag ? "Antigravity" : "Claude Code") + (model.empty() ? wxString() : wxString::FromUTF8(" \xC2\xB7 ") + wxString::FromUTF8(model)));
    wxString why;
    if (llm_agent_is_api(agent)) {
        if (agent != "ollama" && !llm_api_key_present(agent)) why = _L("No API key. Open the settings to set it.");
        else if (model.empty()) why = _L("Choose a model. Open the settings.");
    } else if (llm_agent_executable().empty()) why = oc ? _L("opencode was not found. Open the settings to point to it.") : ag ? _L("agy was not found. Open the settings to point to it.") : _L("Claude Code was not found. Open the settings to point to it.");
    else if (oc && model.empty()) why = _L("opencode needs a model (provider/model). Open the settings.");
    m_setup_lbl->SetLabel(why);
    m_setup_lbl->Show(!why.empty());
    m_setup_lbl->Wrap(std::max(FromDIP(200), GetClientSize().x - FromDIP(16)));
    Layout();
}

void LlmPanel::show_examples(bool on) { m_examples->Show(on); Layout(); }

void LlmPanel::on_tool(const std::string& raw)
{
    const wxColour grey(110, 110, 110);
    for (const std::string& n : action_names(raw)) {
        if (!is_read_only(n)) ++m_turn_edits;
        append(wxString::FromUTF8("  \xE2\x80\xA2 ") + words(n) + "\n", is_read_only(n) ? wxColour(150, 150, 150) : grey);
        m_status->SetLabel(words(n) + "...");
    }
    if (m_raw->GetValue()) append(wxString("      ") + wxString::FromUTF8(raw) + "\n", wxColour(160, 160, 160));
}

void LlmPanel::save_chat()
{
    AppConfig* c = wxGetApp().app_config;
    std::string out;
    if (!(c && c->get("llm_no_chat_save") == "1") && !m_hist.empty()) {
        // Keep the tail: a long session must not bloat every save of the project.
        size_t total = 0, from = m_hist.size();
        while (from > 0 && total < 200000) total += m_hist[--from].text.length();
        nlohmann::json j = nlohmann::json::array();
        for (size_t i = from; i < m_hist.size(); ++i) j.push_back({std::string(m_hist[i].text.ToUTF8()), m_hist[i].rgb, m_hist[i].bold});
        out = j.dump();
    }
    m_chat_json = out;
    if (m_chat_sink) m_chat_sink(out);
}

void LlmPanel::load_chat(const std::string& json)
{
    if (m_busy || json == m_chat_json) return;   // the project we already show (this runs every time the tab is shown)
    m_chat_json = json;
    m_hist.clear();
    m_log->Clear();
    m_session_id.clear();   // an agent session belongs to one conversation on one machine
    m_result->Hide();
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    m_restoring = true;
    if (j.is_array())
        for (const auto& e : j)
            if (e.is_array() && e.size() == 3 && e[0].is_string() && e[1].is_number_unsigned() && e[2].is_boolean()) {
                const unsigned long rgb = e[1].get<unsigned long>();
                append(wxString::FromUTF8(e[0].get<std::string>()), wxColour(rgb), e[2].get<bool>());
                m_hist.push_back({wxString::FromUTF8(e[0].get<std::string>()), rgb, e[2].get<bool>()});
            }
    m_restoring = false;
    show_examples(m_hist.empty());
}

void LlmPanel::new_chat()
{
    m_session_id.clear();
    m_hist.clear();
    m_result->Hide();
    show_examples(true);
    m_files.clear();
    refresh_attachments();
    m_log->Clear();
    save_chat();
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
    m_turn_edits = 0;
    m_result->Hide();
    show_examples(false);
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
            case LlmEvent::Tool:   on_tool(e.text); break;
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
            save_chat();
            if (m_undo_depth() > m_undo_at_start) {   // agy streams no tool events, so the undo stack is the source of truth
                m_result_lbl->SetLabel(m_turn_edits > 0 ? wxString::Format(_L("%d edits in this turn"), m_turn_edits) : _L("This turn changed the design"));
                m_result->Show();
                Layout();
            }
            m_input->SetFocus();
        });
}

}} // namespace Slic3r::GUI
