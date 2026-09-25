#include "SceneBenchmark.hpp"

#include "3DScene.hpp"
#include "BuildCommit.hpp"
#include "Camera.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "NotificationManager.hpp"
#include "OpenGLManager.hpp"
#include "Plater.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/ProgressBar.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/libslic3r.h"

#include <glad/gl.h>
#include <wx/clipbrd.h>
#include <wx/glcanvas.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <memory>
#include <numeric>
#include <sstream>

namespace Slic3r {
namespace GUI {

FrameTimeStats frame_time_stats(std::vector<double> frame_ms)
{
    FrameTimeStats stats;
    if (frame_ms.empty())
        return stats;

    std::sort(frame_ms.begin(), frame_ms.end());
    const double total_ms = std::accumulate(frame_ms.begin(), frame_ms.end(), 0.0);
    auto percentile = [&frame_ms](double p) {
        const size_t rank = size_t(std::ceil(p * double(frame_ms.size()) / 100.0));
        return frame_ms[std::clamp<size_t>(rank, 1, frame_ms.size()) - 1];
    };
    stats.fps        = total_ms > 0.0 ? 1000.0 * double(frame_ms.size()) / total_ms : 0.0;
    stats.average_ms = total_ms / double(frame_ms.size());
    stats.median_ms  = percentile(50.0);
    stats.p95_ms     = percentile(95.0);
    stats.p99_ms     = percentile(99.0);
    stats.max_ms     = frame_ms.back();
    return stats;
}

namespace {

constexpr size_t WARMUP_FRAMES = 30;
// A scene runs the camera path twice: timing the frames, then averaging the render timings.
constexpr size_t PASS_FRAMES  = 360;
constexpr size_t SCENE_FRAMES = WARMUP_FRAMES + 2 * PASS_FRAMES;

struct SceneResult
{
    std::string                         name;
    std::vector<double>                 frame_ms;
    std::vector<FrameProfiler::Section> sections;
};

class SceneBenchmarkDialog : public DPIDialog
{
public:
    SceneBenchmarkDialog();
    ~SceneBenchmarkDialog() override;

    bool is_running() const { return m_stage != Stage::Done; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    enum class Stage { Loading, Prepare, Slicing, Preview, Done };

    void on_timer(wxTimerEvent& evt);
    void on_idle(wxIdleEvent& evt);
    void on_first_frame();
    void begin_scene(GLCanvas3D* canvas, Stage stage);
    void end_scene();
    void stop_scene();
    void start_slicing();
    void set_camera(size_t pass_frame);
    void set_status(const wxString& status);
    void finish(const wxString& error = wxString());
    void show_report(const wxString& error);
    std::string report() const;

    Stage                             m_stage{ Stage::Loading };
    std::unique_ptr<wxWindowDisabler> m_disabler;
    wxTimer                           m_timer;
    int                               m_polls{ 0 };
    Label*                            m_status{ nullptr };
    ProgressBar*                      m_progress{ nullptr };

    GLCanvas3D*                           m_canvas{ nullptr };
    Camera                                m_saved_camera;
    Vec3d                                 m_target{ Vec3d::Zero() };
    double                                m_zoom{ 1.0 };
    size_t                                m_frame{ 0 };
    std::chrono::steady_clock::time_point m_last_frame;
    int                                   m_swap_interval{ wxGLCanvas::DefaultSwapInterval };
    bool                                  m_restore_swap_interval{ false };
    std::vector<SceneResult>              m_results;

    // Read on the first frame, with the context current.
    std::string m_vsync;
    int         m_msaa_samples{ 0 };
    int         m_viewport_width{ 0 };
    int         m_viewport_height{ 0 };
    std::string m_camera_type;
};

SceneBenchmarkDialog* s_benchmark = nullptr;

SceneBenchmarkDialog::SceneBenchmarkDialog()
    : DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Benchmark 3D Scene"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_timer(this)
{
    s_benchmark = this;
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->SetMinSize(wxSize(FromDIP(360), -1));
    m_status   = new Label(this, _L("Loading the benchmark model") + dots);
    m_progress = new ProgressBar(this, wxID_ANY, 100);
    m_progress->SetHeight(FromDIP(8));
    m_progress->SetMaxSize(wxSize(-1, FromDIP(8)));
    m_progress->SetProgressForedColour(StateColor::darkModeColorFor(wxColour("#DFDFDF")));
    sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(15));
    sizer->Add(m_progress, 0, wxEXPAND | wxALL, FromDIP(15));
    auto* buttons = new DialogButtons(this, {"Cancel"});
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    sizer->Add(buttons, 0, wxEXPAND);
    SetSizerAndFit(sizer);
    wxGetApp().UpdateDlgDarkUI(this);

