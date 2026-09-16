#include "AISlicerDialog.hpp"
#include "AIGenerate.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#include <boost/filesystem.hpp>

#include <wx/listbox.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include <wx/weakref.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "GLCanvas3D.hpp"
#include "Widgets/Button.hpp"
#include "Jobs/Worker.hpp"
#include "Jobs/Job.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

#include "slic3r/Utils/AIProvider.hpp"
#include "slic3r/Utils/AIPrinterContext.hpp"
#include "slic3r/Utils/AIShapeGen.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r { namespace GUI {

// Bed extents (mm) from the active machine config, for the printable-fit check.
static void compute_bed_dims(const DynamicPrintConfig &cfg, double &x, double &y, double &z)
{
    z = (cfg.option("printable_height") != nullptr) ? cfg.opt_float("printable_height") : 250.0;
    x = y = 220.0;
    if (auto *pa = cfg.option<ConfigOptionPoints>("printable_area")) {
        double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
        for (const Vec2d &p : pa->values) {
            minx = std::min(minx, p.x()); maxx = std::max(maxx, p.x());
            miny = std::min(miny, p.y()); maxy = std::max(maxy, p.y());
        }
        if (maxx > minx && maxy > miny) { x = maxx - minx; y = maxy - miny; }
    }
}

AISlicerDialog::AISlicerDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("AI"), wxDefaultPosition,
                wxSize(52 * wxGetApp().em_unit(), -1), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(wxGetApp().normal_font());
    SetBackgroundColour(*wxWHITE);

    auto *nb = new wxNotebook(this, wxID_ANY);
    nb->AddPage(build_generate_tab(nb), _L("Generate shape"), true);
    nb->AddPage(build_search_tab(nb),   _L("Find models"), false);

    auto *close_btn = new Button(this, _L("Close"));
    close_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });
    auto *btn_row = new wxBoxSizer(wxHORIZONTAL);
    btn_row->AddStretchSpacer(1);
    btn_row->Add(close_btn, 0);

    auto *top = new wxBoxSizer(wxVERTICAL);
    top->Add(nb,      1, wxEXPAND | wxALL, FromDIP(12));
    top->Add(btn_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    SetSizer(top);
    top->SetSizeHints(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

wxWindow *AISlicerDialog::build_generate_tab(wxNotebook *nb)
{
    auto *panel = new wxPanel(nb, wxID_ANY);
    auto *s = new wxBoxSizer(wxVERTICAL);

    s->Add(new wxStaticText(panel, wxID_ANY, _L("Describe a mechanical part to generate:")),
           0, wxALL, FromDIP(10));

    m_prompt = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition,
                              wxSize(FromDIP(420), FromDIP(110)), wxTE_MULTILINE);
    s->Add(m_prompt, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    s->Add(new wxStaticText(panel, wxID_ANY,
               _L("Builds parametric parts from primitives + boolean cuts (brackets, knobs, boxes, holders, "
                  "adapters) sized to your printer. Not for organic/sculptural shapes (animals, figurines) — "
                  "for those, use the Find models tab. Your printer's settings and sensors are sent with each request.")),
           0, wxALL, FromDIP(10));

    m_generate_btn = new Button(panel, _L("Generate"));
    m_generate_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_generate(); });
    s->Add(m_generate_btn, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_status = new wxStaticText(panel, wxID_ANY, "");
    s->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    panel->SetSizer(s);
    return panel;
}

// Result shared between the worker thread (fills it) and the finish handler
// (consumes it on the main thread).
namespace { struct AIGenResult { Model model; std::string error; std::string warn; bool ok = false; }; }

// Shared generation entry point (see AIGenerate.hpp). Runs the full pipeline on
// a background Job and calls on_done(ok, message) on the main thread.
void ai_generate_shape_to_plate(const std::string &prompt,
                                std::function<void(bool, const std::string &)> on_done)
{
    auto *ac = wxGetApp().app_config;
    AIConfig cfg = ac ? AIProvider::config_from_app_config(*ac) : AIConfig();
    if (! std::unique_ptr<AIProvider>(AIProvider::create(cfg))) {
        on_done(false, "No AI provider is configured. Open Preferences > General > AI and register a provider and API key first.");
        return;
    }

    // Everything that touches the GUI must happen HERE, on the main thread:
    // gather machine context, build the request, read the bed dimensions.
    AIPrinterContext ctx = AIPrinterContext::gather();
    auto messages = std::make_shared<std::vector<AIMessage>>();
    {
        AIMessage sys;
        sys.role    = "system";
        sys.content = "You design small, printable 3D shapes and return them ONLY by calling the "
                      "create_model tool (never reply in prose). Respect this printer's constraints; "
                      "never exceed the build volume.\n\nPrinter context:\n" + ctx.to_prompt();
        messages->push_back(std::move(sys));
        AIMessage user;
        user.role    = "user";
        user.content = prompt;
        if (ctx.has_image())
            user.images.push_back(ctx.to_image());
        messages->push_back(std::move(user));
    }
    const std::string name = prompt.substr(0, 40);

    // Force a create_model tool call so the model MUST return a structured shape
    // spec rather than prose. A model that replies with text instead is
    // unsuitable — which is exactly how we tell capable models from talky ones.
    auto params = std::make_shared<nlohmann::json>();
    {
        nlohmann::json tool = {
            {"type", "function"},
            {"function", {
                {"name", "create_model"},
                {"description", std::string("Create a printable 3D shape as a parametric tree. Never explain in prose. ")
                               + ai_shape_spec_instructions()},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"shape", {{"type", "object"}, {"description", "The root shape node (type + params + children)."}}},
                        {"design_summary", {{"type", "string"}}}
                    }},
                    {"required", nlohmann::json::array({"shape"})}
                }}
            }}
        };
        (*params)["tools"]       = nlohmann::json::array({tool});
        (*params)["tool_choice"] = {{"type", "function"}, {"function", {{"name", "create_model"}}}};
        (*params)["temperature"] = 0.2;
    }

    double bx = 220, by = 220, bz = 250;
    if (auto *bundle = wxGetApp().preset_bundle)
        compute_bed_dims(bundle->full_config(), bx, by, bz);

    auto result = std::make_shared<AIGenResult>();

    Worker &worker = wxGetApp().plater()->get_ui_job_worker();
    replace_job(worker,
        // ---- worker thread: LLM round-trip + geometry build, with a repair loop
        // (generate -> build/validate -> feed the error back -> regenerate) ----
        [cfg, messages, params, name, result, bx, by, bz](Job::Ctl &ctl) {
            std::unique_ptr<AIProvider> provider(AIProvider::create(cfg));
            if (! provider) { result->error = "No AI provider configured."; return; }

            const int MAX_REPAIRS = 2;
            for (int attempt = 0; attempt <= MAX_REPAIRS; ++attempt) {
                ctl.update_status(attempt == 0 ? 15 : 45,
                                  attempt == 0 ? "Contacting the AI..." : "Fixing the design...");
                AIResponse resp = provider->chat(*messages, *params);
                if (ctl.was_canceled()) return;
                if (! resp.ok) { result->error = "AI request failed: " + resp.error; return; } // network: no retry

                ctl.update_status(70, "Building geometry...");
                Model candidate;
                std::string berr, produced = resp.tool_call_arguments;
                bool built = false;
                if (! resp.tool_call_arguments.empty())
                    built = ai_build_model_from_tool_call(resp.tool_call_arguments, name, candidate, berr);
                else if (! resp.content.empty()) {
                    produced = resp.content;
                    built = ai_build_model_from_response(resp.content, name, candidate, berr);
                } else {
                    // Talky model — no point retrying; this is the unsuitable-model signal.
                    result->error = "This model returned no shape (it didn't call the create_model tool), "
                                    "so it isn't suitable for 3D generation. Try a recommended model such as "
                                    "a llama-3.x-instruct or a code model.";
                    return;
                }

                // Validate: did it build, and does it fit the bed?
                std::string problem;
                if (! built || candidate.objects.empty())
                    problem = berr.empty() ? "the shape could not be built" : berr;
                else {
                    std::string warn;
                    if (! ai_model_fits_bed(candidate, bx, by, bz, warn))
                        problem = warn + " Make it smaller so it fits.";
                }

                if (problem.empty()) {                       // success
                    result->model = std::move(candidate);
                    result->ok    = true;
                    ctl.update_status(100, "");
                    return;
                }
                if (attempt == MAX_REPAIRS) {                // give up repairing
                    if (built && ! candidate.objects.empty()) { // over-volume but usable: accept + warn
                        result->model = std::move(candidate);
                        result->ok    = true;
                        result->warn  = problem;
                    } else {
                        result->error = "Couldn't produce a valid shape after " + std::to_string(MAX_REPAIRS + 1) +
                                        " tries. Last problem: " + problem;
                    }
                    return;
                }
                // Feed the problem back and let the model correct itself.
                AIMessage fix;
                fix.role    = "user";
                fix.content = "Your previous create_model result had a problem: " + problem +
                              "\nCall create_model again with a corrected shape. Your previous attempt was:\n" +
                              produced.substr(0, 1500);
                messages->push_back(std::move(fix));
            }
        },
        // ---- main thread: drop the object on the plate + report ----
        [result, on_done](bool canceled, std::exception_ptr &eptr) {
            wxString msg; bool err = false;
            if (canceled) { msg = _L("Generation canceled."); err = true; }
            else if (eptr) {
                try { std::rethrow_exception(eptr); }
                catch (std::exception &e) { msg = from_u8(std::string("Error: ") + e.what()); }
                catch (...)               { msg = _L("Unknown error during generation."); }
                err = true;
            }
            else if (! result->ok) { msg = from_u8(result->error); err = true; }
            else {
                Plater *plater = wxGetApp().plater();
                plater->take_snapshot(into_u8(_L("Add AI generated object")));
                Model &model = plater->model();
                std::vector<size_t> idxs;
                for (const ModelObject *src : result->model.objects) {
                    ModelObject *dst = model.add_object(*src);
                    dst->ensure_on_bed();
                    idxs.push_back(model.objects.size() - 1);
                }
                wxGetApp().obj_list()->paste_objects_into_list(idxs);
                if (auto *canvas = plater->get_view3D_canvas3D())
                    canvas->reload_scene(true);
                if (result->warn.empty())
                    msg = _L("Added the generated object to the plate.");
                else { msg = from_u8(result->warn) + " " + _L("Added anyway — scale it to fit."); err = true; }
            }
            on_done(! err, into_u8(msg));
        });
}

