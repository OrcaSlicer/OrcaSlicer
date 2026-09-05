#include "AISettingsDialog.hpp"

#include <memory>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/Utils/AIProvider.hpp"
#include "slic3r/Utils/AIShapeGen.hpp"

namespace Slic3r { namespace GUI {

// Provider dropdown order <-> AppConfig provider key.
static std::string provider_key_for_index(int idx)
{
    switch (idx) {
        case 0:  return "openai";
        case 1:  return "anthropic";
        case 2:  return "compatible";
        default: return "";
    }
}

static int index_for_provider_key(const std::string &key)
{
    if (key == "anthropic")                                return 1;
    if (key == "compatible" || key == "openai_compatible") return 2;
    return 0; // default: openai
}

// Qualification test: force a create_model tool call on a canonical prompt and
// see whether the model returns a buildable shape. This is how we tell a model
// suitable for 3D generation from one that merely talks. Fills `msg` either way.
static bool run_qualification(const AIConfig &cfg, wxString &msg)
{
    std::unique_ptr<AIProvider> provider(AIProvider::create(cfg));
    if (! provider) { msg = _L("Configure a provider, API key, and model first."); return false; }

    nlohmann::json tool = {
        {"type", "function"},
        {"function", {
            {"name", "create_model"},
            {"description", "Create a printable 3D shape as a parametric tree. Never explain in prose. "
                            "Node \"type\": box{size:[x,y,z]}, cylinder{radius,height}, cone{radius,height}, "
                            "sphere{radius}, or union|difference|intersection{children:[...]}."},
            {"parameters", {{"type", "object"},
                            {"properties", {{"shape", {{"type", "object"}}}}},
                            {"required", nlohmann::json::array({"shape"})}}}
        }}
    };
    nlohmann::json params;
    params["tools"]       = nlohmann::json::array({tool});
    params["tool_choice"] = {{"type", "function"}, {"function", {{"name", "create_model"}}}};
    params["temperature"] = 0.1;

    std::vector<AIMessage> msgs;
    msgs.push_back({"system", "You design printable 3D shapes by calling create_model."});
    msgs.push_back({"user", "a 30mm cube with a 10mm hole through the center"});

    const auto t0 = std::chrono::steady_clock::now();
    AIResponse resp = provider->chat(msgs, params);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (! resp.ok) {
        msg = _L("Not suitable — request failed: ") + from_u8(resp.error);
        return false;
    }
    if (resp.tool_call_arguments.empty()) {
        msg = _L("Not suitable — the model replied with text instead of a create_model tool call.");
        return false;
    }
    Model m;
    std::string err;
    if (! ai_build_model_from_tool_call(resp.tool_call_arguments, "qualify", m, err) || m.objects.empty()) {
        msg = _L("Returned a tool call, but its shape wasn't buildable: ") + from_u8(err);
        return false;
    }
    msg = wxString::Format(_L("Qualified for 3D generation (%.1fs). This model works."), secs);
    return true;
}

AISettingsDialog::AISettingsDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("AI Settings"), wxDefaultPosition,
                wxSize(46 * wxGetApp().em_unit(), -1), wxDEFAULT_DIALOG_STYLE)
{
    SetFont(wxGetApp().normal_font());
    SetBackgroundColour(*wxWHITE);

    const int em     = wxGetApp().em_unit();
    const wxSize ctl = wxSize(FromDIP(300), -1);

    auto *grid = new wxFlexGridSizer(0, 2, FromDIP(8), FromDIP(10));
    grid->AddGrowableCol(1, 1);

    auto add_row = [&](const wxString &label, wxWindow *ctrl) {
        auto *st = new wxStaticText(this, wxID_ANY, label);
        grid->Add(st, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND);
    };

    m_provider = new ComboBox(this, wxID_ANY, "", wxDefaultPosition, ctl, 0, nullptr, wxCB_READONLY);
    m_provider->Append(_L("OpenAI"));
    m_provider->Append(_L("Anthropic"));
    m_provider->Append(_L("OpenAI-compatible (custom gateway)"));
    add_row(_L("Provider"), m_provider);

    m_api_key = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, ctl, wxTE_PASSWORD);
    add_row(_L("API key"), m_api_key);