    Bind(wxEVT_TIMER, &SceneBenchmarkDialog::on_timer, this);
    Bind(wxEVT_IDLE, &SceneBenchmarkDialog::on_idle, this);
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& evt) {
        if (evt.GetKeyCode() == WXK_ESCAPE)
            Close();
        else
            evt.Skip();
    });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        m_timer.Stop();
        stop_scene();
        m_disabler.reset();
        Destroy();
    });

    // A corner of the 3D view, away from the model.
    const wxRect view = wxGetApp().plater()->get_current_canvas3D()->get_wxglcanvas()->GetScreenRect();
    SetPosition(wxPoint(view.GetLeft() + FromDIP(20), view.GetBottom() - GetSize().GetHeight() - FromDIP(20)));

    m_disabler = std::make_unique<wxWindowDisabler>(this);
    m_timer.Start(100);
}

SceneBenchmarkDialog::~SceneBenchmarkDialog()
{
    // At shutdown the windows it disabled may be gone already.
    if (wxGetApp().is_closing())
        (void) m_disabler.release();
    if (s_benchmark == this)
        s_benchmark = nullptr;
}

void SceneBenchmarkDialog::on_dpi_changed(const wxRect&)
{
    Fit();
    Refresh();
}

void SceneBenchmarkDialog::on_timer(wxTimerEvent&)
{
    Plater* plater = wxGetApp().plater();
    if (wxGetApp().is_closing() || plater == nullptr) {
        m_timer.Stop();
        return;
    }

    if (m_stage == Stage::Loading) {
        if (!plater->get_ui_job_worker().is_idle())
            return;
        m_timer.Stop();
        GLCanvas3D* canvas = plater->get_view3D_canvas3D();
        const BoundingBoxf3 box = canvas->volumes_bounding_box(true);
        if (!box.defined) {
            finish(_L("The benchmark model could not be loaded."));
            return;
        }
        m_target = box.center();
        const Size size = canvas->get_canvas_size();
        m_zoom = double(std::min(size.get_width(), size.get_height())) / (1.1 * box.size().norm());
        plater->deselect_all();
        begin_scene(canvas, Stage::Prepare);
    } else if (m_stage == Stage::Slicing) {
        if (plater->is_background_process_slicing())
            return;
        GLCanvas3D* canvas = plater->get_preview_canvas3D();
        if (canvas->get_gcode_viewer().has_data() && canvas->get_wxglcanvas()->IsShownOnScreen()) {
            m_timer.Stop();
            plater->get_notification_manager()->set_slicing_progress_hidden();
            begin_scene(canvas, Stage::Preview);
        } else if (++m_polls > 30)
            finish(_L("Slicing failed, so only Prepare was measured."));
    }
}