void AISlicerDialog::on_generate()
{
    const wxString promptw = m_prompt->GetValue();
    if (promptw.IsEmpty()) { set_status(_L("Enter a description first."), true); return; }

    set_status(_L("Generating in the background..."));
    if (m_generate_btn) m_generate_btn->Disable();

    wxWeakRef<AISlicerDialog> self(this);   // survives the dialog closing mid-generation
    ai_generate_shape_to_plate(into_u8(promptw), [self](bool ok, const std::string &message) {
        if (self) {
            self->set_status(from_u8(message), ! ok);
            if (self->m_generate_btn) self->m_generate_btn->Enable();
        }
    });
}

void AISlicerDialog::set_status(const wxString &msg, bool error)
{
    if (m_status == nullptr)
        return;
    m_status->SetForegroundColour(error ? wxColour(0xC0, 0x30, 0x30) : wxColour(0x30, 0x80, 0x30));
    m_status->SetLabel(msg);
    Layout();
}

// --- Search on the web (open the site's own search in the browser) ----------
//
// A plain LLM cannot return working download URLs: it has no live web access, and
// the model repositories gate downloads behind sign-in / anti-bot. So rather than
// try to fetch a file directly (which yields 403/404/DNS errors), we hand the
// query to each site's real search page in the user's browser, where their normal
// (signed-in) download flow works. The file is then loaded via File > Import.