    m_gateway_url = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, ctl);
    add_row(_L("Gateway URL"), m_gateway_url);

    m_model = new ComboBox(this, wxID_ANY, "", wxDefaultPosition, ctl, 0, nullptr, 0);
    add_row(_L("Model"), m_model);

    // Test / Fetch / Qualify buttons.
    m_test_btn  = new Button(this, _L("Test connection"));
    m_fetch_btn = new Button(this, _L("Fetch models"));
    auto *qualify_btn = new Button(this, _L("Qualify for 3D"));
    qualify_btn->SetToolTip(_L("Check whether the selected model can actually generate 3D shapes."));
    auto *btn_row = new wxBoxSizer(wxHORIZONTAL);
    btn_row->Add(m_test_btn, 0, wxRIGHT, FromDIP(8));
    btn_row->Add(m_fetch_btn, 0, wxRIGHT, FromDIP(8));
    btn_row->Add(qualify_btn, 0);

    m_gateway_hint = new wxStaticText(this, wxID_ANY,
        _L("Gateway URL is used only for an OpenAI-compatible provider (Ollama, LiteLLM, a proxy...)."));
    auto *rec_hint = new wxStaticText(this, wxID_ANY,
        _L("Model tip: use a mid/large instruct or code model (e.g. a llama-3.x-instruct or qwen-*-instruct). "
           "Avoid embedding, reranker, and tiny models. Click \"Qualify for 3D\" to test the current one."));
    m_status = new wxStaticText(this, wxID_ANY, "");

    // Assemble.
    auto *top = new wxBoxSizer(wxVERTICAL);
    top->Add(grid,           0, wxEXPAND | wxALL, FromDIP(16));
    top->Add(m_gateway_hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    top->Add(rec_hint,       0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    top->Add(btn_row,        0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    top->Add(m_status,       0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(16));

    auto *dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    top->Add(dlg_btns, 0, wxEXPAND);

    // Wiring.
    m_provider->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { on_provider_changed(); });
    m_test_btn->Bind(wxEVT_BUTTON,   [this](wxCommandEvent &) { on_test(); });
    m_fetch_btn->Bind(wxEVT_BUTTON,  [this](wxCommandEvent &) { on_fetch_models(); });
    qualify_btn->Bind(wxEVT_BUTTON,  [this](wxCommandEvent &) {
        AIConfig cfg;
        cfg.provider = provider_key_for_index(m_provider->GetSelection());
        cfg.api_key  = into_u8(m_api_key->GetValue());
        cfg.base_url = into_u8(m_gateway_url->GetValue());
        cfg.model    = into_u8(m_model->GetValue());
        set_status(_L("Qualifying the selected model for 3D (this runs a real generation)..."));
        wxString msg; bool ok;
        { wxBusyCursor wait; ok = run_qualification(cfg, msg); }
        set_status(msg, ! ok);
    });
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { save_to_config(); EndModal(wxID_OK); });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

    load_from_config();
    on_provider_changed();

    SetSizer(top);
    top->SetSizeHints(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    (void) em;
}

void AISettingsDialog::load_from_config()
{
    auto *ac = wxGetApp().app_config;
    if (ac == nullptr)
        return;
    m_provider->SetSelection(index_for_provider_key(ac->get("ai_slicer", "provider")));
    m_api_key->SetValue(from_u8(ac->get("ai_slicer", "api_key")));
    m_gateway_url->SetValue(from_u8(ac->get("ai_slicer", "gateway_url")));
    const std::string model = ac->get("ai_slicer", "model");
    if (! model.empty()) {
        m_model->Append(from_u8(model));
        m_model->SetValue(from_u8(model));
    }
}

void AISettingsDialog::save_to_config()
{
    auto *ac = wxGetApp().app_config;
    if (ac == nullptr)
        return;
    ac->set("ai_slicer", "provider",    provider_key_for_index(m_provider->GetSelection()));
    ac->set("ai_slicer", "api_key",     into_u8(m_api_key->GetValue()));
    ac->set("ai_slicer", "gateway_url", into_u8(m_gateway_url->GetValue()));
    ac->set("ai_slicer", "model",       into_u8(m_model->GetValue()));
    ac->save();
}

void AISettingsDialog::on_provider_changed()
{
    const bool compatible = m_provider->GetSelection() == 2;
    m_gateway_url->Enable(compatible);
    m_gateway_hint->Enable(compatible);
}

static AIConfig make_config_from_fields(ComboBox *provider, wxTextCtrl *key, wxTextCtrl *url, ComboBox *model)
{
    AIConfig cfg;
    cfg.provider = provider_key_for_index(provider->GetSelection());
    cfg.api_key  = into_u8(key->GetValue());
    cfg.base_url = into_u8(url->GetValue());
    cfg.model    = into_u8(model->GetValue());
    return cfg;
}

void AISettingsDialog::on_test()
{
    std::unique_ptr<AIProvider> provider(AIProvider::create(
        make_config_from_fields(m_provider, m_api_key, m_gateway_url, m_model)));
    if (! provider) {
        set_status(_L("Select a provider and enter an API key (and a gateway URL for the compatible provider)."), true);
        return;
    }
    set_status(_L("Testing..."));
    std::string error;
    bool ok = false;
    {
        wxBusyCursor wait; // the round-trip briefly blocks the UI, as the printer Test button does
        ok = provider->test_connection(error);
    }
    if (ok)
        set_status(_L("Connection OK."));
    else
        set_status(_L("Failed: ") + from_u8(error), true);
}

void AISettingsDialog::on_fetch_models()
{
    std::unique_ptr<AIProvider> provider(AIProvider::create(
        make_config_from_fields(m_provider, m_api_key, m_gateway_url, m_model)));
    if (! provider) {
        set_status(_L("Select a provider and enter an API key first."), true);
        return;
    }
    set_status(_L("Fetching models..."));
    std::vector<AIModelInfo> models;
    std::string error;
    bool ok = false;
    {
        wxBusyCursor wait;
        ok = provider->get_models(models, error);
    }
    if (! ok) {
        set_status(_L("Fetch failed: ") + from_u8(error), true);
        return;
    }
    const wxString keep = m_model->GetValue();
    m_model->Clear();
    int shown = 0, hidden = 0;
    for (const auto &m : models) {
        // Hide models that can't do chat/geometry at all (embeddings, rerankers,
        // guardrails, speech, OCR, ...). They only clutter the picker.
        std::string low = m.id;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        bool skip = false;
        for (const char *bad : {"embed", "rerank", "reward", "guard", "moderation", "ocr",
                                "whisper", "riva", "clip", "-tts", "-stt", "speech"})
            if (low.find(bad) != std::string::npos) { skip = true; break; }
        if (skip) { ++hidden; continue; }
        m_model->Append(from_u8(m.id));
        ++shown;
    }
    if (! keep.IsEmpty())
        m_model->SetValue(keep);
    set_status(wxString::Format(_L("Fetched %d usable models (hid %d embedding/reranker/etc.). "
                                   "Use \"Qualify for 3D\" to confirm one works."), shown, hidden));
}

void AISettingsDialog::set_status(const wxString &msg, bool error)
{
    if (m_status == nullptr)
        return;
    m_status->SetForegroundColour(error ? wxColour(0xC0, 0x30, 0x30) : wxColour(0x30, 0x80, 0x30));
    m_status->SetLabel(msg);
    Layout();
}

void AISettingsDialog::on_dpi_changed(const wxRect & /*suggested_rect*/)
{
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