void SceneBenchmarkDialog::on_idle(wxIdleEvent& evt)
{
    evt.Skip();
    if (m_canvas == nullptr || wxGetApp().is_closing())
        return;
    if (!m_canvas->get_wxglcanvas()->IsShownOnScreen()) {
        finish(_L("The benchmark stopped because the 3D view was hidden."));
        return;
    }

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (m_frame > WARMUP_FRAMES && m_frame <= WARMUP_FRAMES + PASS_FRAMES)
        m_results.back().frame_ms.push_back(std::chrono::duration<double, std::milli>(now - m_last_frame).count());
    m_last_frame = now;

    FrameProfiler& profiler = m_canvas->get_frame_profiler();
    if (m_frame == WARMUP_FRAMES + PASS_FRAMES)
        profiler.start_averaging();
    set_camera(m_frame < WARMUP_FRAMES ? 0 : (m_frame - WARMUP_FRAMES) % PASS_FRAMES);
    m_canvas->render();
    if (m_frame == 0)
        on_first_frame();

    if (++m_frame == SCENE_FRAMES) {
        m_results.back().sections = profiler.finish_averaging(true);
        end_scene();
        return;
    }
    const int percent = int(50 * m_frame / SCENE_FRAMES);
    if (m_frame % 30 == 0)
        m_progress->SetValue(m_stage == Stage::Prepare ? percent : 50 + percent);
    evt.RequestMore();
}

void SceneBenchmarkDialog::on_first_frame()
{
    // A benchmark measures the frames the GPU can draw, not the display refresh rate.
    wxGLCanvas* glcanvas = m_canvas->get_wxglcanvas();
    m_swap_interval      = glcanvas->GetSwapInterval();
    const bool known     = m_swap_interval != wxGLCanvas::DefaultSwapInterval;
    if (known && m_swap_interval != 0)
        m_restore_swap_interval = glcanvas->SetSwapInterval(0) != wxGLCanvas::SwapInterval::NotSet;
    if (m_results.size() > 1)
        return;

    m_vsync = !known ? "unknown" : m_swap_interval == 0 || m_restore_swap_interval ? "off" : "on";
    GLint samples = 0;
    glsafe(::glGetIntegerv(GL_SAMPLES, &samples));
    m_msaa_samples                    = samples;
    const Camera&             camera   = wxGetApp().plater()->get_camera();
    const std::array<int, 4>& viewport = camera.get_viewport();
    m_viewport_width                  = viewport[2];
    m_viewport_height                 = viewport[3];
    m_camera_type                     = camera.get_type_as_string();
}

void SceneBenchmarkDialog::begin_scene(GLCanvas3D* canvas, Stage stage)
{
    m_stage                 = stage;
    m_canvas                = canvas;
    m_saved_camera          = wxGetApp().plater()->get_camera();
    m_frame                 = 0;
    m_restore_swap_interval = false;
    m_results.push_back({stage == Stage::Prepare ? "Prepare" : "Preview", {}, {}});
    m_results.back().frame_ms.reserve(PASS_FRAMES);
    set_status(stage == Stage::Prepare ? _L("Turning the camera in Prepare") : _L("Turning the camera in Preview"));
    canvas->set_benchmarking(true);
}

void SceneBenchmarkDialog::end_scene()
{
    const Stage stage = m_stage;
    stop_scene();
    if (stage == Stage::Prepare)
        start_slicing();
    else
        finish();
}

void SceneBenchmarkDialog::stop_scene()
{
    if (m_canvas == nullptr)
        return;
    if (m_frame < SCENE_FRAMES)
        m_results.pop_back();
    FrameProfiler& profiler = m_canvas->get_frame_profiler();
    if (profiler.is_averaging())
        profiler.finish_averaging(false);
    if (m_restore_swap_interval && m_canvas->make_current_for_postinit())
        m_canvas->get_wxglcanvas()->SetSwapInterval(m_swap_interval);
    m_canvas->set_benchmarking(false);
    m_canvas->set_as_dirty();
    wxGetApp().plater()->get_camera() = m_saved_camera;
    m_canvas = nullptr;
}

void SceneBenchmarkDialog::start_slicing()
{
    m_stage = Stage::Slicing;
    set_status(_L("Slicing"));
    m_progress->SetValue(50);
    Plater* plater = wxGetApp().plater();
    plater->reslice();
    plater->select_view_3D("Preview", false);
    wxGetApp().mainframe->select_tab(TAB_ID_PREVIEW);
    m_polls = 0;
    m_timer.Start(100);
}