static std::string url_encode(const std::string &s)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 0x0F]; }
    }
    return out;
}

wxWindow *AISlicerDialog::build_search_tab(wxNotebook *nb)
{
    auto *panel = new wxPanel(nb, wxID_ANY);
    auto *s = new wxBoxSizer(wxVERTICAL);

    s->Add(new wxStaticText(panel, wxID_ANY,
               _L("Find a model on a printing site, then download it there and load it with File > Import:")),
           0, wxALL, FromDIP(10));

    m_query = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(420), -1), wxTE_PROCESS_ENTER);
    s->Add(m_query, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    // One button per repository — each opens that site's search results in the browser.
    struct Site { const wchar_t *name; const char *prefix; const char *suffix; };
    static const Site kSites[] = {
        { L"Printables",  "https://www.printables.com/search/models?q=",      ""             },
        { L"MakerWorld",  "https://makerworld.com/en/search/models?keyword=", ""             },
        { L"Thingiverse", "https://www.thingiverse.com/search?q=",            "&type=things" },
        { L"Thangs",      "https://thangs.com/search/",                       "?scope=all"   },
    };
    auto *btn_row = new wxBoxSizer(wxHORIZONTAL);
    for (const auto &site : kSites) {
        auto *b = new Button(panel, wxString(site.name));
        const std::string prefix = site.prefix, suffix = site.suffix;
        b->Bind(wxEVT_BUTTON, [this, prefix, suffix](wxCommandEvent &) { open_model_search(prefix, suffix); });
        btn_row->Add(b, 0, wxRIGHT, FromDIP(8));
    }
    s->Add(btn_row, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    // Enter opens the first site (Printables).
    m_query->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) {
        open_model_search("https://www.printables.com/search/models?q=", "");
    });

    s->Add(new wxStaticText(panel, wxID_ANY,
               _L("These are community model sites; most let you download after a free sign-in. "
                  "Save the .stl/.3mf, then load it with File > Import.")),
           0, wxALL, FromDIP(10));

    m_search_status = new wxStaticText(panel, wxID_ANY, "");
    s->Add(m_search_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    panel->SetSizer(s);
    return panel;
}

void AISlicerDialog::open_model_search(const std::string &url_prefix, const std::string &url_suffix)
{
    const std::string q = into_u8(m_query->GetValue());
    if (q.empty()) { set_search_status(_L("Type what you're looking for first."), true); return; }
    const std::string url = url_prefix + url_encode(q) + url_suffix;
    wxLaunchDefaultBrowser(from_u8(url));
    set_search_status(_L("Opened the search in your browser."));
}

void AISlicerDialog::set_search_status(const wxString &msg, bool error)
{
    if (m_search_status == nullptr)
        return;
    m_search_status->SetForegroundColour(error ? wxColour(0xC0, 0x30, 0x30) : wxColour(0x30, 0x80, 0x30));
    m_search_status->SetLabel(msg);
    Layout();
}

void AISlicerDialog::on_dpi_changed(const wxRect & /*suggested_rect*/)
{
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