void SceneBenchmarkDialog::set_camera(size_t pass_frame)
{
    // Two turns around the model while the view goes three times from below the plate to nearly
    // straight above it, zooming in and out twice.
    const double t         = double(pass_frame) / double(PASS_FRAMES);
    const double azimuth   = (-0.75 + 4.0 * t) * PI;
    const double elevation = (30.0 + 55.0 * std::sin(6.0 * PI * t)) * PI / 180.0;
    const Vec3d  dir(std::cos(elevation) * std::cos(azimuth), std::cos(elevation) * std::sin(azimuth), std::sin(elevation));
    Camera&      camera = wxGetApp().plater()->get_camera();
    camera.look_at(m_target + Camera::DefaultDistance * dir, m_target, Vec3d::UnitZ());
    camera.set_zoom(m_zoom * (1.0 + 0.4 * std::sin(4.0 * PI * t)));
}

void SceneBenchmarkDialog::set_status(const wxString& status)
{
    m_status->SetLabel(status + dots);
    Fit();
}

void SceneBenchmarkDialog::finish(const wxString& error)
{
    m_timer.Stop();
    stop_scene();
    m_disabler.reset();
    m_stage = Stage::Done;
    show_report(error);
}

void SceneBenchmarkDialog::show_report(const wxString& error)
{
    DestroyChildren();
    m_status   = nullptr;
    m_progress = nullptr;

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    if (!error.IsEmpty()) {
        auto* message = new Label(this, error);
        message->Wrap(FromDIP(500));
        sizer->Add(message, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(15));
    }

    std::string text;
    if (!m_results.empty()) {
        text          = report();
        auto* content = new wxTextCtrl(this, wxID_ANY, from_u8(text), wxDefaultPosition, wxDefaultSize,
                                       wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxBORDER_SIMPLE);
        content->SetFont(wxGetApp().code_font());
        wxClientDC dc(content);
        dc.SetFont(content->GetFont());
        const wxSize extent = dc.GetMultiLineTextExtent(content->GetValue()) + FromDIP(wxSize(40, 20));
        content->SetMinSize(wxSize(extent.GetWidth(), std::min(extent.GetHeight(), FromDIP(560))));
        sizer->Add(content, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(15));
    }

    auto* buttons = new DialogButtons(this, text.empty() ? std::vector<wxString>{"OK"} : std::vector<wxString>{"Copy", "OK"});
    if (Button* copy = buttons->GetButtonFromID(wxID_COPY)) {
        copy->SetToolTip(_L("Copy the results to the clipboard"));
        copy->Bind(wxEVT_BUTTON, [text](wxCommandEvent&) {
            wxClipboardLocker lock;
            if (!lock)
                return;
            wxTheClipboard->SetData(new wxTextDataObject(from_u8(text)));
        });
    }
    buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    sizer->Add(buttons, 0, wxEXPAND);

    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
    Raise();
}

std::string SceneBenchmarkDialog::report() const
{
    const AppConfig&            config = *wxGetApp().app_config;
    const OpenGLManager::GLInfo& gl    = OpenGLManager::get_gl_info();
    auto on_off = [](bool on) { return on ? "on" : "off"; };

    std::ostringstream out;
    out << SLIC3R_APP_NAME << " 3D scene benchmark\n"
        << "Version:    " << SoftFever_VERSION << " (" << build_commit_label << ")\n"
        << "GPU:        " << gl.get_renderer() << '\n'
        << "OpenGL:     " << gl.get_version() << '\n'
        << "Viewport:   " << m_viewport_width << " x " << m_viewport_height << " px, " << m_camera_type << " camera\n"
        << "Settings:   MSAA " << (m_msaa_samples > 1 ? std::to_string(m_msaa_samples) + "x" : "off")
        << ", FXAA " << on_off(config.get_bool(SETTING_OPENGL_FXAA_ENABLED))
        << ", scene cache " << on_off(config.get_bool(SETTING_OPENGL_SCENE_CACHE)) << ", VSync " << m_vsync << '\n'
        << "Realistic:  ";
    if (config.get_bool(SETTING_OPENGL_REALISTIC_MODE))
        out << "on, Phong " << on_off(config.get_bool(SETTING_OPENGL_REALISTIC_PHONG)) << ", SSAO "
            << on_off(config.get_bool(SETTING_OPENGL_PHONG_SSAO)) << ", plate shadows "
            << on_off(config.get_bool(SETTING_OPENGL_PHONG_BASIC_PLATE_SHADOWS)) << ", in Preview " << on_off(config.get_bool(SETTING_OPENGL_REALISTIC_PREVIEW)) << '\n';
    else
        out << "off\n";
    out << "Scene:      OrcaSliced Combo, " << PASS_FRAMES << " frames per pass\n\n";

    std::vector<FrameTimeStats> stats;
    for (const SceneResult& result : m_results)
        stats.push_back(frame_time_stats(result.frame_ms));
    auto row = [&out, &stats](const char* label, double FrameTimeStats::*value) {
        out << std::left << std::setw(22) << label << std::right;
        for (const FrameTimeStats& s : stats)
            out << std::setw(10) << s.*value;
        out << '\n';
    };

    out << std::fixed << std::setprecision(1) << std::setw(22) << "";
    for (const SceneResult& result : m_results)
        out << std::right << std::setw(10) << result.name;
    out << '\n';
    row("Average FPS", &FrameTimeStats::fps);
    out << std::setprecision(2) << "Frame time (ms)\n";
    row("  average", &FrameTimeStats::average_ms);
    row("  median", &FrameTimeStats::median_ms);
    row("  95th percentile", &FrameTimeStats::p95_ms);
    row("  99th percentile", &FrameTimeStats::p99_ms);
    row("  maximum", &FrameTimeStats::max_ms);

    for (const SceneResult& result : m_results) {
        out << "\nRender timings, " << result.name << " (ms per frame)\n";
        if (result.sections.empty()) {
            out << "  not supported by the graphics driver\n";
            continue;
        }
        out << std::setw(22) << "" << std::setw(10) << "CPU" << std::setw(10) << "GPU" << '\n';
        double cpu_ms = 0.0;
        double gpu_ms = 0.0;
        for (const FrameProfiler::Section& section : result.sections) {
            out << "  " << std::left << std::setw(20) << section.name << std::right << std::setw(10) << section.cpu_ms << std::setw(10)
                << section.gpu_ms << '\n';
            cpu_ms += section.cpu_ms;
            gpu_ms += section.gpu_ms;
        }
        out << "  " << std::left << std::setw(20) << "total" << std::right << std::setw(10) << cpu_ms << std::setw(10) << gpu_ms << '\n';
    }
    return out.str();
}

} // namespace

void run_scene_benchmark()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || !wxGetApp().is_editor())
        return;
    if (s_benchmark != nullptr) {
        if (s_benchmark->is_running()) {
            s_benchmark->Raise();
            return;
        }
        s_benchmark->Close();
    }

    MessageDialog confirm(wxGetApp().mainframe,
                          _L("The benchmark replaces the current project with the OrcaSliced Combo and slices it. It turns the camera "
                             "around the model in Prepare and in Preview, timing every frame, then shows the results.") + "\n\n" +
                          _L("It uses the current graphics settings and usually takes less than a minute. The main window stays "
                             "locked until it finishes."),
                          _L("Benchmark 3D Scene"), wxICON_INFORMATION | wxOK | wxCANCEL);
    confirm.SetButtonLabel(wxID_OK, _L("Start"), true);
    if (confirm.ShowModal() != wxID_OK || plater->new_project() == wxID_CANCEL)
        return;

    const std::vector<MenuFactory::HandyModel>& models = MenuFactory::handy_models();
    const auto combo = std::find_if(models.begin(), models.end(),
                                    [](const MenuFactory::HandyModel& model) { return std::strcmp(model.key, "orcasliced_combo") == 0; });
    MenuFactory::load_handy_model(size_t(combo - models.begin()));
    (new SceneBenchmarkDialog())->Show();
}

} // namespace GUI
} // namespace Slic3r
