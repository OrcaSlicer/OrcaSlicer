#include "ModelGenerationPanel.hpp"

#include "3DScene.hpp"
#include "AI/Model/VertexColorRegionEditor.hpp"
#include "AI/AIWindowAppearance.hpp"
#include "AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "AI/ModelGeneration/ModelPreview3D.hpp"
#include "AI/ModelGeneration/ModelImageDisplayCopy.hpp"
#include "AI/ModelGeneration/ModelGenerationStatusText.hpp"
#include "AISidecarClient.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "GLModel.hpp"
#include "GLShader.hpp"
#include "GuiColor.hpp"
#include "MsgDialog.hpp"
#include "OpenGLManager.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

#include <glad/gl.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/collpane.h>
#include <wx/colordlg.h>
#include <wx/clrpicker.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/datetime.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stdpaths.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

namespace Slic3r::GUI {
using namespace ModelGenerationStatusText;
using namespace ModelGenerationPresentation;
namespace {

constexpr int POLL_TIMER_ID = wxID_HIGHEST + 913;

// Native scrolling copies pixels, including stale OpenGL contents. Invalidate
// the page after moving its child windows so controls below the canvas repaint.
class ModelResultScrolledWindow final : public wxScrolledWindow
{
public:
    using wxScrolledWindow::wxScrolledWindow;
    void ScrollWindow(int dx, int dy, const wxRect* rect = nullptr) override
    {
        wxScrolledWindow::ScrollWindow(dx, dy, rect);
        Refresh();
    }
};

} // namespace

ModelGenerationPanel::ModelGenerationPanel(wxWindow* parent, AI::IModelArtifactConsumer& artifact_consumer,
                                           AI::IPrintablePaletteProvider& palette_provider)
    : wxPanel(parent)
    , m_artifact_consumer(artifact_consumer)
    , m_palette_provider(palette_provider)
    , m_client(AISidecarClient::default_endpoint())
    , m_poll_timer(this, POLL_TIMER_ID)
{
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: build page";
    SetBackgroundColour(*wxWHITE);
    build_page();
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: refresh palette";
    refresh_palette();
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: load library entries";
    load_library_entries();
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: bind events";
    Bind(wxEVT_TIMER, &ModelGenerationPanel::on_poll, this, POLL_TIMER_ID);
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        if (event.IsShown()) {
            refresh_controls();
            if (m_model_preview_ready && m_model_preview != nullptr) {
                wxGetApp().CallAfter([this]() {
                    if (!m_shutdown && m_model_preview != nullptr)
                        m_model_preview->refresh();
                });
            }
        }
        event.Skip();
    });
    m_status->SetLabel(_L("正在检查本地 3D 生成服务..."));
    m_result_summary->SetLabel(_L("本地服务就绪后即可使用 3D 生成功能。"));
    refresh_controls();
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: constructor complete";
    refresh_ai_appearance(this);
}

ModelGenerationPanel::~ModelGenerationPanel()
{
    shutdown();
}

void ModelGenerationPanel::set_service_availability(bool available, const std::string& message)
{
    if (m_shutdown)
        return;
    m_service_available = available;
    if (available && !m_busy) {
        m_status->SetLabel(_L("本地 3D 生成服务已就绪。"));
        m_result_summary->SetLabel(_L("输入描述、选择参考图，或同时提供两者即可开始。"));
        update_workflow();
        if (!m_restore_checked && m_job_id.empty()) {
            m_restore_checked = true;
            restore_latest_job();
        }
    } else if (!m_busy) {
        if (!message.empty())
            BOOST_LOG_TRIVIAL(warning) << "AI model generation service unavailable: " << message;
        m_status->SetLabel(_L("本地生成服务未启动。点击“重新检测服务”即可恢复。"));
        m_result_summary->SetLabel(_L("服务恢复后会自动载入最近任务，当前本地模型不会丢失。"));
    }
    refresh_controls();
}

void ModelGenerationPanel::set_service_retry_handler(std::function<void()> handler)
{
    m_service_retry_handler = std::move(handler);
    refresh_controls();
}

void ModelGenerationPanel::show_input_hint(const wxString& message, wxWindow* focus)
{
    m_status->SetLabel(message);
    m_status->Wrap(std::max(1, m_status->GetParent()->GetClientSize().x - FromDIP(24)));
    m_status->GetParent()->Layout();
    if (focus != nullptr) focus->SetFocus();
}

void ModelGenerationPanel::restore_latest_job()
{
    if (m_shutdown || !m_service_available || !m_job_id.empty())
        return;
    const uint64_t sequence = ++m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_latest(
        [weak, sequence](std::optional<AIModelGenerationClient::JobStatus> status) mutable {
            if (!weak || !status) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(*status)]() mutable {
                if (weak) weak->restore_job(std::move(status), sequence);
            });
        },
        [weak](std::string error) {
            if (!weak) return;
            BOOST_LOG_TRIVIAL(warning) << "Unable to restore the latest generated-model job: " << error;
        });
}

void ModelGenerationPanel::restore_job(AIModelGenerationClient::JobStatus status, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence || !m_job_id.empty())
        return;
    m_job_palette = status.palette;
    m_job_palette_color_count = status.palette_color_count;
    if (m_palette_color_count != nullptr) {
        m_palette_color_count->SetSelection(static_cast<int>(
            status.palette_color_count - Slic3r::AI::kMinTargetPaletteColors));
    }
    m_job_palette_roles = status.palette_roles.empty() ? automatic_palette_roles(status.palette) : status.palette_roles;
    m_palette_roles = m_job_palette_roles;
    m_palette_roles_source = status.palette;
    if (status.palette_recommendation.confirmed && !status.palette.empty()) {
        m_palette_recommendation_confirmed = true;
        m_custom_palette = status.palette;
        if (m_palette_source != nullptr)
            m_palette_source->SetSelection(2);
    }
    m_job_use_printable_colors = !status.palette.empty() || status.palette_recommendation.available;
    m_job_preview_expected = status.source == "image" || status.preview_ready || status.raw_preview_ready ||
                             status.model_reference_ready ||
                             !status.palette.empty();
    m_palette = status.palette;
    m_job_style = status.style;
    m_job_custom_style = status.custom_style;
    m_palette_quality_ok = status.palette_quality_ok;
    m_material_fragmentation_ok = status.material_fragmentation_ok;
    m_model_input_eligible = status.model_input_eligible;
    m_model_input_primary_blocker = status.model_input_blockers.empty() ? std::string() : status.model_input_blockers.front();
    m_meaningful_palette_count = status.meaningful_palette_count;
    m_meaningful_subject_color_count = status.meaningful_subject_color_count;
    m_job_print_settings = status.print_settings;
    m_job_face_limit = status.face_limit;
    m_job_generation_profile = status.generation_profile == "performance" ? "performance" : "quality";
    m_job_prompt = wxString::FromUTF8(status.user_prompt);
    if (m_prompt != nullptr)
        m_prompt->SetValue(m_job_prompt);
    if (m_use_printable_colors != nullptr)
        m_use_printable_colors->SetValue(m_job_use_printable_colors);
    if (m_style != nullptr) {
        m_style->SetSelection(style_selection(status.style));
        m_stylized_style->SetSelection(stylized_style_selection(status.style));
        m_style_user_selected = true;
    }
    if (m_custom_style != nullptr)
        m_custom_style->SetValue(wxString::FromUTF8(status.custom_style));
    if (m_palette_source != nullptr) {
        m_custom_palette = status.palette;
        m_palette_source->SetSelection(m_job_use_printable_colors ? 2 : 1);
        m_palette_recommendation_confirmed = !status.palette.empty();
    }
    if (m_quality != nullptr) {
        m_quality->SetSelection(m_job_generation_profile == "performance" ? 1 : 0);
    }
    if (m_print_width != nullptr) m_print_width->SetValue(status.print_settings.width_mm);
    if (m_nozzle_size != nullptr) m_nozzle_size->SetValue(status.print_settings.nozzle_mm);
    if (m_line_width != nullptr) m_line_width->SetValue(status.print_settings.line_width_mm);
    if (m_minimum_feature != nullptr) m_minimum_feature->SetValue(status.print_settings.minimum_feature_mm);
    if (m_shadow_color != nullptr) {
        const int selection = status.print_settings.shadow_color == "red" ? 1 :
                              status.print_settings.shadow_color == "green" ? 2 :
                              status.print_settings.shadow_color == "white" ? 3 : 0;
        m_shadow_color->SetSelection(selection);
    }
    m_job_image_path.clear();
    if (status.source == "image" && status.input_ready) {
        m_job_image_path = temp_path(status.id + "-input", "png");
        m_selected_image_path = m_job_image_path;
        m_restoring_input = true;
    }
    // Widget setters may clamp or normalize restored values (notably spin controls).
    // Rebase the comparison snapshot on the values the user actually sees so opening
    // a completed preview cannot immediately mark that same preview as stale.
    m_job_prompt = m_prompt->GetValue();
    m_job_style = current_style();
    m_job_custom_style = current_custom_style();
    m_job_use_printable_colors = use_printable_colors();
    m_job_palette = current_palette();
    m_job_print_settings = current_print_settings();
    m_status->SetLabel(_L("正在恢复上次模型生成任务..."));
    handle_status(std::move(status), sequence);
    // The persisted job owns the confirmed semantic material mapping. Widget
    // refreshes may infer generic light/chroma defaults while restore is still
    // asynchronous; never let that overwrite skin-versus-garment ownership.
    m_palette_roles = m_job_palette_roles;
    m_palette_roles_source = m_job_palette;
    if (m_restoring_input)
        download_restored_input(sequence);
}

void ModelGenerationPanel::download_restored_input(uint64_t sequence)
{
    const std::string job_id = m_job_id;
    const boost::filesystem::path path = m_job_image_path;
    if (job_id.empty() || path.empty())
        return;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_input(job_id, path,
        [weak, sequence](boost::filesystem::path restored) {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, restored = std::move(restored)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) return;
                weak->m_selected_image_path = restored;
                weak->m_job_image_path = restored;
                weak->m_reference_image_path = restored;
                weak->m_restoring_input = false;
                weak->m_selected_image->SetLabel(_L("已恢复上次参考图"));
                weak->show_selected_image_preview();
                // The restore is now fully materialized. Capture the effective UI
                // values once more after loading the image so asynchronous restore
                // order cannot turn the just-restored job into a stale job.
                weak->m_job_prompt = weak->m_prompt->GetValue();
                weak->m_job_style = weak->current_style();
                weak->m_job_custom_style = weak->current_custom_style();
                weak->m_job_use_printable_colors = weak->use_printable_colors();
                weak->m_job_palette = weak->current_palette();
                weak->m_palette_roles = weak->m_job_palette_roles;
                weak->m_palette_roles_source = weak->m_job_palette;
                weak->m_job_print_settings = weak->current_print_settings();
                // Restoring an already chosen style must not issue a new
                // recommendation: that request cancels concurrent downloads.
                weak->refresh_style_recommendation();
                if (!weak->m_awaiting_palette_confirmation && weak->m_preview_path.empty() && !weak->m_style_preview_ready)
                    weak->download_preview(sequence);
                weak->refresh_controls();
            });
        },
        [weak, sequence](std::string error) {
            if (!weak) return;
            BOOST_LOG_TRIVIAL(warning) << "Unable to restore the generated-model input image: " << error;
            wxGetApp().CallAfter([weak, sequence]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) return;
                weak->m_restoring_input = false;
                if (!weak->m_awaiting_palette_confirmation)
                    weak->download_preview(sequence);
            });
        });
}

void ModelGenerationPanel::shutdown()
{
    if (m_shutdown)
        return;
    m_shutdown = true;
    stop_model_finishing();
    if (m_preview_worker.joinable()) m_preview_worker.join();
    ++m_sequence;
    m_poll_timer.Stop();
    m_client.cancel_current();
    cleanup_files();
}

void ModelGenerationPanel::build_page()
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxPanel(this);
    header->SetBackgroundColour(wxColour(246, 249, 249));
    auto* header_sizer = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(header, wxID_ANY, _L("3D 生成"));
    wxFont title_font = title->GetFont();
    title_font.SetPointSize(title_font.GetPointSize() + 5);
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(title_font);
    title->SetForegroundColour(wxColour(31, 55, 59));
    header_sizer->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    header_sizer->AddSpacer(FromDIP(4));
    header_sizer->Add(new wxStaticText(header, wxID_ANY, _L("通过文字、参考图或两者组合生成可预览、可打印的 3D 模型。")),
                      0, wxLEFT | wxRIGHT, FromDIP(12));
    header_sizer->AddSpacer(FromDIP(10));
    header->SetSizer(header_sizer);
    root->Add(header, 0, wxEXPAND);

    auto* content = new wxBoxSizer(wxHORIZONTAL);
    m_workflow_panel = build_workflow_panel(this);
    content->Add(m_workflow_panel, 0, wxEXPAND | wxALL, FromDIP(12));

    content->Add(build_preview_panel(this), 1, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(content, 1, wxEXPAND);
    SetSizer(root);
}

wxWindow* ModelGenerationPanel::build_workflow_panel(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(400), -1), wxBORDER_SIMPLE);
    panel->SetMinSize(wxSize(FromDIP(360), -1));
    panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* journey = new wxPanel(panel);
    journey->SetBackgroundColour(wxColour(244, 248, 248));
    auto* journey_sizer = new wxBoxSizer(wxVERTICAL);
    auto* step_row = new wxBoxSizer(wxHORIZONTAL);
    const std::array<wxString, 4> step_names = {
        _L("1 输入"), _L("2 图片确认"), _L("3 生成 3D"), _L("4 导入")
    };
    for (size_t index = 0; index < step_names.size(); ++index) {
        m_step_labels[index] = new wxStaticText(journey, wxID_ANY, step_names[index],
                                                wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        m_step_labels[index]->SetForegroundColour(index == 0 ? wxColour(24, 112, 105) : wxColour(132, 143, 145));
        wxFont step_font = m_step_labels[index]->GetFont();
        step_font.SetWeight(index == 0 ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        m_step_labels[index]->SetFont(step_font);
        step_row->Add(m_step_labels[index], 1, wxALIGN_CENTER_VERTICAL);
    }
    journey_sizer->Add(step_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    m_workflow_steps = new wxStaticText(journey, wxID_ANY, _L("输入文字、图片，或同时使用两者"));
    m_workflow_steps->SetForegroundColour(wxColour(91, 104, 107));
    m_workflow_steps->Wrap(FromDIP(330));
    journey_sizer->Add(m_workflow_steps, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    auto* progress_header = new wxBoxSizer(wxHORIZONTAL);
    m_workflow_phase = new wxStaticText(journey, wxID_ANY, _L("检查本地服务"));
    wxFont workflow_font = m_workflow_phase->GetFont();
    workflow_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_workflow_phase->SetFont(workflow_font);
    m_progress_percent = new wxStaticText(journey, wxID_ANY, "0%");
    m_progress_percent->SetForegroundColour(wxColour(91, 104, 107));
    progress_header->Add(m_workflow_phase, 1, wxALIGN_CENTER_VERTICAL);
    progress_header->Add(m_progress_percent, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    journey_sizer->Add(progress_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    m_generation_progress = new wxGauge(journey, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, FromDIP(6)));
    m_generation_progress->SetValue(0);
    journey_sizer->Add(m_generation_progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    journey_sizer->AddSpacer(FromDIP(8));
    journey->SetSizer(journey_sizer);
    outer->Add(journey, 0, wxEXPAND);

    auto* scroll = new wxScrolledWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetBackgroundColour(wxColour(250, 251, 251));
    scroll->SetScrollRate(0, FromDIP(12));
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    sizer->Add(section_label(scroll, _L("输入内容")), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    sizer->AddSpacer(FromDIP(4));
    auto* input_hint = new wxStaticText(scroll, wxID_ANY,
                                        _L("文字和图片至少提供一项。\n同时提供时，文字用于描述调整方向。"));
    input_hint->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(input_hint, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(6));
    m_prompt_label = new wxStaticText(scroll, wxID_ANY, _L("文字描述（可选）"));
    sizer->Add(m_prompt_label, 0, wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(4));
    m_prompt = new wxTextCtrl(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(50)),
                              wxTE_MULTILINE);
    m_prompt->SetHint(_L("例如：一只坐在圆形底座上的机械猫"));
    sizer->Add(m_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(8));

    auto* style_row = new wxBoxSizer(wxHORIZONTAL);
    auto* style_label = new wxStaticText(scroll, wxID_ANY, _L("风格"));
    wxArrayString styles;
    styles.Add(_L("单色写实"));
    styles.Add(_L("多色写实"));
    styles.Add(_L("多色风格化"));
    m_style = new wxChoice(scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, styles);
    m_style->SetSelection(0);
    style_row->Add(style_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    style_row->Add(m_style, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(style_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(8));
    wxArrayString stylized;
    for (const char* style : {"portrait_sketch", "cartoon", "low_poly", "relief", "ink_relief", "diorama", "custom"})
        stylized.Add(ModelGenerationPresentation::style_label(style));
    m_stylized_style = new wxChoice(scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, stylized);
    m_stylized_style->SetSelection(1);
    m_stylized_style->SetToolTip(_L("选择多色风格化的具体表现方式"));
    sizer->Add(m_stylized_style, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    m_custom_style_panel = new wxPanel(scroll);
    m_custom_style_panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* custom_style_sizer = new wxBoxSizer(wxVERTICAL);
    auto* custom_style_label = new wxStaticText(m_custom_style_panel, wxID_ANY, _L("自定义风格描述"));
    m_custom_style = new wxTextCtrl(m_custom_style_panel, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                    wxSize(-1, FromDIP(50)), wxTE_MULTILINE);
    m_custom_style->SetHint(_L("描述外观即可；系统会保留主体、构图和可见元素"));
    m_custom_style->SetMaxLength(240);
    custom_style_sizer->Add(custom_style_label, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    custom_style_sizer->Add(m_custom_style, 0, wxEXPAND);
    m_custom_style_panel->SetSizer(custom_style_sizer);
    m_custom_style_panel->Hide();
    sizer->Add(m_custom_style_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(8));

    auto* image_row = new wxBoxSizer(wxHORIZONTAL);
    m_choose_image = new wxButton(scroll, wxID_ANY, _L("选择图片"));
    m_clear_image = new wxButton(scroll, wxID_ANY, _L("移除"));
    m_selected_image = new wxStaticText(scroll, wxID_ANY, _L("未选择图片"),
                                        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_selected_image->SetMinSize(wxSize(FromDIP(70), -1));
    image_row->Add(m_choose_image, 0, wxRIGHT, FromDIP(8));
    image_row->Add(m_clear_image, 0, wxRIGHT, FromDIP(8));
    image_row->Add(m_selected_image, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(image_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    m_style_recommendation_panel = new wxPanel(scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    m_style_recommendation_panel->SetBackgroundColour(wxColour(239, 248, 246));
    auto* style_recommendation_sizer = new wxBoxSizer(wxVERTICAL);
    m_style_recommendation_title = new wxStaticText(m_style_recommendation_panel, wxID_ANY, _L("正在推荐风格..."));
    wxFont recommendation_font = m_style_recommendation_title->GetFont();
    recommendation_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_style_recommendation_title->SetFont(recommendation_font);
    m_style_recommendation_title->SetForegroundColour(wxColour(31, 97, 90));
    style_recommendation_sizer->Add(m_style_recommendation_title, 0, wxEXPAND | wxALL, FromDIP(8));
    m_style_recommendation_reason = new wxStaticText(m_style_recommendation_panel, wxID_ANY, wxEmptyString);
    m_style_recommendation_reason->SetForegroundColour(wxColour(75, 91, 94));
    m_style_recommendation_reason->Wrap(FromDIP(290));
    style_recommendation_sizer->Add(m_style_recommendation_reason, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    auto* style_alternatives = new wxBoxSizer(wxHORIZONTAL);
    m_style_recommendation_alternative_label = new wxStaticText(
        m_style_recommendation_panel, wxID_ANY, _L("也可以："));
    style_alternatives->Add(m_style_recommendation_alternative_label, 0,
                            wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    for (wxButton*& button : m_style_recommendation_alternatives) {
        button = new wxButton(m_style_recommendation_panel, wxID_ANY, wxEmptyString,
                              wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        style_alternatives->Add(button, 0, wxRIGHT, FromDIP(6));
    }
    style_recommendation_sizer->Add(style_alternatives, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_style_recommendation_panel->SetSizer(style_recommendation_sizer);
    m_style_recommendation_panel->Hide();
    sizer->AddSpacer(FromDIP(6));
    sizer->Add(m_style_recommendation_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    m_upload_notice = new wxStaticText(scroll, wxID_ANY, _L("仅会将选中的图片和文字描述发送给 AI。"));
    m_upload_notice->Wrap(FromDIP(310));
    m_upload_notice->SetForegroundColour(wxColour(91, 104, 107));
    sizer->AddSpacer(FromDIP(4));
    sizer->Add(m_upload_notice, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(10));

    auto* creation_colors = new wxStaticText(scroll, wxID_ANY,
        _L("生成保留自然颜色和细节；完成后再到 Orca 匹配打印耗材。"));
    creation_colors->Wrap(FromDIP(310));
    sizer->Add(creation_colors, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    auto* legacy_palette_controls = new wxPanel(scroll);
    auto* legacy_palette_sizer = new wxBoxSizer(wxVERTICAL);
    legacy_palette_sizer->Add(section_label(legacy_palette_controls, _L("配色")), 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    legacy_palette_sizer->AddSpacer(FromDIP(6));
    m_use_printable_colors = new wxCheckBox(legacy_palette_controls, wxID_ANY, _L("限制为打印机耗材颜色"));
    m_use_printable_colors->SetValue(false);
    m_use_printable_colors->SetToolTip(_L("开启后只使用下方 1–6 种耗材颜色，生成结果更适合多色打印。"));
    m_use_printable_colors->Hide();
    wxArrayString palette_sources;
    palette_sources.Add(_L("读取耗材颜色"));
    palette_sources.Add(_L("不限制颜色"));
    palette_sources.Add(_L("AI 推荐配色"));
    m_palette_source = new wxChoice(legacy_palette_controls, wxID_ANY, wxDefaultPosition, wxDefaultSize, palette_sources);
    m_palette_source->SetSelection(1);
    legacy_palette_sizer->Add(m_palette_source, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    legacy_palette_sizer->AddSpacer(FromDIP(6));
    m_palette_panel = new wxPanel(legacy_palette_controls);
    m_palette_panel->SetBackgroundColour(wxColour(250, 251, 251));
    m_palette_sizer = new wxGridSizer(6, FromDIP(6), FromDIP(6));
    m_palette_panel->SetSizer(m_palette_sizer);
    legacy_palette_sizer->Add(m_palette_panel, 0, wxLEFT | wxRIGHT, FromDIP(12));
    legacy_palette_sizer->AddSpacer(FromDIP(5));
    m_palette_summary = new wxStaticText(legacy_palette_controls, wxID_ANY, wxEmptyString);
    m_palette_summary->Wrap(FromDIP(310));
    m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    legacy_palette_sizer->Add(m_palette_summary, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    legacy_palette_sizer->AddSpacer(FromDIP(6));
    m_custom_color_panel = new wxPanel(legacy_palette_controls);
    m_custom_color_panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* custom_color_row = new wxBoxSizer(wxHORIZONTAL);
    m_custom_color = new wxColourPickerCtrl(m_custom_color_panel, wxID_ANY, *wxWHITE);
    m_custom_color->SetName("ai_content_color");
    m_add_custom_color = new wxButton(m_custom_color_panel, wxID_ANY, _L("添加颜色"));
    custom_color_row->Add(m_custom_color, 1, wxRIGHT, FromDIP(8));
    custom_color_row->Add(m_add_custom_color, 0);
    m_custom_color_panel->SetSizer(custom_color_row);
    legacy_palette_sizer->Add(m_custom_color_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    legacy_palette_sizer->AddSpacer(FromDIP(6));

    m_palette_recommendation_panel = new wxPanel(legacy_palette_controls, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    m_palette_recommendation_panel->SetBackgroundColour(*wxWHITE);
    auto* recommendation_sizer = new wxBoxSizer(wxVERTICAL);
    auto* recommendation_actions = new wxBoxSizer(wxVERTICAL);
    auto* color_count_row = new wxBoxSizer(wxHORIZONTAL);
    auto* color_count_label = new wxStaticText(m_palette_recommendation_panel, wxID_ANY, _L("推荐颜色数量"));
    wxArrayString color_counts;
    for (size_t count = Slic3r::AI::kMinTargetPaletteColors; count <= Slic3r::AI::kMaxTargetPaletteColors; ++count) {
        color_counts.Add(wxString::Format(_L("%llu 色"), static_cast<unsigned long long>(count)));
    }
    m_palette_color_count = new wxChoice(m_palette_recommendation_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, color_counts);
    m_palette_color_count->SetSelection(static_cast<int>(Slic3r::AI::kLegacyDefaultTargetPaletteColors - Slic3r::AI::kMinTargetPaletteColors));
    m_palette_color_count->SetToolTip(_L("选择 AI 本次推荐的设计目标色数量；它不等同于物理进料通道数。"));
    color_count_row->Add(color_count_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    color_count_row->Add(m_palette_color_count, 0, wxALIGN_CENTER_VERTICAL);
    recommendation_actions->Add(color_count_row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    m_recommend_palette = new wxButton(m_palette_recommendation_panel, wxID_ANY, _L("AI 推荐配色"));
    m_recommend_palette->SetToolTip(_L("根据文字、参考图和风格推荐一组设计目标色；不会修改打印机耗材槽"));
    m_confirm_recommended_palette = new wxButton(
        m_palette_recommendation_panel, wxID_ANY, _L("确认配色并生成预览"));
    recommendation_actions->Add(m_recommend_palette, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    recommendation_actions->Add(m_confirm_recommended_palette, 0, wxEXPAND);
    recommendation_sizer->Add(recommendation_actions, 0, wxEXPAND | wxALL, FromDIP(8));
    m_palette_recommendation_summary = new wxStaticText(
        m_palette_recommendation_panel, wxID_ANY,
        _L("AI 会推荐理想目标色；确认后再由你匹配实际耗材。"));
    m_palette_recommendation_summary->SetForegroundColour(wxColour(91, 104, 107));
    m_palette_recommendation_summary->Wrap(FromDIP(300));
    recommendation_sizer->Add(
        m_palette_recommendation_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    for (size_t index = 0; index < m_palette_recommendation_cards.size(); ++index) {
        auto* card = new wxPanel(m_palette_recommendation_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        card->SetBackgroundColour(wxColour(250, 251, 251));
        auto* card_sizer = new wxBoxSizer(wxVERTICAL);
        auto* content = new wxBoxSizer(wxHORIZONTAL);
        auto* swatch = new wxPanel(card, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(30, 30)), wxBORDER_SIMPLE);
        swatch->SetMinSize(FromDIP(wxSize(30, 30)));
        auto* details = new wxStaticText(card, wxID_ANY, wxEmptyString);
        details->Wrap(FromDIP(230));
        auto* replace = new wxButton(
            card, wxID_ANY, _L("替换"), wxDefaultPosition, FromDIP(wxSize(52, 28)), wxBU_EXACTFIT);
        auto* remove = new wxButton(
            card, wxID_ANY, _L("删除"), wxDefaultPosition, FromDIP(wxSize(52, 28)), wxBU_EXACTFIT);
        content->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(6));
        content->Add(details, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(6));
        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        actions->AddStretchSpacer();
        actions->Add(replace, 0, wxRIGHT, FromDIP(4));
        actions->Add(remove, 0);
        actions->AddSpacer(FromDIP(20));
        card_sizer->Add(content, 0, wxEXPAND);
        card_sizer->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
        card->SetSizer(card_sizer);
        recommendation_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
        m_palette_recommendation_cards[index] = card;
        m_palette_recommendation_swatches[index] = swatch;
        m_palette_recommendation_details[index] = details;
        m_palette_recommendation_replace[index] = replace;
        m_palette_recommendation_remove[index] = remove;
        card->Hide();
        replace->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) { replace_recommended_color(index); });
        remove->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (index < m_custom_palette.size())
                remove_custom_color(m_custom_palette[index]);
        });
    }
    m_palette_recommendation_panel->SetSizer(recommendation_sizer);
    m_palette_recommendation_panel->Hide();
    legacy_palette_sizer->Add(m_palette_recommendation_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    legacy_palette_sizer->AddSpacer(FromDIP(6));

    m_advanced_toggle = new wxButton(legacy_palette_controls, wxID_ANY, _L("显示高级设置"), wxDefaultPosition,
                                     wxSize(-1, FromDIP(30)), wxBU_LEFT);
    m_advanced_toggle->SetToolTip(_L("显示颜色用途、打印尺寸和最小色块设置。"));
    legacy_palette_sizer->Add(m_advanced_toggle, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    legacy_palette_sizer->AddSpacer(FromDIP(6));

    m_advanced_options = new wxPanel(legacy_palette_controls);
    m_advanced_options->SetBackgroundColour(wxColour(250, 251, 251));
    auto* advanced = m_advanced_options;
    auto* advanced_sizer = new wxBoxSizer(wxVERTICAL);

    auto* palette_roles_label = new wxStaticText(advanced, wxID_ANY, _L("颜色用途"));
    wxFont palette_roles_font = palette_roles_label->GetFont();
    palette_roles_font.SetWeight(wxFONTWEIGHT_BOLD);
    palette_roles_label->SetFont(palette_roles_font);
    advanced_sizer->Add(palette_roles_label, 0, wxEXPAND);
    auto* palette_roles_hint = new wxStaticText(advanced, wxID_ANY, _L("系统已自动分配；只有效果不理想时才调整。"));
    palette_roles_hint->SetForegroundColour(wxColour(91, 104, 107));
    advanced_sizer->Add(palette_roles_hint, 0, wxEXPAND | wxTOP, FromDIP(4));
    advanced_sizer->AddSpacer(FromDIP(5));

    m_palette_roles_panel = new wxPanel(advanced);
    m_palette_roles_panel->SetBackgroundColour(advanced->GetBackgroundColour());
    auto* palette_roles_sizer = new wxBoxSizer(wxVERTICAL);
    const std::array<wxString, Slic3r::AI::kMaxTargetPaletteColors> role_labels {
        _L("主色"), _L("轮廓 / 暗部"), _L("浅色"), _L("点缀色"), _L("辅助色"), _L("细节色") };
    for (size_t index = 0; index < m_palette_role_choices.size(); ++index) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(m_palette_roles_panel, wxID_ANY, role_labels[index]), 0,
                 wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        m_palette_role_choices[index] = new wxChoice(m_palette_roles_panel, wxID_ANY);
        row->Add(m_palette_role_choices[index], 1, wxALIGN_CENTER_VERTICAL);
        palette_roles_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    }
    m_palette_roles_panel->SetSizer(palette_roles_sizer);
    advanced_sizer->Add(m_palette_roles_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(6));

    auto* print_constraints_label = new wxStaticText(advanced, wxID_ANY, _L("参考图细节简化（不修改模型尺寸或设备）"));
    wxFont print_constraints_font = print_constraints_label->GetFont();
    print_constraints_font.SetWeight(wxFONTWEIGHT_BOLD);
    print_constraints_label->SetFont(print_constraints_font);
    advanced_sizer->Add(print_constraints_label, 0, wxEXPAND | wxTOP, FromDIP(4));
    const auto add_print_number = [this, advanced, advanced_sizer](const wxString& label, wxSpinCtrlDouble*& control,
                                                                  double value, double minimum, double maximum,
                                                                  double increment, int digits) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(advanced, wxID_ANY, label), 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        control = new wxSpinCtrlDouble(advanced, wxID_ANY);
        control->SetRange(minimum, maximum);
        control->SetIncrement(increment);
        control->SetDigits(digits);
        control->SetValue(value);
        row->Add(control, 0, wxALIGN_CENTER_VERTICAL);
        advanced_sizer->Add(row, 0, wxEXPAND | wxTOP, FromDIP(5));
    };
    add_print_number(_L("参考图处理宽度（mm）"), m_print_width, 160.0, 20.0, 2000.0, 10.0, 1);
    m_print_width->SetToolTip(_L("仅用于参考图的色块简化。实际模型高度请在准备页“智能切片”的“调整当前模型”中设置。"));
    add_print_number(_L("喷嘴直径（mm）"), m_nozzle_size, 0.4, 0.1, 2.0, 0.1, 2);
    add_print_number(_L("挤出线宽（mm）"), m_line_width, 0.4, 0.1, 3.0, 0.05, 2);
    add_print_number(_L("最小特征（mm）"), m_minimum_feature, 0.8, 0.1, 20.0, 0.1, 2);
    m_minimum_feature->SetToolTip(_L("建议不小于两条挤出线宽；过小色块会合并到相邻主色块。"));
    advanced->SetSizer(advanced_sizer);
    m_advanced_options->Hide();
    legacy_palette_sizer->Add(m_advanced_options, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    legacy_palette_sizer->AddSpacer(FromDIP(12));

    legacy_palette_controls->SetSizer(legacy_palette_sizer);
    legacy_palette_controls->Hide();

    m_model_settings_panel = new wxPanel(scroll);
    m_model_settings_panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* model_settings_sizer = new wxBoxSizer(wxVERTICAL);
    model_settings_sizer->Add(section_label(m_model_settings_panel, _L("3D 生成设置")), 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* quality_row = new wxBoxSizer(wxHORIZONTAL);
    auto* quality_label = new wxStaticText(m_model_settings_panel, wxID_ANY, _L("生成策略"));
    wxArrayString quality_levels;
    quality_levels.Add(_L("高质量（推荐）"));
    quality_levels.Add(_L("高性能"));
    m_quality = new wxChoice(m_model_settings_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, quality_levels);
    m_quality->SetSelection(0);
    m_quality->SetToolTip(
        _L("高质量：100 万面目标、标准几何和标准纹理。高性能：30 万面目标，缩短处理时间。两种模式均使用已确认的 AI 设计图，不自动裁切。"));
    quality_row->Add(quality_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    quality_row->Add(m_quality, 1, wxALIGN_CENTER_VERTICAL);
    model_settings_sizer->Add(quality_row, 0, wxEXPAND);
    m_model_settings_panel->SetSizer(model_settings_sizer);
    sizer->Insert(0, m_model_settings_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_import_settings_panel = new wxPanel(scroll);
    m_import_settings_panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* import_settings_sizer = new wxBoxSizer(wxVERTICAL);
    import_settings_sizer->Add(section_label(m_import_settings_panel, _L("导入设置")), 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* import_color_row = new wxBoxSizer(wxHORIZONTAL);
    auto* import_color_label = new wxStaticText(m_import_settings_panel, wxID_ANY, _L("颜色处理"));
    wxArrayString import_color_modes;
    import_color_modes.Add(_L("完整颜色匹配（支持叠色，推荐）"));
    import_color_modes.Add(_L("自动匹配当前耗材"));
    import_color_modes.Add(_L("单色导入"));
    import_color_modes.Add(_L("简单匹配耗材槽"));
    m_import_color_mode = new wxChoice(m_import_settings_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, import_color_modes);
    m_import_color_mode->SetSelection(0);
    m_import_color_mode->SetToolTip(
        _L("默认打开完整颜色匹配窗口，可预览并选择叠色方案后确认导入；自动匹配仅使用当前物理耗材；单色导入忽略模型颜色。"));
    import_color_row->Add(import_color_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    import_color_row->Add(m_import_color_mode, 1, wxALIGN_CENTER_VERTICAL);
    import_settings_sizer->Add(import_color_row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));

    m_import_settings_panel->SetSizer(import_settings_sizer);
    sizer->Insert(0, m_import_settings_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_preprocess_section = section_label(scroll, _L("确认提示词"));
    sizer->Add(m_preprocess_section, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_prepared_prompt_label = new wxStaticText(scroll, wxID_ANY, _L("用于 3D 生成的提示词"));
    sizer->Add(m_prepared_prompt_label, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_prepared_prompt = new wxTextCtrl(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(72)), wxTE_MULTILINE);
    sizer->Add(m_prepared_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    scroll->SetSizer(sizer);
    scroll->FitInside();
    outer->Add(scroll, 1, wxEXPAND);

    auto* action_panel = new wxPanel(panel);
    action_panel->SetBackgroundColour(*wxWHITE);
    auto* action_panel_sizer = new wxBoxSizer(wxVERTICAL);
    auto* status_row = new wxBoxSizer(wxHORIZONTAL);
    m_status = new wxStaticText(action_panel, wxID_ANY, _L("空闲"));
    m_status->Wrap(FromDIP(310));
    m_workflow_steps->Wrap(FromDIP(330));
    m_workflow_steps->InvalidateBestSize();
    m_status->SetForegroundColour(wxColour(60, 75, 78));
    auto* open_diagnostics = new wxButton(action_panel, wxID_ANY, _L("打开诊断日志"));
    open_diagnostics->SetToolTip(_L("打开 AI 后台日志目录；反馈问题时请发送 orca-ai-sidecar.log 和诊断 ID"));
    status_row->Add(m_status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    status_row->Add(open_diagnostics, 0, wxALIGN_CENTER_VERTICAL);
    action_panel_sizer->Add(status_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    auto* action_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_preprocess = new wxButton(action_panel, wxID_ANY, _L("生成图片预览"),
                                wxDefaultPosition, wxSize(-1, FromDIP(38)));
    m_generate = new wxButton(action_panel, wxID_ANY, _L("确认并生成 3D"),
                              wxDefaultPosition, wxSize(-1, FromDIP(38)));
    m_stop = new wxButton(action_panel, wxID_ANY, _L("停止生成"),
                          wxDefaultPosition, wxSize(-1, FromDIP(38)));
    m_retry_service = new wxButton(action_panel, wxID_ANY, _L("重新检测服务"),
                                   wxDefaultPosition, wxSize(-1, FromDIP(38)));
    m_import = new wxButton(action_panel, wxID_ANY, _L("导入到准备页"),
                            wxDefaultPosition, wxSize(-1, FromDIP(38)));
    m_discard = new wxButton(action_panel, wxID_ANY, _L("重新开始"),
                             wxDefaultPosition, wxSize(-1, FromDIP(38)));
    action_buttons->Add(m_preprocess, 1, wxRIGHT, FromDIP(8));
    action_buttons->Add(m_generate, 1, wxRIGHT, FromDIP(8));
    action_buttons->Add(m_stop, 1, wxRIGHT, FromDIP(8));
    action_buttons->Add(m_retry_service, 1, wxRIGHT, FromDIP(8));
    action_buttons->Add(m_import, 1, wxRIGHT, FromDIP(8));
    action_buttons->Add(m_discard, 0);
    action_panel_sizer->Add(action_buttons, 0, wxEXPAND | wxALL, FromDIP(12));
    auto* back_to_prepare = new wxButton(action_panel, wxID_ANY, _L("返回当前工程 · 准备页"));
    back_to_prepare->SetToolTip(_L("继续编辑当前工程；生成任务和本页输入会保留。"));
    back_to_prepare->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_prepare_navigation) m_prepare_navigation();
    });
    action_panel_sizer->Add(back_to_prepare, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    action_panel->SetSizer(action_panel_sizer);
    outer->Add(action_panel, 0, wxEXPAND);
    panel->SetSizer(outer);

    m_prompt->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refresh_controls(); });
    m_style->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        select_style(current_style(), true);
    });
    m_stylized_style->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { select_style(current_style(), true); });
    for (size_t index = 0; index < m_style_recommendation_alternatives.size(); ++index) {
        m_style_recommendation_alternatives[index]->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (index < m_style_recommendation.alternatives.size())
                select_style(m_style_recommendation.alternatives[index], true);
        });
    }
    m_custom_style->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refresh_controls(); });
    m_quality->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_controls(); });
    m_choose_image->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_choose_image, this);
    m_clear_image->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_clear_image, this);
    m_use_printable_colors->Bind(wxEVT_CHECKBOX, &ModelGenerationPanel::on_printable_colors_toggled, this);
    m_palette_source->Bind(wxEVT_CHOICE, &ModelGenerationPanel::on_palette_source_changed, this);
    m_palette_color_count->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_controls(); });
    m_import_color_mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_controls(); });
    m_add_custom_color->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_add_custom_color, this);
    m_recommend_palette->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_recommend_palette, this);
    m_confirm_recommended_palette->Bind(
        wxEVT_BUTTON, &ModelGenerationPanel::on_confirm_recommended_palette, this);
    for (size_t index = 0; index < m_palette_role_choices.size(); ++index)
        m_palette_role_choices[index]->Bind(wxEVT_CHOICE, [this, index](wxCommandEvent&) { on_palette_role_changed(index); });
    for (wxSpinCtrlDouble* control : {m_print_width, m_nozzle_size, m_line_width, m_minimum_feature})
        control->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_controls(); });
    m_advanced_toggle->Bind(wxEVT_BUTTON, [this, scroll](wxCommandEvent&) {
        m_advanced_options_expanded = !m_advanced_options_expanded;
        m_advanced_options->Show(m_advanced_options_expanded);
        m_advanced_toggle->SetLabel(m_advanced_options_expanded ? _L("收起高级设置") : _L("显示高级设置"));
        scroll->Layout();
        scroll->FitInside();
    });
    m_preprocess->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_preprocess, this);
    m_generate->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_generate, this);
    m_stop->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_stop, this);
    m_retry_service->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_retry_service, this);
    m_import->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_import, this);
    m_discard->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_discard, this);
    open_diagnostics->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const boost::filesystem::path log_directory = boost::filesystem::path(Slic3r::data_dir()) / "log";
        boost::system::error_code ec;
        boost::filesystem::create_directories(log_directory, ec);
        wxString path = from_path(log_directory);
        if (!path.empty() && !wxFileName::IsPathSeparator(path.Last()))
            path += wxFileName::GetPathSeparator();
        if (ec || !wxLaunchDefaultApplication(path))
            show_error(this, _L("无法打开诊断日志目录：") + from_path(log_directory));
    });
    return panel;
}

wxWindow* ModelGenerationPanel::build_preview_panel(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    panel->SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    header->Add(section_label(panel, _L("预览结果")), 1, wxALIGN_CENTER_VERTICAL);
    m_preview_kind = new wxStaticText(panel, wxID_ANY, _L("结果对照"));
    m_preview_kind->SetForegroundColour(wxColour(91, 104, 107));
    m_preview_details_pane = new wxCollapsiblePane(panel, wxID_ANY, _L("多视图"), wxDefaultPosition,
        wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    wxWindow* preview_details_parent = m_preview_details_pane->GetPane();
    wxArrayString preview_stages;
    preview_stages.Add(_L("AI 设计图"));
    preview_stages.Add(_L("模型多视图"));
    m_preview_stage = new wxChoice(preview_details_parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, preview_stages);
    m_preview_stage->SetSelection(0);
    m_preview_stage_hint = new wxStaticText(
        panel, wxID_ANY, _L("生成后可在这里确认图片效果。"));
    m_preview_stage_hint->SetForegroundColour(wxColour(91, 104, 107));
    m_preview_stage_hint->Wrap(FromDIP(760));
    m_zoom_out = new wxButton(panel, wxID_ANY, "-", wxDefaultPosition, wxSize(FromDIP(30), FromDIP(28)));
    m_zoom_fit = new wxButton(panel, wxID_ANY, _L("图片适应"), wxDefaultPosition, wxDefaultSize);
    m_zoom_in = new wxButton(panel, wxID_ANY, "+", wxDefaultPosition, wxSize(FromDIP(30), FromDIP(28)));
    m_preview_zoom = new wxStaticText(panel, wxID_ANY, "100%", wxDefaultPosition, wxSize(FromDIP(48), -1), wxALIGN_CENTER_HORIZONTAL);
    m_zoom_out->SetToolTip(_L("缩小图片预览"));
    m_zoom_fit->SetToolTip(_L("完整显示图片"));
    m_zoom_in->SetToolTip(_L("放大图片预览"));
    header->Add(m_preview_kind, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    header->Add(m_zoom_out, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    header->Add(m_zoom_fit, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    header->Add(m_zoom_in, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    header->Add(m_preview_zoom, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    sizer->Add(header, 0, wxEXPAND | wxALL, FromDIP(18));
    sizer->Add(m_preview_stage_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));
    auto* preview_stage_row = new wxBoxSizer(wxHORIZONTAL);
    preview_stage_row->Add(new wxStaticText(preview_details_parent, wxID_ANY, _L("查看")), 0,
                           wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    preview_stage_row->Add(m_preview_stage, 0, wxALIGN_CENTER_VERTICAL);
    m_preview_technical_details = new wxStaticText(
        preview_details_parent, wxID_ANY, _L("多视图来自生成模型，用于查看不同角度的形体。"));
    m_preview_technical_details->SetForegroundColour(wxColour(91, 104, 107));
    m_preview_technical_details->Wrap(FromDIP(740));
    auto* preview_details_sizer = new wxBoxSizer(wxVERTICAL);
    preview_details_sizer->Add(preview_stage_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    preview_details_sizer->Add(m_preview_technical_details, 0, wxEXPAND | wxALL, FromDIP(8));
    preview_details_parent->SetSizerAndFit(preview_details_sizer);
    m_preview_details_pane->Collapse(true);
    m_preview_details_pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [panel, preview_details_parent](wxCollapsiblePaneEvent& event) {
        preview_details_parent->Layout();
        panel->Layout();
        if (panel->GetParent() != nullptr)
            panel->GetParent()->Layout();
        event.Skip();
    });
    sizer->Add(m_preview_details_pane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));

    m_preview_book = new wxNotebook(panel, wxID_ANY);
    auto* model_page = new ModelResultScrolledWindow(
        m_preview_book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    model_page->SetBackgroundColour(wxColour(241, 244, 245));
    model_page->SetScrollRate(FromDIP(12), FromDIP(12));
    auto* model_sizer = new wxBoxSizer(wxVERTICAL);
    auto* comparison_panel = new wxPanel(model_page);
    m_comparison_panel = comparison_panel;
    m_model_page = model_page;
    comparison_panel->SetBackgroundColour(wxColour(241, 244, 245));
    auto* comparison_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_preview_area = new wxScrolledWindow(
        comparison_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    m_preview_area->SetBackgroundColour(wxColour(241, 244, 245));
    m_preview_area->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_preview_area->SetMinSize(wxSize(FromDIP(420), FromDIP(320)));
    m_preview_area->SetScrollRate(FromDIP(12), FromDIP(12));
    m_preview_area->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(m_preview_area);
        dc.SetBackground(wxBrush(m_preview_area->GetBackgroundColour()));
        dc.Clear();
        if (m_reference_preview_pane.IsEmpty() && m_style_preview_pane.IsEmpty())
            return;
        int view_x = 0;
        int view_y = 0;
        int unit_x = 1;
        int unit_y = 1;
        m_preview_area->GetViewStart(&view_x, &view_y);
        m_preview_area->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const wxPoint offset(view_x * unit_x, view_y * unit_y);
        const int label_height = FromDIP(32);

        auto draw_pane = [&](const wxRect& virtual_rect, const wxString& label, const wxBitmap& bitmap,
                             const wxString& placeholder, bool ai_result) {
            if (virtual_rect.IsEmpty())
                return;
            wxRect rect = virtual_rect;
            rect.Offset(-offset.x, -offset.y);
            dc.SetPen(wxPen(wxColour(204, 213, 215)));
            dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
            dc.DrawRectangle(rect);

            const wxRect label_rect(rect.x, rect.y, rect.width, label_height);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(ai_result ? wxColour(229, 244, 242) : wxColour(235, 239, 240)));
            dc.DrawRectangle(label_rect);
            wxFont label_font = dc.GetFont();
            label_font.SetWeight(wxFONTWEIGHT_BOLD);
            dc.SetFont(label_font);
            dc.SetTextForeground(ai_result ? wxColour(24, 112, 105) : wxColour(60, 75, 78));
            const wxSize label_size = dc.GetTextExtent(label);
            dc.DrawText(label, label_rect.x + FromDIP(10), label_rect.y + (label_rect.height - label_size.y) / 2);

            const wxRect image_rect(rect.x, rect.y + label_height, rect.width, rect.height - label_height);
            if (bitmap.IsOk()) {
                const int x = image_rect.x + (image_rect.width - bitmap.GetWidth()) / 2;
                const int y = image_rect.y + (image_rect.height - bitmap.GetHeight()) / 2;
                // Paint beneath alpha in either pane; never composite it into
                // the reference or generation file.
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(wxColour(160, 160, 160)));
                dc.DrawRectangle(x, y, bitmap.GetWidth(), bitmap.GetHeight());
                dc.DrawBitmap(bitmap, x, y, true);
            } else if (!placeholder.empty()) {
                wxFont placeholder_font = dc.GetFont();
                placeholder_font.SetWeight(wxFONTWEIGHT_NORMAL);
                dc.SetFont(placeholder_font);
                dc.SetTextForeground(wxColour(108, 120, 123));
                const wxSize text_size = dc.GetTextExtent(placeholder);
                dc.DrawText(placeholder,
                            image_rect.x + std::max(FromDIP(8), (image_rect.width - text_size.x) / 2),
                            image_rect.y + std::max(FromDIP(8), (image_rect.height - text_size.y) / 2));
            }
        };

        const wxString reference_placeholder = m_library_model_loaded
            ? _L("该历史记录未保存原图")
            : _L("文字生成，无原图");
        draw_pane(m_reference_preview_pane, _L("原图"), m_reference_bitmap, reference_placeholder, false);
        const wxString result_label = m_preview_stage != nullptr && m_preview_stage->GetSelection() != wxNOT_FOUND
            ? m_preview_stage->GetStringSelection() : _L("AI 生成图");
        draw_pane(m_style_preview_pane, result_label, m_style_preview_bitmap, m_style_preview_placeholder, true);
    });
    m_preview_area->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        update_preview_view();
        event.Skip();
    });
    auto* model_card = new wxPanel(comparison_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    model_card->SetBackgroundColour(*wxWHITE);
    // The card's explicit minimum is what the outer comparison row uses; it
    // must include the viewport plus the color controls, not just the old canvas.
    model_card->SetMinSize(wxSize(FromDIP(440), FromDIP(560)));
    auto* model_card_sizer = new wxBoxSizer(wxVERTICAL);
    auto* model_toolbar = new wxBoxSizer(wxHORIZONTAL);
    m_model_stats = new wxStaticText(model_card, wxID_ANY, _L("3D 模型 · 生成后将在这里显示"));
    m_model_stats->SetForegroundColour(wxColour(91, 104, 107));
    m_front_model_view = new wxButton(model_card, wxID_ANY, _L("完整显示模型"));
    m_front_model_view->SetToolTip(_L("恢复规范正面并自动适应画布大小"));
    m_reset_model_view = new wxButton(model_card, wxID_ANY, _L("三维视角"));
    m_reset_model_view->SetToolTip(_L("恢复便于检查侧面和底座的三维观察角度"));
    model_toolbar->Add(m_model_stats, 1, wxALIGN_CENTER_VERTICAL);
    model_toolbar->Add(m_front_model_view, 0, wxLEFT, FromDIP(8));
    model_toolbar->Add(m_reset_model_view, 0, wxLEFT, FromDIP(6));
    model_card_sizer->Add(model_toolbar, 0, wxEXPAND | wxALL, FromDIP(10));
    m_finishing_compare_model = new wxButton(model_card, wxID_ANY, _L("查看处理前"));
    m_finishing_compare_model->Hide();
    model_card_sizer->Add(m_finishing_compare_model, 0, wxLEFT | wxBOTTOM, FromDIP(10));
    m_model_preview = new ModelPreview3D(model_card);
    m_model_preview->SetMinSize(wxSize(FromDIP(420), FromDIP(280)));
    model_card_sizer->Add(m_model_preview, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    model_card->SetSizer(model_card_sizer);
    comparison_sizer->Add(model_card, 5, wxEXPAND | wxRIGHT, FromDIP(10));
    comparison_sizer->Add(m_preview_area, 4, wxEXPAND);
    comparison_sizer->Add(build_model_finishing(comparison_panel), 0, wxEXPAND | wxLEFT, FromDIP(10));
    comparison_panel->SetSizer(comparison_sizer);
    auto* expand_images = m_expand_images = new wxToggleButton(panel, wxID_ANY, _L("展开图片"));
    header->Add(expand_images, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    expand_images->Bind(wxEVT_TOGGLEBUTTON, [this, model_card, comparison_panel, model_page, expand_images](wxCommandEvent&) {
        model_card->Show(!expand_images->GetValue());
        expand_images->SetLabel(expand_images->GetValue() ? _L("返回模型对照") : _L("展开图片"));
        comparison_panel->Layout();
        model_page->FitInside();
        update_preview_view(true);
    });
    comparison_panel->Bind(wxEVT_SIZE, [this, comparison_panel, comparison_sizer, model_card](wxSizeEvent& event) {
        const bool stacked = !m_finishing_workbench && comparison_panel->GetClientSize().x < FromDIP(920);
        const int orientation = stacked ? wxVERTICAL : wxHORIZONTAL;
        if (comparison_sizer->GetOrientation() != orientation) {
            comparison_sizer->SetOrientation(orientation);
            comparison_sizer->GetItem(model_card)->SetFlag(wxEXPAND | (stacked ? wxBOTTOM : wxRIGHT));
            comparison_panel->Layout();
            if (auto* page = dynamic_cast<wxScrolledWindow*>(comparison_panel->GetParent())) page->FitInside();
        }
        event.Skip();
    });
    model_sizer->Add(comparison_panel, 1, wxEXPAND | wxALL, FromDIP(12));
    auto* finishing_shortcut = m_finishing_shortcut = new wxButton(panel, wxID_ANY, _L("3D 美颜工作台"));
    header->Add(finishing_shortcut, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    finishing_shortcut->Bind(wxEVT_BUTTON, [this, model_page](wxCommandEvent&) {
        if (!m_model_preview_ready && !m_finishing_workbench) return;
        set_finishing_workbench(!m_finishing_workbench);
    });

    m_model_decision_panel = new wxPanel(
        model_page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    auto* decision_sizer = new wxBoxSizer(wxVERTICAL);
    m_model_decision_status = new wxStaticText(m_model_decision_panel, wxID_ANY, _L("尚未检查"));
    wxFont decision_font = m_model_decision_status->GetFont();
    decision_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_model_decision_status->SetFont(decision_font);
    m_model_decision_summary = new wxStaticText(
        m_model_decision_panel, wxID_ANY, _L("模型生成或加载后会显示是否适合继续导入。"));
    m_model_decision_summary->Wrap(FromDIP(620));
    m_model_decision_status->Hide();
    m_model_decision_summary->Hide();
    m_model_decision_panel->SetSizer(decision_sizer);
    m_model_decision_panel->Hide();
    wxWindow* model_advanced_parent = model_page;
    m_local_recolor_panel = new wxPanel(
        m_finishing_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_local_recolor_panel->SetMaxSize(wxSize(FromDIP(280), -1));
    m_local_recolor_panel->SetBackgroundColour(*wxWHITE);
    auto* recolor_sizer = new wxBoxSizer(wxVERTICAL);
    auto* recolor_header = new wxBoxSizer(wxVERTICAL);
    auto* recolor_title = new wxStaticText(m_local_recolor_panel, wxID_ANY, _L("局部改色"));
    wxFont recolor_title_font = recolor_title->GetFont();
    recolor_title_font.SetWeight(wxFONTWEIGHT_BOLD);
    recolor_title->SetFont(recolor_title_font);
    m_local_recolor_toggle = new wxToggleButton(
        m_local_recolor_panel, wxID_ANY, _L("开始改色"));
    m_local_recolor_toggle->SetMinSize(wxSize(FromDIP(118), FromDIP(34)));
    m_local_recolor_toggle->SetToolTip(_L("打开局部改色工具，在模型上直接选择需要换色的部位"));
    auto* recolor_intro = new wxStaticText(
        m_local_recolor_panel, wxID_ANY, _L("范围确定后，整片改成指定耗材色"));
    recolor_intro->SetForegroundColour(wxColour(91, 104, 107));
    recolor_header->Add(recolor_title, 0, wxEXPAND);
    recolor_header->Add(recolor_intro, 0, wxEXPAND | wxTOP, FromDIP(6));
    recolor_header->Add(m_local_recolor_toggle, 0, wxEXPAND);
    recolor_sizer->Add(recolor_header, 0, wxEXPAND | wxALL, FromDIP(10));

    m_local_recolor_controls = new wxPanel(m_local_recolor_panel);
    m_local_recolor_controls->SetBackgroundColour(*wxWHITE);
    auto* controls_sizer = new wxBoxSizer(wxVERTICAL);
    controls_sizer->Add(new wxStaticText(m_local_recolor_controls, wxID_ANY, _L("选择目标耗材色")), 0,
                        wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    auto* color_grid = new wxGridSizer(2, FromDIP(6), FromDIP(6));
    for (size_t index = 0; index < m_region_color_buttons.size(); ++index) {
        m_region_color_buttons[index] = new wxToggleButton(
            m_local_recolor_controls, wxID_ANY,
            wxString::Format(_L("耗材 %llu"), static_cast<unsigned long long>(index + 1)));
        m_region_color_buttons[index]->SetMinSize(wxSize(FromDIP(88), FromDIP(40)));
        color_grid->Add(m_region_color_buttons[index], 1, wxEXPAND);
    }
    controls_sizer->Add(color_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    m_apply_region_color = new wxButton(
        m_local_recolor_controls, wxID_ANY, _L("选择部位后应用"));
    m_apply_region_color->SetMinSize(wxSize(FromDIP(140), FromDIP(40)));
    controls_sizer->Add(m_apply_region_color, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    auto* recolor_hint = new wxStaticText(
        m_local_recolor_controls, wxID_ANY,
        _L("短按选择 · 拖动旋转\n滚轮缩放"));
    recolor_hint->SetForegroundColour(wxColour(91, 104, 107));
    controls_sizer->Add(recolor_hint, 0, wxEXPAND | wxALL, FromDIP(10));
    m_local_recolor_controls->SetSizer(controls_sizer);
    m_local_recolor_controls->Hide();
    recolor_sizer->Add(m_local_recolor_controls, 0, wxEXPAND);
    m_local_recolor_panel->SetSizer(recolor_sizer);
    m_finishing_panel->GetSizer()->Add(m_local_recolor_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    m_local_recolor_panel->Hide();
    m_model_quality_panel = new wxPanel(
        model_advanced_parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    auto* quality_sizer = new wxBoxSizer(wxVERTICAL);
    auto* quality_header = new wxBoxSizer(wxHORIZONTAL);
    m_model_quality_status = new wxStaticText(m_model_quality_panel, wxID_ANY, _L("尚未检查"));
    wxFont quality_font = m_model_quality_status->GetFont();
    quality_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_model_quality_status->SetFont(quality_font);
    m_recheck_model = new wxButton(m_model_quality_panel, wxID_ANY, _L("重新检查"));
    m_recheck_model->SetToolTip(_L("使用本地结构门禁重新检查当前模型，不会调用付费 AI"));
    m_locate_thin_regions = new wxButton(m_model_quality_panel, wxID_ANY, _L("定位薄壁"));
    m_locate_thin_regions->SetToolTip(
        _L("高亮本地厚度采样命中的薄壁面片；结果用于复核，不会自动修改模型"));
    m_locate_overhang_regions = new wxButton(m_model_quality_panel, wxID_ANY, _L("定位悬垂面"));
    m_locate_overhang_regions->SetToolTip(
        _L("高亮显著的离床向下面，便于旋转检查；不会自动添加支撑或改变切片参数"));
    quality_header->Add(m_model_quality_status, 1, wxALIGN_CENTER_VERTICAL);
    quality_header->Add(m_locate_thin_regions, 0, wxLEFT, FromDIP(12));
    quality_header->Add(m_locate_overhang_regions, 0, wxLEFT, FromDIP(12));
    quality_header->Add(m_recheck_model, 0, wxLEFT, FromDIP(12));
    quality_sizer->Add(quality_header, 0, wxEXPAND | wxALL, FromDIP(10));
    m_model_quality_summary = new wxStaticText(m_model_quality_panel, wxID_ANY, _L("模型生成或加载后可进行结构检查。"));
    m_model_quality_summary->Wrap(FromDIP(500));
    quality_sizer->Add(m_model_quality_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_model_quality_details_pane = new wxCollapsiblePane(m_model_quality_panel, wxID_ANY, _L("查看检查指标"), wxDefaultPosition,
        wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    auto* quality_details_sizer = new wxBoxSizer(wxVERTICAL);
    m_model_quality_details = new wxStaticText(m_model_quality_details_pane->GetPane(), wxID_ANY, wxEmptyString);
    m_model_quality_details->Wrap(FromDIP(480));
    quality_details_sizer->Add(m_model_quality_details, 0, wxEXPAND | wxALL, FromDIP(8));
    m_model_quality_details_pane->GetPane()->SetSizer(quality_details_sizer);
    quality_sizer->Add(m_model_quality_details_pane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_model_quality_details_pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this, model_page](wxCollapsiblePaneEvent& event) {
        m_model_quality_panel->Layout();
        model_page->Layout();
        model_page->FitInside();
        if (m_model_preview != nullptr)
            m_model_preview->refresh();
        event.Skip();
    });
    auto* visual_header = new wxBoxSizer(wxHORIZONTAL);
    m_visual_quality_status = new wxStaticText(m_model_quality_panel, wxID_ANY, _L("AI 视觉复核：未运行"));
    wxFont visual_font = m_visual_quality_status->GetFont();
    visual_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_visual_quality_status->SetFont(visual_font);
    m_visual_review_model = new wxButton(m_model_quality_panel, wxID_ANY, _L("AI 视觉复核"));
    m_visual_review_model->SetToolTip(_L("生成最终模型五视图，对照原图检查人脸、主体和材料串色；检查仅作提示，不阻止导入"));
    visual_header->Add(m_visual_quality_status, 1, wxALIGN_CENTER_VERTICAL);
    visual_header->Add(m_visual_review_model, 0, wxLEFT, FromDIP(12));
    quality_sizer->Add(visual_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    m_visual_quality_summary = new wxStaticText(m_model_quality_panel, wxID_ANY,
        _L("模型准备好后可按需生成五视图并进行 AI 外观复核。"));
    m_visual_quality_summary->Wrap(FromDIP(500));
    quality_sizer->Add(m_visual_quality_summary, 0, wxEXPAND | wxALL, FromDIP(10));

    m_model_refinement_panel = new wxPanel(m_model_quality_panel);
    auto* refinement_sizer = new wxBoxSizer(wxVERTICAL);
    auto* refinement_header = new wxBoxSizer(wxHORIZONTAL);
    m_model_refinement_status = new wxStaticText(
        m_model_refinement_panel, wxID_ANY, _L("下一次生成优化"));
    wxFont refinement_font = m_model_refinement_status->GetFont();
    refinement_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_model_refinement_status->SetFont(refinement_font);
    m_apply_model_refinement = new wxButton(
        m_model_refinement_panel, wxID_ANY, _L("应用到下一次生成"));
    m_apply_model_refinement->SetToolTip(
        _L("把本地质量建议加入文字输入；不会立即调用 Image2、Tripo 或其他付费服务"));
    refinement_header->Add(m_model_refinement_status, 1, wxALIGN_CENTER_VERTICAL);
    refinement_header->Add(m_apply_model_refinement, 0, wxLEFT, FromDIP(12));
    refinement_sizer->Add(refinement_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    m_model_refinement_summary = new wxStaticText(m_model_refinement_panel, wxID_ANY, wxEmptyString);
    m_model_refinement_summary->SetForegroundColour(wxColour(91, 104, 107));
    m_model_refinement_summary->Wrap(FromDIP(500));
    refinement_sizer->Add(m_model_refinement_summary, 0, wxEXPAND | wxALL, FromDIP(10));
    m_model_refinement_panel->SetSizer(refinement_sizer);
    m_model_refinement_panel->Hide();
    quality_sizer->Add(m_model_refinement_panel, 0, wxEXPAND);
    m_model_quality_panel->SetSizer(quality_sizer);
    m_model_quality_panel->Hide();
    m_model_preview_message = new wxStaticText(
        model_page, wxID_ANY, _L("拖动模型旋转，滚轮缩放；点击“完整显示模型”恢复全貌。上方缩放按钮用于图片。"));
    m_model_preview_message->SetForegroundColour(wxColour(91, 104, 107));
    model_sizer->Add(m_model_preview_message, 0, wxEXPAND | wxALL, FromDIP(12));
    model_page->SetSizer(model_sizer);
    model_page->FitInside();
    m_preview_book->AddPage(model_page, _L("结果对照"), true);
    m_preview_book->AddPage(build_model_library(m_preview_book), _L("历史资产"), false);
    sizer->Add(m_preview_book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));

    m_preview_message = new wxStaticText(panel, wxID_ANY, _L("请先输入描述或选择参考图。"));
    m_preview_message->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(m_preview_message, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));

    m_result_summary = new wxStaticText(panel, wxID_ANY, _L("尚未生成模型。"));
    m_result_summary->Wrap(FromDIP(520));
    sizer->Add(m_result_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));
    panel->SetSizer(sizer);

    m_zoom_out->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_preview_zoom(m_preview_zoom_factor / 1.25); });
    m_zoom_fit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_preview_zoom(1.0); });
    m_zoom_in->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_preview_zoom(m_preview_zoom_factor * 1.25); });
    m_preview_stage->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { apply_preview_stage(true); });
    m_reset_model_view->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_model_preview != nullptr)
            m_model_preview->reset_view();
    });
    m_front_model_view->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_model_preview != nullptr)
            m_model_preview->front_view();
    });
    m_recheck_model->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_recheck_model, this);
    m_locate_thin_regions->Bind(wxEVT_BUTTON, [this, model_page](wxCommandEvent&) {
        if (m_model_preview == nullptr)
            return;
        const bool has_ranked_region = !m_model_quality.thin_local_regions.empty() &&
            !m_model_quality.thin_local_regions.front().face_indices.empty();
        size_t region_index = 0;
        const std::vector<size_t>* evidence = &m_model_quality.thin_local_face_indices;
        if (has_ranked_region) {
            region_index = m_thin_region_navigation_active
                ? (m_thin_region_navigation_index + 1) % m_model_quality.thin_local_regions.size()
                : 0;
            evidence = &m_model_quality.thin_local_regions[region_index].face_indices;
        }
        const size_t localized = m_model_preview->select_face_evidence(*evidence);
        if (localized == 0) {
            if (m_model_preview->region_selection_preparing()) {
                m_local_recolor_toggle->SetValue(true); refresh_local_recolor_controls();
                m_status->SetLabel(_L("正在准备局部选择，完成后自动定位。")); return;
            }
            m_status->SetLabel(_L("当前质量报告没有可定位的局部薄壁证据。"));
            return;
        }
        m_thin_region_navigation_active = has_ranked_region;
        m_thin_region_navigation_index = region_index;
        m_local_recolor_toggle->SetValue(true);
        refresh_local_recolor_controls();
        model_page->Layout();
        model_page->FitInside();
        if (has_ranked_region) {
            const size_t region_count = std::max(
                m_model_quality.thin_local_region_count, m_model_quality.thin_local_regions.size());
            wxString preview_message = wxString::Format(
                _L("已高亮薄壁风险区 %llu/%llu 的 %llu 个证据面（共识别 %llu 个）"),
                static_cast<unsigned long long>(region_index + 1),
                static_cast<unsigned long long>(m_model_quality.thin_local_regions.size()),
                static_cast<unsigned long long>(localized),
                static_cast<unsigned long long>(region_count));
            const auto& region = m_model_quality.thin_local_regions[region_index];
            const wxString metrics = thin_local_region_metrics(
                region,
                m_model_quality.local_wall_thickness_threshold_available,
                m_model_quality.minimum_local_wall_thickness_mm);
            if (!metrics.empty())
                preview_message += _L(" · ") + metrics;
            preview_message += _L("；可旋转复核或手动增减。");
            m_model_preview_message->SetLabel(preview_message);
            m_status->SetLabel(thin_local_region_status(
                region_index,
                m_model_quality.thin_local_regions.size(),
                region,
                m_model_quality.local_wall_thickness_threshold_available,
                m_model_quality.minimum_local_wall_thickness_mm));
        } else {
            m_model_preview_message->SetLabel(wxString::Format(
                _L("已高亮 %llu 个局部薄壁采样面；可旋转复核，或在局部区域工具中手动增减。"),
                static_cast<unsigned long long>(localized)));
            m_status->SetLabel(_L("已定位局部薄壁证据；这里只做风险复核，不会自动修改模型。"));
        }
    });
    m_locate_overhang_regions->Bind(wxEVT_BUTTON, [this, model_page](wxCommandEvent&) {
        if (m_model_preview == nullptr)
            return;
        const size_t localized = m_model_preview->select_elevated_overhang_regions();
        if (localized == 0) {
            if (m_model_preview->region_selection_preparing()) {
                m_local_recolor_toggle->SetValue(true); refresh_local_recolor_controls();
                m_status->SetLabel(_L("正在准备局部选择，完成后自动定位。")); return;
            }
            m_status->SetLabel(_L("当前模型没有达到显著阈值的离床悬垂区域。"));
            return;
        }
        m_local_recolor_toggle->SetValue(true);
        refresh_local_recolor_controls();
        model_page->Layout();
        model_page->FitInside();
        m_model_preview_message->SetLabel(wxString::Format(
            _L("已高亮 %llu 个悬垂三角面；可旋转检查，或在局部区域工具中手动增减。"),
            static_cast<unsigned long long>(localized)));
        m_status->SetLabel(_L("已定位显著局部悬垂；这里只做风险复核，不会自动生成支撑。"));
    });
    m_visual_review_model->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_visual_review_model, this);
    m_apply_model_refinement->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_apply_model_refinement, this);
    m_local_recolor_toggle->Bind(wxEVT_TOGGLEBUTTON, [this, model_page](wxCommandEvent&) {
        refresh_local_recolor_controls();
        model_page->Layout();
        model_page->FitInside();
    });
    for (size_t index = 0; index < m_region_color_buttons.size(); ++index) {
        m_region_color_buttons[index]->Bind(wxEVT_TOGGLEBUTTON, [this, index](wxCommandEvent&) {
            m_region_color_index = static_cast<int>(index);
            refresh_local_recolor_controls();
        });
    }
    m_apply_region_color->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_apply_local_recolor, this);
    m_model_preview->set_selection_changed_callback([this](size_t selected_faces) {
        if (m_finishing_workbench && (m_finishing_tool->GetSelection() == 1 || m_finishing_tool->GetSelection() == 4 || m_finishing_tool->GetSelection() == 5)) {
            if (m_busy || !m_finishing_candidate.empty()) selected_faces = m_finishing_options.selected_faces.size();
            m_finishing_selection_status->SetLabel(wxString::Format(_L("已选 %llu 个面 · 保护 %llu 个面"),
                static_cast<unsigned long long>(selected_faces), static_cast<unsigned long long>(m_model_preview->protected_face_count())));
            refresh_local_recolor_controls();
            return;
        }
        bool matched_region = false;
        size_t matched_region_index = 0;
        if (m_model_preview != nullptr) {
            for (size_t index = 0; index < m_model_quality.thin_local_regions.size(); ++index) {
                if (!m_model_preview->selection_matches_face_evidence(
                        m_model_quality.thin_local_regions[index].face_indices))
                    continue;
                m_thin_region_navigation_active = true;
                m_thin_region_navigation_index = index;
                matched_region = true;
                matched_region_index = index;
                break;
            }
        }
        if (!matched_region) {
            m_thin_region_navigation_active = false;
            m_thin_region_navigation_index = 0;
        }
        if (m_model_preview_message != nullptr) {
            m_model_preview_message->SetLabel(selected_faces == 0
                ? _L("生成完成后可拖动旋转模型，并使用滚轮缩放。")
                : wxString::Format(_L("当前选区包含 %llu 个三角面；可继续检查或手动增减。"),
                                   static_cast<unsigned long long>(selected_faces)));
        }
        if (m_status != nullptr) {
            if (matched_region) {
                m_status->SetLabel(thin_local_region_status(
                    matched_region_index,
                    m_model_quality.thin_local_regions.size(),
                    m_model_quality.thin_local_regions[matched_region_index],
                    m_model_quality.local_wall_thickness_threshold_available,
                    m_model_quality.minimum_local_wall_thickness_mm));
            } else {
                m_status->SetLabel(selected_faces == 0
                    ? _L("当前未选择局部区域。")
                    : _L("已选择局部区域；可继续复核或手动增减。"));
            }
        }
        refresh_local_recolor_controls();
    });
    update_region_mode();
    m_preview_book->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this, panel, expand_images](wxBookCtrlEvent& event) {
        const int selection = event.GetSelection();
        const bool result_page = selection == 0;
        m_zoom_out->Show(result_page && !m_finishing_workbench);
        m_zoom_fit->Show(result_page && !m_finishing_workbench);
        m_zoom_in->Show(result_page && !m_finishing_workbench);
        m_preview_zoom->Show(result_page && !m_finishing_workbench);
        expand_images->Show(result_page && !m_finishing_workbench);
        if (result_page) show_model_comparison();
        m_preview_details_pane->Show(result_page && m_model_views_available);
        m_preview_kind->SetLabel(result_page ? (m_finishing_workbench ? _L("美颜工作台") : _L("结果对照")) : _L("历史资产"));
        if (!result_page) load_library_entries();
        panel->Layout();
        if (selection == 0 && m_model_preview != nullptr)
            m_model_preview->refresh();
        event.Skip();
    });
    return panel;
}

void ModelGenerationPanel::show_model_comparison()
{
    m_expand_images->SetValue(false);
    m_expand_images->SetLabel(_L("展开图片"));
    wxWindow* card = m_model_preview->GetParent();
    card->Show();
    card->GetParent()->Layout();
}

wxWindow* ModelGenerationPanel::build_model_library(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    panel->SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(section_label(panel, _L("历史资产")), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
    auto* session = new wxStaticText(
        panel, wxID_ANY, _L("重开设计图继续创作，或加载模型预览和导入；每次重新设计分别保留。"));
    session->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(session, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
    m_library_empty = new wxStaticText(panel, wxID_ANY, _L("还没有保存的设计图或模型。"));
    m_library_empty->SetForegroundColour(wxColour(110, 122, 125));
    sizer->Add(m_library_empty, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
    m_library_scroller = new wxScrolledWindow(panel, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(-1, 180)), wxVSCROLL);
    m_library_scroller->SetScrollRate(0, FromDIP(8));
    m_library_sizer = new wxBoxSizer(wxVERTICAL);
    m_library_scroller->SetSizer(m_library_sizer);
    sizer->Add(m_library_scroller, 1, wxEXPAND | wxALL, FromDIP(12));
    panel->SetSizer(sizer);
    refresh_library();
    return panel;
}

void ModelGenerationPanel::on_choose_image(wxCommandEvent&)
{
    wxString initial_directory;
    const boost::filesystem::path generated_root = generated_models_root();
    const auto is_user_image_directory = [&generated_root](const boost::filesystem::path& directory) {
        if (!boost::filesystem::is_directory(directory))
            return false;
        boost::system::error_code root_ec;
        boost::system::error_code directory_ec;
        const boost::filesystem::path canonical_root = boost::filesystem::canonical(generated_root, root_ec);
        const boost::filesystem::path canonical_directory = boost::filesystem::canonical(directory, directory_ec);
        return root_ec || directory_ec ||
               (canonical_directory != canonical_root && !path_is_inside(canonical_root, canonical_directory));
    };
    const boost::filesystem::path selected_directory = m_selected_image_path.parent_path();
    if (is_user_image_directory(selected_directory))
        initial_directory = wxString::FromUTF8(selected_directory.string());
    if (wxGetApp().app_config != nullptr) {
        const std::string saved = wxGetApp().app_config->get("model_generation_image_directory");
        if (initial_directory.empty() && !saved.empty() &&
            is_user_image_directory(boost::filesystem::path(saved)))
            initial_directory = wxString::FromUTF8(saved);
    }
    if (initial_directory.empty())
        initial_directory = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Pictures);
    wxFileDialog dialog(this, _L("选择参考图"), initial_directory, wxEmptyString,
                        _L("PNG 和 JPEG 图片 (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;
    boost::filesystem::path path(dialog.GetPath().ToStdWstring());
    if (!is_supported_image(path)) {
        MessageDialog error(this, _L("请选择可完整打开、边长至少 64 px 且不超过 20 MB 的 PNG 或 JPEG 图片。"),
                            wxEmptyString, wxOK | wxICON_ERROR);
        error.ShowModal();
        return;
    }
    if (!m_job_id.empty() && !m_awaiting_palette_confirmation)
        reset(true);
    m_selected_image_path = std::move(path);
    m_style_user_selected = false;
    m_style_recommendation_available = false;
    if (wxGetApp().app_config != nullptr)
        wxGetApp().app_config->set(
            "model_generation_image_directory", m_selected_image_path.parent_path().string());
    m_style_preview_ready = false;
    m_raw_preview_available = false;
    m_model_reference_available = false;
    m_strict_preview_available = false;
    m_model_views_available = false;
    m_heatmap_available = false;
    const size_t bytes = boost::filesystem::file_size(m_selected_image_path);
    m_selected_image->SetLabel(wxString::FromUTF8(m_selected_image_path.filename().string()) +
                               wxString::Format(" (%llu KB)", static_cast<unsigned long long>((bytes + 1023) / 1024)));
    show_selected_image_preview();
    request_style_recommendation();
    refresh_controls();
}

void ModelGenerationPanel::on_clear_image(wxCommandEvent&)
{
    if (!m_job_id.empty() && !m_awaiting_palette_confirmation)
        reset(true);
    m_selected_image_path.clear();
    ++m_style_recommendation_sequence;
    m_style_recommendation_loading = false;
    m_style_recommendation_available = false;
    m_style_recommendation = {};
    m_selected_image->SetLabel(_L("未选择图片"));
    set_preview_empty(_L("请输入描述、选择参考图，或同时提供两者。"));
    refresh_controls();
}

void ModelGenerationPanel::on_palette_source_changed(wxCommandEvent&)
{
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::on_printable_colors_toggled(wxCommandEvent&)
{
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::on_add_custom_color(wxCommandEvent&)
{
    if (m_palette_source->GetSelection() == 0)
        return;
    const size_t palette_limit = m_palette_source->GetSelection() == 2
        ? current_palette_color_count() : Slic3r::AI::kMaxTargetPaletteColors;
    if (m_custom_palette.size() >= palette_limit) {
        show_input_hint(wxString::Format(_L("当前配色最多使用 %llu 种目标色。"), static_cast<unsigned long long>(palette_limit)));
        return;
    }
    std::string color = m_custom_color->GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    std::transform(color.begin(), color.end(), color.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    if (std::find(m_custom_palette.begin(), m_custom_palette.end(), color) == m_custom_palette.end()) {
        m_custom_palette.emplace_back(std::move(color));
        if (m_palette_source->GetSelection() == 2)
            m_user_adjusted_palette_colors.emplace_back(m_custom_palette.back());
    }
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::on_recommend_palette(wxCommandEvent&)
{
    if (m_busy || m_shutdown)
        return;
    const std::string prompt = m_prompt->GetValue().ToUTF8().data();
    const bool image_mode = has_image_input();
    const size_t palette_color_count = current_palette_color_count();
    if (prompt.empty() && !image_mode) {
        show_input_hint(_L("请先输入描述或选择参考图。"), m_prompt);
        return;
    }
    if (current_style() == "custom" && current_custom_style().empty()) {
        show_input_hint(_L("请描述希望使用的自定义风格。"), m_custom_style);
        return;
    }
    reset(true);
    m_palette_source->SetSelection(2);
    m_job_palette.clear();
    m_job_palette_roles.clear();
    m_job_palette_color_count = palette_color_count;
    m_job_use_printable_colors = true;
    m_job_prompt = m_prompt->GetValue();
    m_job_style = current_style();
    m_job_custom_style = current_custom_style();
    m_job_generation_profile = current_generation_profile();
    m_job_face_limit = current_face_limit();
    m_job_print_settings = current_print_settings();
    m_job_image_path = m_selected_image_path;
    m_palette_recommendation_confirmed = false;
    m_awaiting_palette_confirmation = false;
    m_job_preview_expected = true;
    m_busy = true;
    const uint64_t sequence = ++m_sequence;
    update_progress(3, 1, _L("推荐打印配色"));
    m_status->SetLabel(_L("AI 正在分析主体、风格和打印色区..."));
    m_result_summary->SetLabel(_L("推荐完成后直接生成 AI 设计图，配色会一起显示；不会自动生成 3D。"));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    auto success = [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
        if (!weak) return;
        wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
            if (weak) weak->handle_status(std::move(status), sequence);
        });
    };
    auto failure = [weak, sequence](std::string error) mutable {
        if (!weak) return;
        wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
            if (weak) weak->handle_error(error, sequence);
        });
    };
    if (image_mode) {
        m_client.recommend_image_palette(new_request_id(), prompt, m_selected_image_path, m_job_style,
                                         m_job_custom_style, m_job_palette_color_count, m_job_print_settings,
                                         std::move(success), std::move(failure), true);
    } else {
        m_client.recommend_text_palette(new_request_id(), prompt, m_job_style, m_job_custom_style,
                                        m_job_palette_color_count, m_job_print_settings,
                                        std::move(success), std::move(failure), true);
    }
}

void ModelGenerationPanel::on_confirm_recommended_palette(wxCommandEvent& event)
{
    if (!m_awaiting_palette_confirmation || m_job_id.empty() || m_custom_palette.empty())
        return;
    if (!job_base_inputs_match()) {
        MessageDialog confirm(
            this,
            _L("输入内容已经变化。要保留当前推荐配色，并用新的输入生成图片预览吗？\n\n此操作可能消耗 API 额度。"),
            _L("继续使用当前配色"), wxYES_NO | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES)
            return;
        on_preprocess(event);
        return;
    }
    // Requesting the recommendation already required one quota confirmation,
    // and this handler runs only after the explicit "use palette and generate"
    // click. A second modal repeated the same decision on the happy path.
    m_job_palette = current_palette();
    m_job_palette_roles = current_palette_roles();
    m_palette_recommendation_confirmed = true;
    m_awaiting_palette_confirmation = false;
    m_job_preview_expected = true;
    m_busy = true;
    m_client.record_journey_event("preview_requested", m_job_id);
    const uint64_t sequence = m_sequence;
    update_progress(10, 2, _L("生成AI 设计图"));
    m_status->SetLabel(_L("正在根据当前配色生成 AI 设计图..."));
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.confirm_palette(
        m_job_id, m_job_palette, m_job_palette_roles,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::on_preprocess(wxCommandEvent& event)
{
    if (m_busy || m_shutdown)
        return;
    if (current_style().empty()) {
        show_input_hint(_L("请先选择生成风格。"), m_style);
        return;
    }
    const std::string entered_prompt = m_prompt->GetValue().ToUTF8().data();
    const bool image_mode = has_image_input();
    if (entered_prompt.empty() && !image_mode) {
        show_input_hint(_L("请先输入描述或选择参考图。"), m_prompt);
        return;
    }
    const std::string custom_style = current_custom_style();
    if (current_style() == "custom" && custom_style.empty()) {
        show_input_hint(_L("请描述希望使用的自定义风格。"), m_custom_style);
        return;
    }
    const bool ai_palette_source = use_printable_colors() && m_palette_source->GetSelection() == 2;
    if (ai_palette_source && !m_job_id.empty() &&
        current_palette_color_count() != m_job_palette_color_count) {
        on_recommend_palette(event);
        return;
    }
    if (ai_palette_source && m_awaiting_palette_confirmation && job_base_inputs_match()) {
        on_confirm_recommended_palette(event);
        return;
    }
    if (ai_palette_source && !m_palette_recommendation_confirmed && !m_awaiting_palette_confirmation) {
        on_recommend_palette(event);
        return;
    }
    const std::string prompt = entered_prompt;
    const std::vector<std::string> palette = current_palette();
    const AIModelGenerationClient::PaletteRoles palette_roles = current_palette_roles();
    if (use_printable_colors() && palette.empty()) {
        show_input_hint(_L("生成可打印模型前，请至少配置一种有效耗材颜色。"));
        return;
    }
    if (use_printable_colors() && m_minimum_feature->GetValue() < m_line_width->GetValue()) {
        show_input_hint(_L("最小特征不能小于挤出线宽。建议设置为两条线宽，例如 0.8 mm。"));
        return;
    }
    const bool regenerating_preview = m_style_preview_ready || m_awaiting_confirmation;
    if (image_mode) {
        static const std::regex absolute_path(R"(^\s*(?:[A-Za-z]:[\\/]|/).*)");
        if (!entered_prompt.empty() && std::regex_match(entered_prompt, absolute_path)) {
            show_input_hint(_L("请描述希望 AI 如何处理图片，不要在描述中粘贴本地文件路径。"), m_prompt);
            return;
        }
        wxString message;
        if (regenerating_preview) {
            message << _L("使用当前风格重新生成 AI 设计图吗？\n\n")
                    << _L("会调用 1 次图片服务生成适合 3D 建模的设计图；不会创建 3D 任务。");
        } else {
            message << _L("要使用这张图片生成 AI 设计图吗？\n\n")
                    << wxString::FromUTF8(m_selected_image_path.filename().string()) << "\n"
                    << _L("仅发送这张图片和文字描述，调用 1 次图片服务；此操作消耗 API 额度。");
        }
        if ((current_style() == "realistic" || current_style() == "portrait_sketch") && use_printable_colors())
            message << _L("\n若识别到真人，优先保留脸型、五官和姿态。");
        MessageDialog confirm(this, message,
                              regenerating_preview ? _L("重新生成图片预览") : _L("生成风格预览"),
                              wxYES_NO | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES)
            return;
    } else {
        MessageDialog confirm(this,
            use_printable_colors()
                ? _L("要根据文字生成 AI 设计图吗？\n\n会生成适合 3D 建模的高质量设计图，并保留所选配色供后续模型使用。此操作消耗 API 额度。")
                : _L("要根据文字生成 AI 设计图吗？\n\n会先生成并检查图片，再用于后续 3D 生成；此操作可能消耗 API 额度。"),
            _L("生成图片预览"), wxYES_NO | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES)
            return;
    }

    const std::string previous_job_id = m_job_id;
    const bool palette_was_ai_recommended = ai_palette_source && m_palette_recommendation_confirmed;
    if (regenerating_preview)
        m_client.record_journey_event("preview_regenerated", previous_job_id);
    reset(true);
    m_client.record_journey_event("preview_requested");
    m_job_palette = palette;
    m_job_palette_color_count = current_palette_color_count();
    // reset() refreshes the controls and may rebuild inferred role defaults.
    // Preserve the explicit semantic mapping that was visible at confirmation;
    // swapping portrait skin and garment roles here causes exactly the kind of
    // skin-on-sleeve material bleed the preview gate is meant to prevent.
    m_palette_roles = palette_roles;
    m_palette_roles_source = palette;
    m_job_palette_roles = palette_roles;
    m_job_use_printable_colors = use_printable_colors();
    m_job_prompt = m_prompt->GetValue();
    m_job_style = current_style();
    m_job_custom_style = custom_style;
    m_job_generation_profile = current_generation_profile();
    m_job_face_limit = current_face_limit();
    m_job_print_settings = current_print_settings();
    m_job_image_path = m_selected_image_path;
    m_job_preview_expected = true;
    m_palette_recommendation_confirmed = palette_was_ai_recommended;
    m_busy = true;
    const bool preview_mode = true;
    if (preview_mode) {
        m_style_preview_placeholder = _L("正在生成...");
        if (m_reference_image.IsOk()) {
            m_preview_message->SetLabel(
                wxString::Format(_L("原图 %d × %d px  ·  正在生成 AI 图"),
                                 m_reference_image.GetWidth(), m_reference_image.GetHeight()));
        }
        update_preview_view();
    }
    const uint64_t sequence = ++m_sequence;
    const wxString prepare_phase = preview_mode ? _L("生成AI 设计图") : _L("准备提示词");
    update_progress(10, 2, prepare_phase);
    m_workflow_phase->SetLabel(prepare_phase);
    m_status->SetLabel(preview_mode ? _L("正在生成高质量 AI 设计图...") : _L("正在准备 3D 提示词..."));
    m_result_summary->SetLabel(preview_mode ? _L("完成后可对照原图，确认形体与细节，再生成 3D。")
                                            : _L("正在整理用于 3D 生成的提示词。"));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    auto success = [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
        if (!weak)
            return;
        wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
            if (weak)
                weak->handle_status(std::move(status), sequence);
        });
    };
    auto failure = [weak, sequence](std::string error) mutable {
        if (!weak)
            return;
        wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
            if (weak)
                weak->handle_error(error, sequence);
        });
    };
    if (image_mode)
        m_client.preprocess_image(new_request_id(), prompt, m_selected_image_path, m_job_palette, m_job_palette_roles,
                                  m_palette_recommendation_confirmed, m_job_style, m_job_custom_style,
                                  m_job_print_settings,
                                  std::move(success), std::move(failure));
    else
        m_client.preprocess_text(new_request_id(), prompt, m_job_palette, m_job_palette_roles,
                                 m_palette_recommendation_confirmed, m_job_style, m_job_custom_style,
                                 m_job_print_settings,
                                 std::move(success), std::move(failure));
}

void ModelGenerationPanel::on_generate(wxCommandEvent&)
{
    const bool image_mode = m_job_preview_expected;
    if (!m_awaiting_confirmation || m_job_id.empty() || !job_inputs_match() || (image_mode && !m_style_preview_ready))
        return;
    if (use_printable_colors() != m_job_use_printable_colors || current_palette() != m_job_palette) {
        show_input_hint(_L("颜色模式或耗材色板发生了变化，请先重新生成预览。"));
        return;
    }
    m_job_generation_profile = current_generation_profile();
    m_job_face_limit = current_face_limit();
    wxString message = image_mode
        ? _L("要根据当前 AI 设计图创建 1 个付费 Tripo 3D 生成任务吗？")
        : _L("要根据已确认的提示词创建 1 个付费 3D 生成任务吗？");
    message += _L("\n\n生成策略：") + current_generation_profile_label();
    if (m_job_generation_profile == "quality") {
        message += _L("\n质量：100 万面目标、标准几何、标准纹理和 PBR。");
        if (image_mode && (m_job_style == "realistic" || m_job_style == "portrait_sketch"))
            message += _L("\n保留 AI 设计图中的脸型、五官、姿态和完整构图，不自动裁切或替换底座；本次只创建 1 个 Tripo 模型任务。");
    } else {
        message += _L("\n性能：30 万面目标、标准几何与纹理，保留 PBR。");
    }
    message += _L("\n预计：通常 3–10 分钟；高质量写实人像通常 15–35 分钟。"
                  "\n费用：由模型服务商账户按套餐或额度结算。"
                  "\n停止：只停止本地等待；已提交的远端任务可能继续运行并计费。");
    MessageDialog confirm(this, message, _L("确认生成 3D 模型"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;
    if (image_mode)
        m_client.record_journey_event("preview_accepted", m_job_id);
    m_client.record_journey_event("model_submitted", m_job_id);
    m_journey_model_submitted = true;
    m_busy = true;
    m_awaiting_confirmation = false;
    m_artifact_download_started = false;
    m_model_preview_ready = false;
    m_library_model_loaded = false;
    m_displayed_model_path.clear();
    m_displayed_model_job_id.clear();
    m_displayed_model_palette.clear();
    m_displayed_model_palette_roles.clear();
    clear_model_quality();
    if (m_model_preview != nullptr)
        m_model_preview->clear();
    const uint64_t sequence = m_sequence;
    update_progress(40, 3, _L("生成模型"));
    m_workflow_phase->SetLabel(_L("生成模型"));
    m_status->SetLabel(_L("正在提交 3D 生成请求..."));
    refresh_controls();
    const std::string prepared = image_mode ? std::string() : m_prepared_prompt->GetValue().ToUTF8().data();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.generate(m_job_id, prepared, m_job_palette, m_job_generation_profile,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (!weak)
                    return;
                // A transient failure here is ambiguous: the sidecar may have
                // accepted and persisted the paid task before its HTTP response
                // was interrupted.  Keep polling the same job id so users do not
                // submit a duplicate paid task just to recover the UI.
                if (is_transient_sidecar_poll_error(error) && !weak->m_job_id.empty()) {
                    weak->m_status->SetLabel(_L("提交响应中断，正在查询已保存的任务..."));
                    weak->m_result_summary->SetLabel(
                        _L("不会重复提交；将使用同一任务编号恢复生成进度。"));
                    weak->m_poll_connection_failures = 0;
                    weak->m_poll_timer.StartOnce(500);
                    weak->refresh_controls();
                    return;
                }
                weak->handle_error(error, sequence);
                // Validation can reject the exact geometry reference before a
                // paid task is created. Reload the persisted job so the panel
                // keeps the approved source/palette, exposes "Change image",
                // and disables 3D submission until a clean reroll exists.
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence || weak->m_job_id.empty())
                    return;
                const std::string job_id = weak->m_job_id;
                weak->m_client.get_status(job_id,
                    [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
                        if (!weak)
                            return;
                        wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                            if (weak)
                                weak->handle_status(std::move(status), sequence);
                        });
                    },
                    [](std::string) {});
            });
        });
}

void ModelGenerationPanel::on_retexture_from_library(const std::string& geometry_job_id,
                                                      const wxString& title)
{
    if (m_busy || !m_service_available || m_job_id.empty() || geometry_job_id.empty() ||
        geometry_job_id == m_job_id || !m_job_preview_expected || (!m_ready && !m_awaiting_confirmation))
        return;
    wxString message = _L("要保留历史模型“") + title + _L("”的脸部和整体造型，只使用当前确认图片重新生成颜色吗？");
    message += _L("\n\n将创建 1 个付费 Tripo 纹理任务，不会重建网格；因此能保留已经满意的脸和姿态，"
                  "但也不会修复历史模型原有的形状问题。"
                  "\n费用：由当前模型服务商账户按其套餐或额度结算；OrcaSlicer 无法读取具体金额。"
                  "\n预计耗时：通常 3–10 分钟。"
                  "\n停止说明：停止按钮只终止本地等待；已经提交的远端任务可能继续运行并计费。");
    MessageDialog confirm(this, message, _L("确认复用造型并重新上色"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;

    const std::string reference_job_id = m_job_id;
    m_client.record_journey_event("model_submitted", reference_job_id);
    m_journey_model_submitted = true;
    m_busy = true;
    m_awaiting_confirmation = false;
    m_ready = false;
    m_artifact_download_started = false;
    m_model_preview_ready = false;
    m_library_model_loaded = false;
    m_artifact_path.clear();
    m_color_intent_path.clear();
    m_color_intent_schema.clear();
    m_color_intent_sha256.clear();
    m_displayed_model_path.clear();
    m_displayed_model_job_id.clear();
    m_displayed_model_palette.clear();
    m_displayed_model_palette_roles.clear();
    clear_model_quality();
    if (m_model_preview != nullptr)
        m_model_preview->clear();
    const uint64_t sequence = m_sequence;
    update_progress(40, 3, _L("保留造型并上色"));
    m_workflow_phase->SetLabel(_L("保留造型并上色"));
    m_status->SetLabel(_L("正在提交保留造型的纹理任务..."));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.retexture(reference_job_id, geometry_job_id,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::on_stop(wxCommandEvent&)
{
    if (m_design_history_loading) {
        ++m_design_history_sequence;
        m_design_history_loading = false;
        m_busy = false;
        refresh_controls();
        m_status->SetLabel(_L("已取消历史设计加载，当前内容已保留。"));
        return;
    }
    if (m_job_id.empty())
        return;
    m_poll_timer.Stop();
    m_client.cancel_current();
    if (m_ready && m_artifact_download_started && !m_model_preview_ready) {
        ++m_sequence;
        m_busy = false;
        m_restoring_input = false;
        m_artifact_download_started = false;
        m_status->SetLabel(_L("已取消本地模型加载。"));
        m_result_summary->SetLabel(_L("生成结果仍然保留，可点击“重新加载 3D 模型”继续。"));
        m_model_stats->SetLabel(_L("模型尚未加载"));
        refresh_controls();
        return;
    }
    m_status->SetLabel(_L("正在停止本地任务；已提交的远端任务可能仍会继续运行并计费。"));
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.stop(m_job_id,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_poll_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::on_retry_service(wxCommandEvent&)
{
    if (m_service_available || m_busy || !m_service_retry_handler)
        return;
    m_retry_service->Disable();
    m_status->SetLabel(_L("正在重新检测本地 3D 生成服务..."));
    m_result_summary->SetLabel(_L("检测完成后会自动恢复可用功能和最近任务。"));
    m_service_retry_handler();
}

void ModelGenerationPanel::on_import(wxCommandEvent&)
{
    if (m_finishing_running || !m_finishing_candidate.empty()) return;
    if (m_ready && !m_model_preview_ready && !m_artifact_download_started) {
        m_artifact_download_started = true;
        download_model_preview(m_sequence);
        return;
    }
    download_and_import();
}
void ModelGenerationPanel::on_poll(wxTimerEvent&) { schedule_poll(); }

void ModelGenerationPanel::handle_error(const std::string& error, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    if (m_journey_model_submitted) {
        m_client.record_journey_event("model_failed", m_job_id);
        m_journey_model_submitted = false;
    } else if (m_job_preview_expected) {
        m_client.record_journey_event("preview_failed", m_job_id);
    }
    m_poll_timer.Stop();
    m_busy = false;
    const bool paid_preflight_rejected =
        error.find("large square cutout") != std::string::npos ||
        error.find("missing body region") != std::string::npos ||
        error.find("shoulder silhouette") != std::string::npos ||
        error.find("background remnant") != std::string::npos ||
        error.find("not suitable for 3D input") != std::string::npos ||
        error.find("before paying for 3D generation") != std::string::npos;
    m_awaiting_confirmation = paid_preflight_rejected;
    if (!paid_preflight_rejected) {
        m_awaiting_palette_confirmation = false;
        m_palette_recommendation_confirmed = false;
    } else {
        // The preview itself remains a useful user artifact, but it may not be
        // resubmitted until a regenerated geometry reference passes preflight.
        m_model_input_eligible = false;
        m_model_input_primary_blocker =
            error.find("shoulder silhouette") != std::string::npos ||
            error.find("background remnant") != std::string::npos
                ? "portrait_shoulder_silhouette_unverified"
                : "subject_has_rectangular_cutout";
    }
    m_ready = false;
    m_artifact_download_started = false;
    m_model_preview_ready = false;
    m_library_model_loaded = false;
    m_displayed_model_path.clear();
    m_displayed_model_palette.clear();
    m_displayed_model_palette_roles.clear();
    if (m_model_preview != nullptr)
        m_model_preview->clear();
    m_artifact_format.clear();
    m_artifact_color_encoding.clear();
    m_color_intent_path.clear();
    m_color_intent_schema.clear();
    m_color_intent_sha256.clear();
    wxString message = localized_service_error(error);
    if (!m_job_id.empty())
        message += "\n" + _L("诊断 ID：") + from_u8(m_job_id);
    m_status->SetLabel(message);
    m_result_summary->SetLabel(_L("模型尚未生成完成。"));
    if (job_uses_image() && m_reference_image.IsOk() && !m_style_preview_ready) {
        m_style_preview_placeholder = _L("预览不可用");
        update_preview_view();
    }
    refresh_controls();
}

void ModelGenerationPanel::handle_poll_error(const std::string& error, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    if (!is_transient_sidecar_poll_error(error)) {
        handle_error(error, sequence);
        return;
    }

    ++m_poll_connection_failures;
    const int delay_ms = std::min(10000, 1000 << std::min(m_poll_connection_failures - 1, 3));
    m_status->SetLabel(wxString::Format(
        _L("本地 AI 服务暂时断开，%d 秒后自动重连..."),
        std::max(1, delay_ms / 1000)));
    m_result_summary->SetLabel(
        _L("远端任务编号和当前进度已保存；重连只恢复查询，不会重复提交或重复计费。"));
    m_poll_timer.StartOnce(delay_ms);
    refresh_controls();
}

void ModelGenerationPanel::handle_status(AIModelGenerationClient::JobStatus status, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    m_poll_connection_failures = 0;
    const bool became_model_ready = !m_ready && status.state == "ready" && status.artifact_ready;
    const bool job_changed = status.id != m_job_id;
    const bool palette_count_unchanged = current_palette_color_count() == m_job_palette_color_count;
    if (!job_changed)
        synchronize_palette_roles(current_palette(), m_palette_roles, m_job_palette, m_job_palette_roles,
                                  status.palette, status.palette_roles);
    if (job_changed) {
        m_job_provider_name.clear();
        m_job_provider_task_id.clear();
        m_job_provider_conversion_task_id.clear();
        m_color_intent_path.clear();
    }
    m_job_id = status.id;
    m_job_phase = status.phase;
    m_job_palette_color_count = status.palette_color_count;
    if (m_palette_color_count != nullptr &&
        (job_changed || palette_count_unchanged)) {
        m_palette_color_count->SetSelection(static_cast<int>(
            status.palette_color_count - Slic3r::AI::kMinTargetPaletteColors));
    }
    if (!status.provider_task_id.empty()) {
        m_job_provider_name = status.provider_name;
        m_job_provider_task_id = status.provider_task_id;
        m_job_provider_conversion_task_id = status.provider_conversion_task_id;
    }
    if (status.palette_recommendation.available) {
        const bool new_recommendation = m_palette_recommendation_job_id != status.id;
        m_palette_recommendation = status.palette_recommendation;
        m_palette_recommendation_confirmed = status.palette_recommendation.confirmed;
        if (new_recommendation) {
            m_palette_recommendation_job_id = status.id;
            m_user_adjusted_palette_colors.clear();
            m_custom_palette.clear();
            m_palette_roles.clear();
            if (status.palette_recommendation.confirmed && !status.palette.empty()) {
                m_custom_palette = status.palette;
                m_palette_roles = status.palette_roles.empty() ? automatic_palette_roles(status.palette) : status.palette_roles;
                for (const std::string& color : status.palette) {
                    const auto recommended = std::find_if(
                        status.palette_recommendation.colors.begin(), status.palette_recommendation.colors.end(),
                        [&color](const AIModelGenerationClient::PaletteRecommendationColor& item) { return item.hex == color; });
                    if (recommended == status.palette_recommendation.colors.end())
                        m_user_adjusted_palette_colors.emplace_back(color);
                }
            } else {
                for (const auto& color : status.palette_recommendation.colors) {
                    m_custom_palette.emplace_back(color.hex);
                    m_palette_roles[color.role] = color.hex;
                }
            }
            m_palette_roles_source = m_custom_palette;
            if (m_palette_source != nullptr)
                m_palette_source->SetSelection(2);
            m_job_use_printable_colors = true;
        }
    }
    if (!status.palette.empty()) {
        m_job_palette = status.palette;
        m_job_use_printable_colors = true;
    }
    m_status->SetLabel(localized_job_status(status));
    m_busy = status.state == "recommending_palette" || status.state == "preprocessing" ||
             status.state == "queued" || status.state == "running" || status.state == "stopping";
    m_awaiting_palette_confirmation = status.state == "awaiting_palette_confirmation";
    m_awaiting_confirmation = status.state == "awaiting_confirmation";
    m_ready = status.state == "ready" && status.artifact_ready;
    if (m_journey_model_submitted &&
        (status.state == "failed" ||
         (status.state == "awaiting_confirmation" && status.phase == "multiview_retry"))) {
        m_client.record_journey_event("model_failed", status.id);
        m_journey_model_submitted = false;
    }
    if (became_model_ready) {
        m_client.record_journey_event("model_ready", status.id);
        m_journey_model_submitted = false;
    }
    m_artifact_format = status.artifact_format;
    m_artifact_color_encoding = status.artifact_color_encoding;
    if (!status.color_intent_ready) {
        m_color_intent_path.clear();
        m_color_intent_schema.clear();
        m_color_intent_sha256.clear();
    } else {
        if (status.color_intent_schema != m_color_intent_schema ||
            status.color_intent_sha256 != m_color_intent_sha256)
            m_color_intent_path.clear();
        m_color_intent_schema = status.color_intent_schema;
        m_color_intent_sha256 = status.color_intent_sha256;
    }
    m_raw_preview_available = status.raw_preview_ready;
    m_preview_output = status.model_reference_ready ? "model-reference" : status.preview_ready ? "preview" : "raw-preview";
    const bool new_model_views = status.model_views_ready && !m_model_views_available;
    m_model_views_available = status.model_views_ready;
    m_model_reference_available = status.model_reference_ready;
    m_strict_preview_available = status.strict_preview_ready;
    m_heatmap_available = status.heatmap_ready;
    if (status.preview_ready || status.raw_preview_ready || status.model_reference_ready || status.strict_preview_ready)
        m_job_preview_expected = true;
    else if (status.state == "awaiting_confirmation" && status.source == "text" && status.palette.empty())
        m_job_preview_expected = false;
    m_preview_metrics_available = status.metadata_ready;
    m_preview_changed_pixel_ratio = status.changed_pixel_ratio;
    m_preview_minimum_feature_px = status.minimum_feature_px;
    m_palette_quality_ok = status.palette_quality_ok;
    m_material_fragmentation_ok = status.material_fragmentation_ok;
    m_model_input_eligible = status.model_input_eligible;
    m_model_input_primary_blocker = status.model_input_blockers.empty() ? std::string() : status.model_input_blockers.front();
    m_meaningful_palette_count = status.meaningful_palette_count;
    m_meaningful_subject_color_count = status.meaningful_subject_color_count;
    if (m_ready) {
        m_displayed_model_job_id = status.id;
        apply_model_quality(status.model_quality);
        apply_visual_quality(status.visual_quality);
        apply_model_refinement(status.refinement);
    }
    if (!status.palette_roles.empty())
        m_job_palette_roles = status.palette_roles;
    if (!status.prepared_prompt.empty())
        m_prepared_prompt->SetValue(wxString::FromUTF8(status.prepared_prompt));
    if ((status.preview_ready || status.model_reference_ready || status.raw_preview_ready) && m_preview_path.empty() && !m_restoring_input) {
        m_status->SetLabel(_L("正在加载 AI 风格预览..."));
        m_style_preview_placeholder = _L("正在加载 AI 生成图...");
        update_preview_view();
        download_preview(sequence);
    }
    if (m_ready) {
        wxString summary;
        summary << (m_model_preview_ready ? _L("3D 模型已可预览") : _L("模型已生成，正在准备 3D 预览"))
                << _L(" · ") << wxString::FromUTF8(m_artifact_format);
        if (status.artifact_size > 0)
            summary << wxString::Format(_L(" · %.1f MB"), double(status.artifact_size) / (1024.0 * 1024.0));
        if (status.visual_quality.available && !status.visual_quality.import_recommended)
            summary << _L(" · AI 外观存在提醒，不建议直接导入");
        m_result_summary->SetLabel(summary);
    } else if (m_awaiting_palette_confirmation) {
        m_result_summary->SetLabel(
            _L("AI 推荐配色已准备好。可以替换、删除或补充颜色；确认后再匹配实际耗材。"));
    } else if (m_awaiting_confirmation && status.phase == "multiview_retry") {
        m_result_summary->SetLabel(
            _L("四视图在付费前检查阶段停止，当前预览和配色均已保留；可重试，或先换一张图片。"));
    } else if (m_awaiting_confirmation) {
        if (status.preview_ready && !status.model_input_eligible) {
            m_result_summary->SetLabel(model_input_quality_label(m_model_input_primary_blocker));
        } else if (status.preview_ready && !status.palette.empty() && !status.palette_quality_ok) {
            const int required_colors = std::min<int>(status.palette.size(), 3);
            if (status.meaningful_subject_color_count < required_colors) {
                m_result_summary->SetLabel(wxString::Format(
                    _L("配色不足：主体只有 %d 种有效耗材色，至少需要 %d 种。请重新生成预览。"),
                    status.meaningful_subject_color_count, required_colors));
            } else if (status.printable_subject_area_ratio < 0.18) {
                m_result_summary->SetLabel(_L("主体占画面比例过小，请放大主体后重新生成预览。"));
            } else if (status.largest_subject_component_ratio < 0.90) {
                m_result_summary->SetLabel(_L("主体被背景分成多个不相连区域，请调整构图后重新生成预览。"));
            } else if (status.largest_detached_subject_diagonal_ratio >= 0.08) {
                m_result_summary->SetLabel(_L("检测到细长部件与主体分离，请重新生成并确认把手、枝条或支撑已连接。"));
            } else if (!status.material_fragmentation_ok) {
                m_result_summary->SetLabel(_L("检测到肤色或衣服颜色形成错误杂色块，请重新生成图片预览后再生成 3D。"));
            } else {
                m_result_summary->SetLabel(_L("预览未通过打印性检查，请调整构图或配色后重新生成。"));
            }
        } else if (status.preview_ready &&
                   std::find(status.model_input_warnings.begin(), status.model_input_warnings.end(),
                             "foreground_segmentation_uncertain") != status.model_input_warnings.end()) {
            m_result_summary->SetLabel(
                _L("主体与背景颜色接近，自动检查无法可靠判断轮廓；请确认主体完整、背景干净后再生成 3D。"));
        } else if (status.preview_ready && !status.palette.empty()) {
            m_result_summary->SetLabel(
                (m_job_style == "realistic" || m_job_style == "portrait_sketch") && m_job_generation_profile == "quality"
                    ? _L("AI 设计图已准备好，请确认脸型、五官、姿态和构图。")
                    : _L("AI 设计图与配色已准备好，确认后即可生成 3D。"));
        } else {
            m_result_summary->SetLabel(m_job_preview_expected
                ? _L("AI 风格预览加载完成后即可生成 3D 模型。")
                : _L("请确认提示词后再开始生成 3D 模型。"));
        }
    } else {
        m_result_summary->SetLabel(localized_job_status(status));
    }
    if (m_awaiting_confirmation && status.model_input_eligible &&
        std::find(status.model_input_warnings.begin(), status.model_input_warnings.end(),
                  "reference_visual_review_unavailable") != status.model_input_warnings.end())
        m_result_summary->SetLabel(m_result_summary->GetLabel() +
            _L("\nAI 视觉复核暂不可用，请自行对照原图检查脸型、姿态和配色。"));
    update_workflow(&status);
    const bool palette_recommendation_fallback =
        status.state == "failed" && !m_custom_palette.empty() &&
        status.message.find("palette recommendation") != std::string::npos;
    if (palette_recommendation_fallback) {
        m_palette_source->SetSelection(2);
        m_palette_recommendation_confirmed = true;
        m_awaiting_palette_confirmation = false;
        m_job_palette.clear();
        m_job_palette_roles.clear();
        m_job_use_printable_colors = true;
        refresh_palette();
        update_progress(0, 1, _L("输入"));
        m_workflow_steps->SetLabel(_L("AI 推荐未通过对比度检查，已保留当前颜色，可直接生成图片预览"));
        m_status->SetLabel(_L("AI 推荐颜色过于接近，已自动保留当前有效配色。"));
        m_result_summary->SetLabel(_L("当前 1–6 种颜色仍可编辑，也可直接生成图片预览。"));
    }
    if (m_busy)
        m_poll_timer.StartOnce(1500);
    refresh_controls();
    if (m_ready && !m_artifact_download_started && !m_model_preview_ready) {
        m_artifact_download_started = true;
        download_model_preview(sequence);
    }
    if (new_model_views && !m_preview_path.empty())
        download_auxiliary_previews(sequence, 2);
}

void ModelGenerationPanel::schedule_poll()
{
    if (m_shutdown || m_job_id.empty() || !m_busy)
        return;
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_status(m_job_id,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (!weak)
                    return;
                weak->handle_error(error, sequence);
                // A paid-task preflight can reject the request without changing
                // the persisted preview job (for example when its subject mask
                // needs local repair).  Reload that authoritative state so the
                // user can retry the same approved preview instead of being left
                // on a false "generating" step with only "start over" available.
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence || weak->m_job_id.empty())
                    return;
                const std::string job_id = weak->m_job_id;
                weak->m_client.get_status(job_id,
                    [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
                        if (!weak)
                            return;
                        wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                            if (weak)
                                weak->handle_status(std::move(status), sequence);
                        });
                    },
                    [](std::string) {});
            });
        });
}

void ModelGenerationPanel::download_preview(uint64_t sequence)
{
    m_preview_path = temp_path(m_job_id, "png");
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_image_output(m_job_id, m_preview_output, m_preview_path,
        [weak, sequence](boost::filesystem::path path) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, path = std::move(path)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                wxImage image(path.wstring());
                if (!image.IsOk()) {
                    weak->m_preview_path.clear();
                    weak->m_style_preview_ready = false;
                    weak->m_style_preview_placeholder = _L("预览不可用");
                    weak->m_status->SetLabel(_L("无法显示 AI 风格预览，请重试。"));
                    weak->m_result_summary->SetLabel(_L("获得有效风格预览后才能继续生成 3D 模型。"));
                    weak->m_client.record_journey_event("preview_failed", weak->m_job_id);
                    weak->update_preview_view();
                    weak->refresh_controls();
                    return;
                }
                weak->m_clean_preview_image = image;
                weak->m_preview_zoom_factor = 1.0;
                weak->m_style_preview_ready = true;
                weak->m_client.record_journey_event("preview_ready", weak->m_job_id);
                weak->m_style_preview_placeholder.clear();
                weak->m_preview_kind->SetLabel(_L("结果对照"));
                weak->apply_preview_stage();
                if (weak->m_reference_image.IsOk()) {
                    weak->m_preview_message->SetLabel(
                        wxString::Format(_L("原图 %d × %d px  ·  AI 生成图 %d × %d px"),
                                         weak->m_reference_image.GetWidth(), weak->m_reference_image.GetHeight(),
                                         image.GetWidth(), image.GetHeight()));
                } else {
                    weak->m_preview_message->SetLabel(
                        wxString::Format(_L("AI 生成图 · %d × %d px"), image.GetWidth(), image.GetHeight()));
                }
                weak->m_status->SetLabel(weak->m_job_phase == "stopped" || weak->m_job_phase == "failed"
                    ? _L("AI 设计图已保留，当前任务已结束；可修改输入重新生成。")
                    : _L("AI 设计图已生成，确认后可继续生成 3D 模型。"));
                weak->update_preview_view(true);
                weak->refresh_controls();
                weak->Layout();
                weak->download_auxiliary_previews(sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak && sequence == weak->m_sequence) {
                    weak->m_preview_path.clear();
                    weak->m_style_preview_ready = false;
                    weak->m_style_preview_placeholder = _L("预览不可用");
                    weak->m_status->SetLabel(_L("风格预览下载失败：") + wxString::FromUTF8(error));
                    weak->m_result_summary->SetLabel(_L("获得有效风格预览后才能继续生成 3D 模型。"));
                    weak->m_client.record_journey_event("preview_failed", weak->m_job_id);
                    weak->update_preview_view();
                    weak->refresh_controls();
                }
            });
        });
}

void ModelGenerationPanel::download_auxiliary_previews(uint64_t sequence, int stage)
{
    if (m_shutdown || sequence != m_sequence || m_job_id.empty() || stage >= 3)
        return;
    const bool available[] = {
        m_raw_preview_available, m_model_reference_available, m_model_views_available
    };
    const char* routes[] = {"raw-preview", "model-reference", "model-view-sheet"};
    const char* suffixes[] = {"raw", "model-reference", "model-views"};
    if (!available[stage]) {
        download_auxiliary_previews(sequence, stage + 1);
        return;
    }
    const boost::filesystem::path path = temp_path(m_job_id + "-" + suffixes[stage], "png");
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_image_output(m_job_id, routes[stage], path,
        [weak, sequence, stage](boost::filesystem::path downloaded) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, stage, downloaded = std::move(downloaded)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) return;
                wxImage image(downloaded.wstring());
                if (image.IsOk()) {
                    if (stage == 0) {
                        weak->m_raw_preview_image = image;
                        weak->m_raw_preview_path = downloaded;
                        weak->m_history_display_source = downloaded;
                        weak->m_history_display_image = load_model_image_display_copy(downloaded);
                    }
                    else if (stage == 1) weak->m_model_reference_image = image;
                    else weak->m_model_views_image = image;
                    weak->apply_preview_stage();
                }
                weak->download_auxiliary_previews(sequence, stage + 1);
            });
        },
        [weak, sequence, stage](std::string error) {
            if (!weak) return;
            BOOST_LOG_TRIVIAL(warning) << "Unable to download printable preview stage: " << error;
            wxGetApp().CallAfter([weak, sequence, stage]() {
                if (weak && !weak->m_shutdown && sequence == weak->m_sequence)
                    weak->download_auxiliary_previews(sequence, stage + 1);
            });
        });
}

void ModelGenerationPanel::download_and_import()
{
    if (!m_ready || !m_model_preview_ready || m_busy)
        return;
    if ((m_artifact_format != "obj" && m_artifact_format != "glb")) {
        m_status->SetLabel(_L("只能导入生成的模型。"));
        return;
    }
    if (m_artifact_format == "obj" && m_artifact_color_encoding != "vertex_colors") {
        m_status->SetLabel(_L("生成的模型不包含受支持的顶点颜色。"));
        return;
    }
    boost::filesystem::path local_path;
    if (is_nonempty_model(m_artifact_path))
        local_path = m_artifact_path;
    else if (!m_job_id.empty()) {
        const boost::filesystem::path downloaded_path = temp_path(m_job_id, m_artifact_format);
        if (is_nonempty_model(downloaded_path))
            local_path = downloaded_path;
        else
            m_artifact_path = downloaded_path;
    }

    if (local_path.empty() && m_job_id.empty()) {
        m_status->SetLabel(_L("本地模型已不存在。"));
        m_result_summary->SetLabel(_L("请从模型库重新加载有效模型后再导入准备页。"));
        refresh_controls();
        return;
    }

    m_busy = true;
    update_progress(96, 5, _L("导入准备页"));
    m_workflow_phase->SetLabel(_L("导入准备页"));
    const uint64_t sequence = m_sequence;
    m_status->SetLabel(local_path.empty() ? _L("正在从本地服务读取生成的模型...")
                                         : _L("正在读取本地模型..."));
    refresh_controls();

    if (!local_path.empty()) {
        import_local_artifact(local_path, sequence);
        return;
    }

    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_artifact(m_job_id, m_artifact_format, m_artifact_path,
        [weak, sequence](boost::filesystem::path path) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, path = std::move(path)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                weak->import_local_artifact(path, sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (!weak) return;
                weak->cleanup_files();
                weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::import_local_artifact(const boost::filesystem::path& path, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    if (!is_nonempty_model(path)) {
        m_busy = false;
        m_status->SetLabel(_L("本地模型无效或已不存在。"));
        m_result_summary->SetLabel(_L("请从模型库重新加载有效模型后再导入准备页。"));
        refresh_controls();
        return;
    }

    m_artifact_path = path;
    update_progress(98, 5, _L("导入模型"));
    m_status->SetLabel(_L("正在导入颜色、检查模型并自动摆放..."));
    refresh_controls();

    const int color_selection = m_import_color_mode != nullptr ? m_import_color_mode->GetSelection() : 0;
    AI::ModelImportRequest request;
    request.artifact.local_path = path;
    request.artifact.job_id = m_job_id;
    request.artifact.format = m_artifact_format;
    request.artifact.color_encoding = m_artifact_color_encoding;
    request.artifact.generation_palette =
        !m_displayed_model_palette.empty() ? m_displayed_model_palette : m_job_palette;
    request.artifact.used_printable_colors = m_job_use_printable_colors;
    if (m_model_preview && m_displayed_model_path == path) {
        const auto trial = m_model_preview->import_color_mapping();
        AI::ModelColorTrial choice {trial.mapping_colors, trial.target_colors};
        if (trial.enabled && choice.valid()) request.color_trial = std::move(choice);
        request.face_color_overrides = m_model_preview->face_color_overrides();
        request.face_color_geometry_id = m_model_preview->geometry_id();
    }
    const bool has_color_intent = !m_color_intent_path.empty() || !m_color_intent_schema.empty() ||
                                  !m_color_intent_sha256.empty();
    if (has_color_intent) {
        AI::ColorIntentManifestRef manifest {
            m_color_intent_path.string(), m_color_intent_schema, m_color_intent_sha256
        };
        if (!AI::is_valid_color_intent_manifest_ref(manifest) ||
            !AIModelGenerationClient::validate_color_intent_manifest_file(
                m_color_intent_path, m_color_intent_schema, m_color_intent_sha256, path)) {
            m_color_intent_path.clear(); m_color_intent_schema.clear(); m_color_intent_sha256.clear();
            m_result_summary->SetLabel(_L("颜色清单不匹配；将使用模型自身颜色继续导入。"));
        } else {
            request.artifact.color_intent_manifest = std::move(manifest);
        }
    }
    request.color_mode = color_selection == 2
        ? AI::ImportColorMode::SingleColor
        : color_selection == 1 ? AI::ImportColorMode::AutoMap
        : color_selection == 3 ? AI::ImportColorMode::ManualMatch : AI::ImportColorMode::NativeMatch;
    const AI::ModelImportResult result = m_artifact_consumer.import_artifact(request);
    if (!result.imported()) {
        m_busy = false;
        if (result.outcome == AI::ModelImportOutcome::Cancelled) {
            m_status->SetLabel(_L("已取消导入。"));
            m_result_summary->SetLabel(_L("模型仍保留在本地，可以稍后重新导入。"));
        } else if (result.outcome == AI::ModelImportOutcome::InvalidArtifact) {
            m_status->SetLabel(_L("无法准备本地模型以供导入。"));
            m_result_summary->SetLabel(result.error.empty()
                ? _L("请从模型库重新加载模型后重试。") : from_u8(result.error));
        } else if (result.outcome == AI::ModelImportOutcome::RepairFailed) {
            m_status->SetLabel(_L("自动网格修复失败，模型未导入。"));
            m_result_summary->SetLabel(
                _L("原始模型和修复诊断已保留在 generated_models。") + from_u8(result.error));
        } else {
            m_status->SetLabel(_L("无法导入生成的模型。"));
            m_result_summary->SetLabel(_L("模型已保留在本地，请调整耗材配置后重试。"));
        }
        refresh_controls();
        return;
    }

    const std::string job_id = m_job_id;
    const std::string library_job_id = !m_displayed_model_job_id.empty()
        ? m_displayed_model_job_id : job_id;
    update_library_import_status(library_job_id);
    m_last_imported_model_path = path;
    m_client.record_journey_event("model_imported", library_job_id);
    // The displayed work remains available after import, including its reference
    // image for starting another design from the same input.
    const auto reference_image_path = m_reference_image_path;
    cleanup_files();
    m_reference_image_path = reference_image_path;
    // Import ends the active UI task, not the saved generation. Keep its job
    // state so history can restore the source design and provider identity.
    m_poll_timer.Stop();
    m_job_id.clear();
    m_job_palette.clear();
    m_job_palette_roles.clear();
    m_job_use_printable_colors = false;
    m_job_prompt.clear();
    m_job_style.clear();
    m_job_custom_style.clear();
    m_job_image_path.clear();
    m_job_preview_expected = false;
    m_artifact_format.clear();
    m_artifact_color_encoding.clear();
    m_busy = false;
    m_awaiting_confirmation = false;
    m_awaiting_palette_confirmation = false;
    m_palette_recommendation_confirmed = false;
    m_ready = false;
    m_artifact_download_started = false;
    m_library_model_loaded = false;

    m_workflow_phase->SetLabel(result.manual_repair_required
                                   ? _L("手动修复")
                                   : result.manual_coloring_required ? _L("手动上色") : _L("已导入准备页"));
    update_progress(100, 5, _L("已导入准备页"));
    if (!result.error.empty()) {
        m_status->SetLabel(_L("模型已导入，但无法切换到准备页。"));
        m_result_summary->SetLabel(from_u8(result.error));
    } else if (result.manual_repair_required) {
        m_status->SetLabel(_L("模型已导入准备页；检测到开放边，请先检查再切片。"));
        m_result_summary->SetLabel(_L("已保留模型几何与颜色，可在准备页按需修复。"));
    } else if (result.manual_coloring_required) {
        if (result.color_mapping_collapsed) {
            m_status->SetLabel(_L("多种模型颜色只匹配到一个耗材槽，模型已导入准备页。"));
            m_result_summary->SetLabel(_L("请重新匹配至少两个耗材槽，或在准备页手动上色后再切片。"));
        } else if (result.color_mode == AI::ImportColorMode::ManualMatch ||
                   result.color_mode == AI::ImportColorMode::NativeMatch) {
            m_status->SetLabel(_L("颜色匹配未完成，模型已导入准备页。"));
            m_result_summary->SetLabel(_L("请完成颜色匹配或手动上色后再切片。"));
        } else {
            m_status->SetLabel(_L("自动匹配当前耗材失败，模型已导入准备页。"));
            m_result_summary->SetLabel(_L("请改用手动匹配或在准备页手动上色。"));
        }
    } else if (result.color_mode == AI::ImportColorMode::SingleColor) {
        m_status->SetLabel(_L("模型已按单色导入并自动摆放，已进入准备页。"));
        m_result_summary->SetLabel(_L("模型已忽略原有颜色；可在准备页调整模型、耗材或后续切片设置。"));
    } else if (result.color_mode == AI::ImportColorMode::AutoMap) {
        m_status->SetLabel(_L("模型已自动匹配耗材颜色并进入准备页。"));
        m_result_summary->SetLabel(_L("可打印模型已自动摆放；请在准备页确认颜色和模型状态。"));
    } else {
        m_status->SetLabel(_L("颜色匹配已确认，模型已进入准备页。"));
        m_result_summary->SetLabel(_L("模型颜色已匹配到当前打印机耗材槽；请在准备页确认结果。"));
    }
    load_library_entries();
    refresh_controls();
}

void ModelGenerationPanel::update_adaptive_text_height(wxTextCtrl* control, int minimum_lines, int maximum_lines)
{
    if (control == nullptr || minimum_lines <= 0 || maximum_lines < minimum_lines)
        return;

    wxClientDC dc(control);
    dc.SetFont(control->GetFont());
    const int line_height = std::max(1, dc.GetTextExtent("Ag").GetHeight() + FromDIP(3));
    int available_width = control->GetClientSize().GetWidth() - FromDIP(18);
    if (available_width < FromDIP(120))
        available_width = FromDIP(280);

    const wxString value = control->GetValue();
    int visual_lines = 0;
    size_t start = 0;
    while (start <= value.length()) {
        const size_t end = value.find('\n', start);
        const wxString line = end == wxString::npos ? value.Mid(start) : value.Mid(start, end - start);
        const int line_width = dc.GetTextExtent(line.empty() ? " " : line).GetWidth();
        visual_lines += std::max(1, (line_width + available_width - 1) / available_width);
        if (end == wxString::npos)
            break;
        start = end + 1;
    }

    const int rows = std::clamp(visual_lines, minimum_lines, maximum_lines);
    const int desired_height = rows * line_height + FromDIP(8);
    if (control->GetMinSize().GetHeight() == desired_height &&
        control->GetMaxSize().GetHeight() == desired_height)
        return;
    control->SetMinSize(wxSize(-1, desired_height));
    control->SetMaxSize(wxSize(-1, desired_height));
    control->InvalidateBestSize();
}

void ModelGenerationPanel::refresh_controls()
{
    if (m_shutdown)
        return;
    if (m_preview_loading || m_finishing_running || m_design_history_loading) m_busy = true;
    update_adaptive_text_height(m_prompt, 2, 6);
    update_adaptive_text_height(m_custom_style, 2, 5);
    refresh_palette();
    m_status->Wrap(FromDIP(310));
    const bool image_input = has_image_input();
    const bool image_job = m_job_preview_expected;
    const bool style_selected = m_style->GetSelection() != wxNOT_FOUND;
    const bool custom_style_selected = current_style() == "custom";
    const bool custom_style_ready = !custom_style_selected || !current_custom_style().empty();
    const bool valid_input = (!m_prompt->GetValue().empty() || image_input) && style_selected && custom_style_ready;
    const bool printable_colors = use_printable_colors();
    const bool ai_palette_source = printable_colors && m_palette_source->GetSelection() == 2;
    const bool palette_matches = !printable_colors || m_awaiting_palette_confirmation || m_job_id.empty() ||
        (printable_colors == m_job_use_printable_colors && (!printable_colors || m_palette == m_job_palette));
    const bool stale_job = !m_restoring_input && !m_job_id.empty() &&
                           (!job_inputs_match() || !palette_matches);
    const bool show_review = m_awaiting_confirmation && !image_job && !stale_job;
    const bool local_artifact = is_nonempty_model(m_artifact_path) ||
        (!m_job_id.empty() && is_nonempty_model(temp_path(m_job_id, m_artifact_format)));
    const bool preview_quality_ok = m_palette_quality_ok && m_model_input_eligible;
    const bool multiview_retry = m_job_phase == "multiview_retry";

    m_preprocess->SetLabel(m_awaiting_confirmation && image_job && !stale_job
                               ? _L("换一张")
                               : ai_palette_source && m_awaiting_palette_confirmation && !stale_job
                               ? _L("采用配色并生成预览")
                               : ai_palette_source && !m_palette_recommendation_confirmed
                               ? _L("AI 推荐并生成")
                               : ai_palette_source && m_palette_recommendation_confirmed
                               ? _L("使用当前配色生成预览")
                               : m_awaiting_confirmation && !preview_quality_ok
                               ? _L("重新生成图片预览")
                               : image_input && printable_colors ? _L("生成多色图片预览")
                               : image_input ? _L("生成风格图片预览")
                               : _L("生成 AI 设计图"));
    m_preprocess->SetToolTip(ai_palette_source && !m_palette_recommendation_confirmed
        ? _L("推荐配色后直接生成 1 张 AI 设计图，消耗 API 额度；不自动创建 3D 任务。")
        : _L("生成适合 3D 建模的高质量 AI 设计图。"));
    m_generate->SetLabel(multiview_retry ? _L("重试生成 3D")
                                         : image_job ? _L("生成 3D") : _L("确认提示词并生成 3D"));
    m_generate->SetToolTip(multiview_retry
                               ? _L("重新准备四视图；通过检查后才会创建 1 个付费 3D 任务")
                               : image_job ? _L("确认当前图片并创建 1 个付费 3D 任务")
                               : _L("确认当前提示词并创建 1 个付费 3D 任务"));
    const bool local_model_loading = m_ready && m_artifact_download_started && !m_model_preview_ready;
    m_stop->SetLabel((local_model_loading || m_design_history_loading) ? _L("取消加载") : _L("停止生成"));
    m_stop->SetToolTip(m_design_history_loading
                           ? _L("取消打开历史设计，保留当前内容")
                           : local_model_loading
                           ? _L("只取消当前本地下载和预览加载；已生成的远端模型会保留，可稍后重新加载")
                           : _L("停止本地任务；已经提交给远端的生成任务可能仍会继续运行并计费"));
    m_import->SetLabel(!m_model_preview_ready
                           ? _L("重新加载 3D 模型")
                           : _L("导入到准备页"));
    m_import->SetToolTip(m_visual_quality.available && !m_visual_quality.import_recommended
                             ? _L("外观检查仅供参考，可继续导入；请对照原图确认效果。")
                             : wxString());
    m_discard->SetLabel(_L("重新开始"));
    m_clear_image->Show(image_input);
    m_upload_notice->Show(image_input);
    const bool show_advanced = printable_colors && !m_busy && !m_ready;
    m_advanced_toggle->Show(show_advanced);
    m_advanced_options->Show(show_advanced && m_advanced_options_expanded);
    m_advanced_toggle->SetLabel(m_advanced_options_expanded ? _L("收起高级设置") : _L("显示高级设置"));
    m_model_settings_panel->Show(m_awaiting_confirmation && !stale_job);
    m_import_settings_panel->Show(m_ready && !stale_job);
    m_preprocess_section->Show(show_review);
    m_prepared_prompt_label->Show(show_review);
    m_prepared_prompt->Show(show_review);

    m_prompt->Enable(!m_busy);
    m_style->Enable(!m_busy);
    m_stylized_style->Show(m_style->GetSelection() == 2);
    m_stylized_style->Enable(!m_busy);
    m_custom_style_panel->Show(custom_style_selected);
    m_custom_style->Enable(!m_busy && custom_style_selected);
    refresh_style_recommendation();
    m_quality->Enable(!m_busy);
    m_choose_image->Enable(!m_busy);
    m_clear_image->Enable(!m_busy);
    m_use_printable_colors->Enable(!m_busy);
    m_palette_source->Enable(!m_busy);
    m_import_color_mode->Enable(!m_busy);
    m_custom_color->Enable(!m_busy && printable_colors && m_palette_source->GetSelection() != 0);
    m_add_custom_color->Enable(!m_busy && printable_colors && m_palette_source->GetSelection() != 0 &&
        m_custom_palette.size() < (ai_palette_source ? current_palette_color_count() : Slic3r::AI::kMaxTargetPaletteColors));
    for (wxSpinCtrlDouble* control : {m_print_width, m_nozzle_size, m_line_width, m_minimum_feature})
        control->Enable(!m_busy && printable_colors);
    if (m_shadow_color != nullptr)
        m_shadow_color->Enable(!m_busy && printable_colors);
    m_preprocess->Enable(m_service_available && !m_busy && valid_input &&
                         (ai_palette_source || !printable_colors || !m_palette.empty()));
    m_prepared_prompt->Enable(m_service_available && !m_busy && show_review);
    m_generate->Enable(m_service_available && !m_busy && m_awaiting_confirmation && !stale_job &&
                       (!image_job || m_style_preview_ready));
    m_stop->Enable(m_design_history_loading || (m_busy && !m_job_id.empty() && (local_model_loading || m_service_available)));
    m_retry_service->Enable(!m_service_available && !m_busy && static_cast<bool>(m_service_retry_handler));
    m_import->Enable((local_artifact || m_service_available) && !m_busy &&
                     m_ready && !stale_job &&
                     (m_model_preview_ready || !m_artifact_download_started));
    m_recheck_model->Enable(m_service_available && !m_busy && !m_quality_check_busy && !m_visual_check_busy &&
                            m_model_preview_ready && !m_displayed_model_job_id.empty());
    m_visual_review_model->Enable(m_service_available && !m_busy && !m_quality_check_busy && !m_visual_check_busy &&
                                  m_model_preview_ready && !m_displayed_model_job_id.empty());
    m_visual_review_model->SetLabel(m_reference_image.IsOk() ? _L("AI 对照原图") : _L("AI 视觉复核"));
    const wxString refinement_suffix = from_u8(m_model_refinement.prompt_suffix);
    const bool refinement_already_applied = !refinement_suffix.empty() &&
        m_prompt->GetValue().Find(refinement_suffix) != wxNOT_FOUND;
    m_apply_model_refinement->Enable(!m_busy && m_model_refinement.available &&
                                     !m_model_refinement.prompt_suffix.empty() &&
                                     !refinement_already_applied);
    const bool has_restartable_work = !m_job_id.empty() || m_ready || m_model_preview_ready || m_style_preview_ready;
    m_discard->Enable(!m_busy && has_restartable_work);

    const bool show_preprocess = !m_busy && (!m_ready || stale_job) &&
        (m_job_id.empty() || stale_job || (!m_awaiting_confirmation && !m_ready) ||
         (m_awaiting_confirmation && image_job));
    m_preprocess->Show(show_preprocess);
    m_generate->Show(!m_busy && m_awaiting_confirmation);
    m_stop->Show(m_busy);
    m_retry_service->Show(!m_service_available && !m_busy);
    m_import->Show(!m_busy && m_ready && !stale_job);
    m_discard->Show(!m_busy && has_restartable_work);
    if (!m_busy && ((m_job_id.empty() && !m_ready) || stale_job))
        update_progress(0, 1, _L("输入"));
    if (!m_busy && stale_job)
        m_status->SetLabel(m_awaiting_palette_confirmation
                               ? _L("输入已变化；可重新推荐，或继续使用当前配色。")
                               : _L("输入或颜色已变化，请重新生成图片预览。"));
    if (m_ready && !stale_job) {
        if (!m_busy) {
            update_progress(100, 4, _L("模型已生成 · 待导入"));
            m_generation_progress->Hide();
            m_progress_percent->Hide();
        }
        wxString ready_guidance = _L("检查右侧 3D 模型，然后选择导入方式");
        if (m_visual_quality.available && !m_visual_quality.import_recommended) {
            const bool identity_risk = std::find(
                m_visual_quality.blocking_warnings.begin(), m_visual_quality.blocking_warnings.end(),
                "visual_identity_mismatch") != m_visual_quality.blocking_warnings.end();
            const bool material_risk = std::find(
                m_visual_quality.blocking_warnings.begin(), m_visual_quality.blocking_warnings.end(),
                "visual_material_color_mixing") != m_visual_quality.blocking_warnings.end();
            ready_guidance = identity_risk && material_risk
                ? _L("人脸和材料边界需复核，建议重新优化")
                : identity_risk
                ? _L("人脸相似度需复核，建议对照原图检查")
                : material_risk
                ? _L("材料边界需复核，请检查手臂、衣物和底座")
                : _L("外观检查未通过，请展开风险项复核");
        }
        m_workflow_steps->SetLabel(ready_guidance);
        m_workflow_steps->Wrap(FromDIP(330));
        m_workflow_steps->InvalidateBestSize();
    }
    else if (m_awaiting_palette_confirmation)
        m_workflow_steps->SetLabel(stale_job
                                       ? _L("输入已变化：重新推荐或确认继续使用当前配色")
                                       : _L("修改或确认 AI 推荐的设计目标色"));
    else if (m_awaiting_confirmation && !stale_job)
        m_workflow_steps->SetLabel(multiview_retry
                                       ? _L("付费前四视图检查未通过，可直接重试")
                                       : preview_quality_ok
                                       ? _L("确认右侧图片效果，并选择 3D 模型精度")
                                       : _L("图片存在质量提醒，确认后仍可生成 3D"));
    else if (!m_busy)
        m_workflow_steps->SetLabel(custom_style_selected && !custom_style_ready
                                       ? _L("请补充自定义风格描述")
                                       : valid_input ? _L("下一步：生成图片预览")
                                                     : _L("输入文字、图片，或同时使用两者"));
    m_workflow_steps->Wrap(FromDIP(330));
    m_workflow_steps->InvalidateBestSize();
    if (stale_job && (m_awaiting_confirmation || m_ready))
        m_result_summary->SetLabel(_L("输入内容或颜色模式发生变化，请重新生成预览后继续。"));

    const bool has_preview = m_reference_image.IsOk() || m_style_preview_image.IsOk();
    const bool image_page_active = m_preview_book != nullptr && m_preview_book->GetSelection() == 0;
    if (m_preview_details_pane != nullptr)
        m_preview_details_pane->Show(image_page_active && m_model_views_available);
    if (m_model_decision_panel != nullptr)
        m_model_decision_panel->Hide();
    if (m_model_decision_panel != nullptr) {
        if (auto* model_page = dynamic_cast<wxScrolledWindow*>(m_model_decision_panel->GetParent())) {
            // These sections are hidden until a model is ready. Refit the
            // scrolled page when they appear so they are placed below the
            // comparison row instead of painting over the image panes.
            model_page->Layout();
            model_page->FitInside();
        }
    }
    m_zoom_out->Enable(has_preview && m_preview_zoom_factor > MIN_PREVIEW_ZOOM);
    m_zoom_fit->Enable(has_preview && std::abs(m_preview_zoom_factor - 1.0) > 0.001);
    m_zoom_in->Enable(has_preview && m_preview_zoom_factor < MAX_PREVIEW_ZOOM);
    m_front_model_view->Enable(m_model_preview_ready);
    m_reset_model_view->Enable(m_model_preview_ready);
    refresh_local_recolor_controls();
    refresh_model_finishing();
    if (m_model_preview != nullptr && m_model_preview->GetParent() != nullptr)
        m_model_preview->GetParent()->Layout();
    if (m_preview_details_pane != nullptr && m_preview_details_pane->GetParent() != nullptr)
        m_preview_details_pane->GetParent()->Layout();
    if (auto* scroll = dynamic_cast<wxScrolledWindow*>(m_prompt->GetParent())) {
        scroll->Layout();
        scroll->FitInside();
    }
    if (!m_busy && !m_ready && !m_last_imported_model_path.empty() &&
        m_last_imported_model_path == m_displayed_model_path && m_job_id.empty()) {
        update_progress(100, 4, _L("该结果已导入过"));
        m_workflow_steps->SetLabel(_L("返回当前工程继续调整尺寸、底座与打印适配。生成原件保留在模型库。"));
        m_workflow_steps->Wrap(FromDIP(330));
    }
    if (wxGetApp().dark_mode()) refresh_ai_appearance(this, false);
    Layout();
}

void ModelGenerationPanel::apply_model_quality(const AIModelGenerationClient::ModelQuality& quality)
{
    m_model_quality = quality;
    refresh_model_quality_card();
}

void ModelGenerationPanel::apply_visual_quality(const AIModelGenerationClient::VisualQuality& quality)
{
    m_visual_quality = quality;
    refresh_model_quality_card();
}

void ModelGenerationPanel::apply_model_refinement(
    const AIModelGenerationClient::ModelRefinementAdvice& refinement)
{
    m_model_refinement = refinement;
    refresh_model_quality_card();
}

void ModelGenerationPanel::clear_model_quality()
{
    m_model_quality = {};
    m_visual_quality = {};
    m_model_refinement = {};
    m_quality_check_busy = false;
    m_visual_check_busy = false;
    m_thin_region_navigation_active = false;
    m_thin_region_navigation_index = 0;
    refresh_model_quality_card();
}

void ModelGenerationPanel::refresh_model_quality_card()
{
    if (m_model_quality_panel == nullptr)
        return;
    wxColour foreground(91, 104, 107);
    wxColour background(246, 248, 248);
    wxString status = _L("按需分析模型");
    wxString summary = m_model_preview_ready
        ? _L("可直接继续导入。厚度与悬垂建议结合准备页的尺寸、方向和打印设置判断。")
        : _L("模型生成或加载后可进行结构检查。");
    if (m_quality_check_busy) {
        status = _L("正在检查...");
        summary = _L("正在本地分析拓扑、组件、接地和悬垂，请稍候。");
        foreground = wxColour(31, 122, 116);
        background = wxColour(229, 244, 242);
    } else if (m_model_quality.available) {
        if (m_model_quality.status == "pass") {
            status = _L("结构检查通过");
            summary = _L("本次分析未发现明显结构问题；仍需结合实际打印设置判断。");
            foreground = wxColour(31, 122, 90);
            background = wxColour(232, 246, 238);
        } else if (m_model_quality.status == "review") {
            status = _L("建议复核");
            foreground = wxColour(174, 112, 22);
            background = wxColour(255, 246, 225);
        } else if (m_model_quality.status == "reject") {
            status = _L("未通过结构检查");
            foreground = wxColour(188, 62, 54);
            background = wxColour(253, 235, 233);
        }
        const auto& codes = m_model_quality.status == "reject" ? m_model_quality.errors : m_model_quality.warnings;
        if (!codes.empty()) {
            summary.clear();
            const size_t visible = std::min<size_t>(2, codes.size());
            for (size_t index = 0; index < visible; ++index) {
                if (!summary.empty()) summary += "\n";
                summary += _L("• ") + model_quality_code_label(codes[index]);
            }
            if (codes.size() > visible)
                summary += wxString::Format(_L("\n另有 %llu 项，请展开查看。"),
                    static_cast<unsigned long long>(codes.size() - visible));
        }
    }
    m_model_quality_status->SetLabel(status);
    m_model_quality_status->SetForegroundColour(foreground);
    m_model_quality_panel->SetBackgroundColour(background);
    m_model_quality_summary->SetLabel(summary);
    if (m_model_decision_panel != nullptr) {
        m_model_decision_status->SetLabel(status);
        m_model_decision_status->SetForegroundColour(foreground);
        m_model_decision_summary->SetLabel(summary);
        m_model_decision_panel->SetBackgroundColour(background);
    }
    wxString details;
    if (m_model_quality.available) {
        details << wxString::Format(_L("三角面：%llu · 顶点：%llu · 连通部件：%llu\n"),
                    static_cast<unsigned long long>(m_model_quality.face_count),
                    static_cast<unsigned long long>(m_model_quality.vertex_count),
                    static_cast<unsigned long long>(m_model_quality.component_count));
        if (m_model_quality.bed_contact_area_available) {
            details << wxString::Format(_L("最大部件占比：%.1f%% · 接地跨度：%.1f%% · 接地面积：%.1f%%\n"),
                        m_model_quality.largest_component_face_ratio * 100.0,
                        m_model_quality.contact_span_ratio * 100.0,
                        m_model_quality.bed_contact_area_ratio * 100.0);
            details << wxString::Format(
                        m_model_quality.elevated_downward_surface_ratio_available
                            ? _L("离床向下表面：%.1f%%") : _L("向下表面：%.1f%%"),
                        (m_model_quality.elevated_downward_surface_ratio_available
                            ? m_model_quality.elevated_downward_surface_ratio
                            : m_model_quality.downward_surface_ratio) * 100.0);
            if (m_model_quality.overhang_region_metrics_available)
                details << wxString::Format(_L(" · 显著局部悬垂：%llu 个"),
                            static_cast<unsigned long long>(m_model_quality.significant_overhang_region_count));
            if (m_model_quality.component_thickness_available &&
                m_model_quality.minimum_component_thickness_mm > 0.0)
                details << wxString::Format(_L("\n最薄组件：%.2f mm · 薄型组件：%llu 个"),
                            m_model_quality.minimum_component_thickness_mm,
                            static_cast<unsigned long long>(m_model_quality.thin_component_count));
            if (m_model_quality.local_thickness_available) {
                details << wxString::Format(_L("\n局部厚度采样：%llu 个"),
                            static_cast<unsigned long long>(m_model_quality.local_thickness_sample_count));
                if (m_model_quality.minimum_sampled_local_thickness_mm > 0.0)
                    details << wxString::Format(_L(" · 最薄命中：%.2f mm · 薄面样本：%llu 个"),
                                m_model_quality.minimum_sampled_local_thickness_mm,
                                static_cast<unsigned long long>(m_model_quality.thin_local_surface_sample_count));
                else
                    details << _L(" · 未发现阈值内相对表面");
                if (m_model_quality.thin_local_region_count > 0)
                    details << wxString::Format(_L("\n局部薄壁风险区：%llu 个 · 报告前 %llu 个"),
                                static_cast<unsigned long long>(m_model_quality.thin_local_region_count),
                                static_cast<unsigned long long>(m_model_quality.reported_thin_local_region_count));
            }
            if (m_model_quality.target_palette_metrics_available) {
                details << wxString::Format(
                    _L("\n最终模型目标色：显著 %llu/%llu · 建议至少 %llu · 覆盖 %.1f%%"),
                    static_cast<unsigned long long>(m_model_quality.meaningful_target_palette_color_count),
                    static_cast<unsigned long long>(m_model_quality.target_palette_color_count),
                    static_cast<unsigned long long>(
                        m_model_quality.required_meaningful_target_palette_color_count),
                    m_model_quality.target_palette_surface_coverage_ratio * 100.0);
                if (!m_model_quality.target_palette_surface_usage.empty()) {
                    details << _L("\n逐色表面积：");
                    for (size_t index = 0; index < m_model_quality.target_palette_surface_usage.size(); ++index) {
                        if (index > 0)
                            details << _L(" · ");
                        const auto& usage = m_model_quality.target_palette_surface_usage[index];
                        details << from_u8(usage.color)
                                << wxString::Format(_L(" %.1f%%"), usage.surface_ratio * 100.0);
                    }
                }
            }
        } else {
            details << wxString::Format(_L("最大部件占比：%.1f%% · 接地覆盖：%.1f%% · 向下表面：%.1f%%"),
                        m_model_quality.largest_component_face_ratio * 100.0,
                        m_model_quality.contact_span_ratio * 100.0,
                        m_model_quality.downward_surface_ratio * 100.0);
        }
        const auto& codes = m_model_quality.status == "reject" ? m_model_quality.errors : m_model_quality.warnings;
        for (const std::string& code : codes)
            details += _L("\n• ") + model_quality_code_label(code);
    } else {
        details = _L("尚无结构化质量指标。");
    }
    m_model_quality_details->SetLabel(details);
    m_model_quality_details_pane->Show(m_model_quality.available);

    wxString visual_status = _L("AI 视觉复核：未运行");
    wxString visual_summary = m_model_preview_ready
        ? _L("质量档会自动生成五视图并检查主体、人脸和材料串色；也可点击上方按钮重新复核。")
        : _L("模型准备好后可按需生成五视图并进行 AI 外观复核。");
    wxColour visual_foreground(91, 104, 107);
    if (m_visual_check_busy) {
        visual_status = _L("AI 视觉复核中...");
        visual_summary = _L("正在生成前后左右和等轴视图，并对照原图检查主体、人脸、底座和材料色区，请稍候。");
        visual_foreground = wxColour(31, 122, 116);
    } else if (m_visual_quality.available) {
        if (m_visual_quality.status == "pass") {
            visual_status = wxString::Format(_L("AI 视觉复核通过 · %d 分"), m_visual_quality.score);
            visual_foreground = wxColour(31, 122, 90);
        } else if (m_visual_quality.status == "review") {
            visual_status = wxString::Format(
                m_visual_quality.import_recommended
                    ? _L("AI 建议人工复核 · %d 分")
                    : _L("AI 外观存在提醒 · %d 分"),
                m_visual_quality.score);
            visual_foreground = m_visual_quality.import_recommended
                ? wxColour(174, 112, 22) : wxColour(181, 62, 55);
        } else {
            visual_status = _L("AI 视觉复核暂不可用");
            visual_foreground = wxColour(174, 112, 22);
        }
        visual_summary = from_u8(m_visual_quality.summary);
        const auto& codes = m_visual_quality.status == "unavailable"
            ? m_visual_quality.errors
            : !m_visual_quality.import_recommended && !m_visual_quality.blocking_warnings.empty()
                ? m_visual_quality.blocking_warnings : m_visual_quality.warnings;
        const size_t visible = std::min<size_t>(3, codes.size());
        for (size_t index = 0; index < visible; ++index)
            visual_summary += _L("\n• ") + visual_quality_code_label(codes[index]);
    }
    m_visual_quality_status->SetLabel(visual_status);
    m_visual_quality_status->SetForegroundColour(visual_foreground);
    m_visual_quality_summary->SetLabel(visual_summary);
    // The compact decision card is the only quality summary visible without
    // expanding advanced details.  A blocking identity/material result must
    // therefore outrank non-blocking structural review warnings; otherwise a
    // bad portrait can appear to need only support or thin-wall inspection.
    if (m_model_decision_panel != nullptr &&
        !(m_model_quality.available && m_model_quality.status == "reject") &&
        m_visual_quality.available && !m_visual_quality.import_recommended) {
        m_model_decision_status->SetLabel(wxString::Format(
            _L("外观提醒 · %d 分"), m_visual_quality.score));
        m_model_decision_status->SetForegroundColour(wxColour(181, 62, 55));
        m_model_decision_summary->SetLabel(visual_summary);
        m_model_decision_summary->Wrap(FromDIP(620));
        m_model_decision_summary->InvalidateBestSize();
        m_model_decision_panel->SetBackgroundColour(wxColour(253, 235, 233));
    }
    if (m_model_refinement.available && !m_model_refinement.prompt_suffix.empty()) {
        wxString refinement_summary = from_u8(m_model_refinement.summary);
        const size_t visible = std::min<size_t>(3, m_model_refinement.issues.size());
        for (size_t index = 0; index < visible; ++index)
            refinement_summary += _L("\n• ") + from_u8(m_model_refinement.issues[index].title);
        if (m_model_refinement.issues.size() > visible)
            refinement_summary += wxString::Format(
                _L("\n• 另有 %llu 类建议"),
                static_cast<unsigned long long>(m_model_refinement.issues.size() - visible));
        m_model_refinement_summary->SetLabel(refinement_summary);
        m_model_refinement_panel->Show();
    } else {
        m_model_refinement_summary->SetLabel(wxEmptyString);
        m_model_refinement_panel->Hide();
    }
    m_model_quality_panel->Layout();
    m_model_quality_panel->GetParent()->Layout();
    if (m_model_decision_panel != nullptr)
        m_model_decision_panel->Layout();
}

void ModelGenerationPanel::on_recheck_model(wxCommandEvent&)
{
    if (m_quality_check_busy || m_displayed_model_job_id.empty() || !m_model_preview_ready)
        return;
    const std::string job_id = m_displayed_model_job_id;
    const uint64_t sequence = m_sequence;
    m_thin_region_navigation_active = false;
    m_thin_region_navigation_index = 0;
    m_quality_check_busy = true;
    m_status->SetLabel(_L("正在重新检查当前 3D 模型..."));
    refresh_model_quality_card();
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.recheck(job_id,
        [weak, sequence, job_id](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->m_quality_check_busy = false;
                weak->apply_model_quality(status.model_quality);
                weak->apply_visual_quality(status.visual_quality);
                weak->apply_model_refinement(status.refinement);
                weak->m_status->SetLabel(status.model_quality.status == "pass"
                    ? _L("结构检查通过。") : status.model_quality.status == "review"
                    ? _L("结构检查完成，建议复核提示项。") : _L("结构检查未通过，已禁用导入。"));
                weak->refresh_controls();
            });
        },
        [weak, sequence, job_id](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, error = std::move(error)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->m_quality_check_busy = false;
                weak->m_status->SetLabel(_L("无法重新检查历史模型：") + from_u8(error));
                weak->refresh_model_quality_card();
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::on_visual_review_model(wxCommandEvent&)
{
    if (m_visual_check_busy || m_quality_check_busy || m_displayed_model_job_id.empty() || !m_model_preview_ready)
        return;
    MessageDialog confirm(
        this,
        _L("要生成当前模型的五视图并调用 AI 对照检查吗？\n\n"
           "会重点检查主体/人脸与原图的相似度，以及肤色、衣物、头发和底座是否串色。\n"
           "五视图会发送给 AI 服务，此操作可能消耗 API 额度；严重的人脸偏差或材料串色会提示不建议导入，但仍可手动确认继续。"),
        _L("确认 AI 视觉复核"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;
    const std::string job_id = m_displayed_model_job_id;
    const uint64_t sequence = m_sequence;
    m_visual_check_busy = true;
    m_status->SetLabel(_L("正在生成多视角并进行 AI 外观复核..."));
    refresh_model_quality_card();
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.visual_review(job_id,
        [weak, sequence, job_id](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->m_visual_check_busy = false;
                weak->apply_visual_quality(status.visual_quality);
                weak->apply_model_refinement(status.refinement);
                weak->m_status->SetLabel(status.visual_quality.status == "pass"
                    ? _L("AI 视觉复核完成，未发现明显外观风险。")
                    : status.visual_quality.status == "review"
                    ? status.visual_quality.import_recommended
                        ? _L("AI 视觉复核完成，建议人工确认提示项。")
                        : _L("AI 外观存在提醒，不建议直接导入；请查看人脸和串色风险。")
                    : _L("AI 视觉复核暂不可用，可稍后重试。"));
                weak->refresh_controls();
            });
        },
        [weak, sequence, job_id](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, error = std::move(error)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->m_visual_check_busy = false;
                weak->m_status->SetLabel(_L("无法完成 AI 视觉复核：") + from_u8(error));
                weak->refresh_model_quality_card();
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::on_apply_model_refinement(wxCommandEvent&)
{
    if (m_busy || !m_model_refinement.available || m_model_refinement.prompt_suffix.empty())
        return;
    const wxString suffix = from_u8(m_model_refinement.prompt_suffix);
    wxString prompt = m_prompt->GetValue();
    if (prompt.Find(suffix) != wxNOT_FOUND) {
        m_status->SetLabel(_L("优化建议已在文字输入中，不会重复添加。"));
        refresh_controls();
        return;
    }
    const wxString candidate = prompt + (prompt.empty() ? wxString() : _L("\n\n")) + suffix;
    const auto encoded = candidate.ToUTF8();
    if (!encoded || encoded.length() > MAX_MODEL_INPUT_BYTES) {
        m_status->SetLabel(_L("文字输入接近长度上限，请先精简原描述再应用优化建议。"));
        return;
    }
    m_prompt->ChangeValue(candidate);
    m_prompt->SetInsertionPointEnd();
    m_prompt->SetFocus();
    refresh_controls();
    m_status->SetLabel(_L("优化建议已加入下一次输入；尚未调用付费服务，请检查后重新生成图片预览。"));
    m_result_summary->SetLabel(_L("当前模型和质量报告仍保留，可与下一次生成结果对比。"));
}

std::vector<std::string> ModelGenerationPanel::local_recolor_palette() const
{
    std::vector<std::string> palette = project_palette();
    if (palette.empty())
        palette = !m_displayed_model_palette.empty() ? m_displayed_model_palette : m_job_palette;
    if (palette.size() > Slic3r::AI::kMaxPhysicalColorChannels)
        palette.resize(Slic3r::AI::kMaxPhysicalColorChannels);
    return palette;
}

void ModelGenerationPanel::update_region_mode()
{
    if (!m_model_preview || !m_finishing_selection_operation) return;
    const auto gesture = static_cast<ModelPreview3D::SelectionGesture>(m_finishing_selection_operation->GetSelection());
    m_model_preview->set_selection_gesture(gesture, FromDIP(m_finishing_radius->GetValue() * 4));
    m_model_preview->set_selection_operation(AI::RegionSelectionOperation::AddSimilar);
    m_model_preview->set_selection_settings(AI::RegionSelectionSettings {});
    m_model_preview->set_selection_preview_color(ColorRGBA(1.0f, 0.55f, 0.0f, 1.0f));
}

void ModelGenerationPanel::refresh_local_recolor_controls()
{
    if (m_local_recolor_panel == nullptr || m_local_recolor_toggle == nullptr ||
        m_local_recolor_controls == nullptr)
        return;
    const bool ready = m_model_preview_ready && m_model_preview != nullptr &&
                       m_model_preview->region_editing_ready();
    if (!ready)
        m_local_recolor_toggle->SetValue(false);
    const bool recolor_tool = m_finishing_workbench && m_finishing_tool->GetSelection() == 4;
    const bool editing = ready && recolor_tool && m_finishing_candidate.empty();
    if (editing) update_region_mode();
    m_local_recolor_toggle->SetValue(editing);
    m_local_recolor_toggle->Hide();
    const auto has_tiny_color_regions = [this]() {
        const auto contains = [](const std::vector<std::string>& codes) {
            return std::find(codes.begin(), codes.end(), "tiny_printable_color_regions") != codes.end();
        };
        return contains(m_model_quality.warnings) || contains(m_model_quality.errors);
    };
    m_local_recolor_panel->Show(ready && recolor_tool);
    m_local_recolor_controls->Show(editing);
    const bool repair_color_regions = has_tiny_color_regions();
    m_local_recolor_toggle->SetLabel(
        editing ? _L("收起改色") : repair_color_regions ? _L("修复杂色块") : _L("开始改色"));
    m_local_recolor_toggle->SetToolTip(repair_color_regions
        ? _L("检查已识别的过小耗材色块，在模型上选择区域并合并到合适的目标色")
        : _L("打开局部改色工具，在模型上直接选择需要换色的部位"));
    m_local_recolor_toggle->Enable(ready && !m_busy);
    if (m_locate_overhang_regions != nullptr)
        m_locate_overhang_regions->Enable(ready && !m_busy);
    if (m_locate_thin_regions != nullptr) {
        m_locate_thin_regions->SetLabel(
            m_thin_region_navigation_active && m_model_quality.thin_local_regions.size() > 1
                ? _L("下一处薄壁") : _L("定位薄壁"));
        m_locate_thin_regions->Enable(
            ready && !m_busy && !m_model_quality.thin_local_face_indices.empty());
    }
    if (m_model_preview != nullptr)
        m_model_preview->set_selection_enabled(editing || (m_finishing_workbench && (m_finishing_tool->GetSelection() == 1 || m_finishing_tool->GetSelection() == 5) && !m_busy && m_finishing_candidate.empty()));

    const std::vector<std::string> palette = local_recolor_palette();
    if (palette != m_region_palette) {
        m_region_palette = palette;
        m_region_color_index = palette.empty()
            ? 0 : std::clamp(m_region_color_index, 0, int(palette.size()) - 1);
    }
    const bool has_selection = m_model_preview != nullptr && m_model_preview->selected_face_count() > 0;
    for (size_t index = 0; index < m_region_color_buttons.size(); ++index) {
        wxToggleButton* button = m_region_color_buttons[index];
        const bool visible = index < palette.size();
        button->Show(visible);
        if (!visible)
            continue;
        const wxColour color(from_u8(palette[index]));
        const bool selected = int(index) == m_region_color_index;
        button->SetValue(selected);
        button->SetLabel(wxString::Format(
            selected ? _L("耗材 %llu（已选）\n") : _L("耗材 %llu\n"),
            static_cast<unsigned long long>(index + 1)) + from_u8(palette[index]));
        button->SetToolTip(wxString::Format(
            _L("将选中区域改为耗材 %llu："), static_cast<unsigned long long>(index + 1)) +
            from_u8(palette[index]));
        if (color.IsOk()) {
            button->SetName("ai_content_color");
            button->SetBackgroundColour(color);
            const double luminance = 0.299 * color.Red() + 0.587 * color.Green() + 0.114 * color.Blue();
            button->SetForegroundColour(luminance >= 150.0 ? *wxBLACK : *wxWHITE);
        }
        button->Enable(editing && !m_busy);
    }

    m_apply_region_color->SetLabel(palette.empty()
        ? _L("没有可用耗材颜色")
        : wxString::Format(_L("预览耗材 %d 的效果"), m_region_color_index + 1));
    m_apply_region_color->Enable(editing && !m_busy && has_selection && !palette.empty());
    m_local_recolor_panel->Layout();
    if (m_local_recolor_panel->GetParent() != nullptr)
        m_local_recolor_panel->GetParent()->Layout();
}

void ModelGenerationPanel::on_apply_local_recolor(wxCommandEvent&)
{
    preview_model_finishing();
}

std::vector<size_t> ModelGenerationPanel::valid_project_slots() const
{
    return m_palette_provider.printable_palette().valid_slots;
}

std::vector<size_t> ModelGenerationPanel::compatible_project_slots() const
{
    return m_palette_provider.printable_palette().compatible_slots;
}

std::vector<std::string> ModelGenerationPanel::project_palette() const
{
    return m_palette_provider.printable_palette().compatible_colors;
}

std::vector<std::string> ModelGenerationPanel::current_palette() const
{
    if (!use_printable_colors())
        return {};
    auto palette = m_palette_source != nullptr && m_palette_source->GetSelection() == 2 ? m_custom_palette : project_palette();
    if (current_style() == "sculpture" && palette.size() > 1)
        palette.resize(1);
    return palette;
}

size_t ModelGenerationPanel::current_palette_color_count() const
{
    if (current_style() == "sculpture")
        return 1;
    if (m_palette_color_count == nullptr || m_palette_color_count->GetSelection() == wxNOT_FOUND)
        return Slic3r::AI::kLegacyDefaultTargetPaletteColors;
    const size_t count = static_cast<size_t>(m_palette_color_count->GetSelection()) +
                         Slic3r::AI::kMinTargetPaletteColors;
    return Slic3r::AI::is_supported_target_palette_color_count(count)
        ? count
        : Slic3r::AI::kLegacyDefaultTargetPaletteColors;
}

AIModelGenerationClient::PaletteRoles ModelGenerationPanel::current_palette_roles() const
{
    if (!use_printable_colors())
        return {};
    const std::vector<std::string> palette = current_palette();
    if (palette.empty())
        return {};
    const size_t expected_roles = std::min(palette.size(), PALETTE_ROLE_IDS.size());
    std::set<std::string> assigned_colors;
    AIModelGenerationClient::PaletteRoles active_roles;
    for (size_t index = 0; index < expected_roles; ++index) {
        const auto role = m_palette_roles.find(PALETTE_ROLE_IDS[index]);
        if (role == m_palette_roles.end()
            || std::find(palette.begin(), palette.end(), role->second) == palette.end()
            || !assigned_colors.insert(role->second).second)
            return automatic_palette_roles(palette);
        active_roles.emplace(role->first, role->second);
    }
    return active_roles;
}

void ModelGenerationPanel::refresh_palette_roles(const std::vector<std::string>& palette)
{
    if (palette != m_palette_roles_source) {
        m_palette_roles_source = palette;
        m_palette_roles = automatic_palette_roles(palette);
    }
    for (size_t index = 0; index < m_palette_role_choices.size(); ++index) {
        wxChoice* choice = m_palette_role_choices[index];
        if (choice == nullptr)
            continue;
        choice->Freeze();
        choice->Clear();
        for (const std::string& color : palette)
            choice->Append(from_u8(color));
        const auto role = m_palette_roles.find(PALETTE_ROLE_IDS[index]);
        if (role != m_palette_roles.end()) {
            const auto color = std::find(palette.begin(), palette.end(), role->second);
            choice->SetSelection(color == palette.end() ? wxNOT_FOUND : int(std::distance(palette.begin(), color)));
            choice->Enable(!m_busy && use_printable_colors());
        } else {
            choice->SetSelection(wxNOT_FOUND);
            choice->Enable(false);
        }
        choice->Thaw();
    }
}

void ModelGenerationPanel::on_palette_role_changed(size_t role_index)
{
    if (m_busy || role_index >= m_palette_role_choices.size())
        return;
    wxChoice* choice = m_palette_role_choices[role_index];
    const int selection = choice == nullptr ? wxNOT_FOUND : choice->GetSelection();
    const std::vector<std::string> palette = current_palette();
    if (selection == wxNOT_FOUND || selection >= int(palette.size()))
        return;
    const std::string role = PALETTE_ROLE_IDS[role_index];
    const std::string selected = palette[selection];
    const std::string previous = m_palette_roles[role];
    for (auto& [other_role, color] : m_palette_roles) {
        if (other_role != role && color == selected) {
            color = previous;
            break;
        }
    }
    m_palette_roles[role] = selected;
    refresh_palette_roles(palette);
    refresh_controls();
}

void ModelGenerationPanel::request_style_recommendation()
{
    if (m_shutdown || m_selected_image_path.empty())
        return;
    const uint64_t sequence = ++m_style_recommendation_sequence;
    const boost::filesystem::path image_path = m_selected_image_path;
    m_style_recommendation_loading = m_service_available;
    m_style_recommendation_available = false;
    m_style_recommendation = {};
    refresh_style_recommendation();
    if (!m_service_available) {
        refresh_controls();
        return;
    }

    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.recommend_image_style(
        m_prompt->GetValue().ToUTF8().data(), image_path,
        [weak, sequence, image_path](AIModelGenerationClient::StyleRecommendation recommendation) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, image_path, recommendation = std::move(recommendation)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_style_recommendation_sequence ||
                    image_path != weak->m_selected_image_path)
                    return;
                weak->m_style_recommendation_loading = false;
                weak->m_style_recommendation_available = true;
                weak->m_style_recommendation = std::move(recommendation);
                if (!weak->m_style_user_selected)
                    weak->select_style(weak->m_style_recommendation.primary, false);
                else
                    weak->refresh_controls();
            });
        },
        [weak, sequence, image_path](std::string error) {
            if (!weak) return;
            BOOST_LOG_TRIVIAL(warning) << "Local model style recommendation failed: " << error;
            wxGetApp().CallAfter([weak, sequence, image_path]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_style_recommendation_sequence ||
                    image_path != weak->m_selected_image_path)
                    return;
                weak->m_style_recommendation_loading = false;
                weak->m_style_recommendation_available = false;
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::select_style(const std::string& style, bool user_selected)
{
    if (m_style == nullptr)
        return;
    m_style->SetToolTip(wxString());
    m_style->SetSelection(style_selection(style));
    m_stylized_style->SetSelection(stylized_style_selection(style));
    if (user_selected)
        m_style_user_selected = true;
    const bool multicolor = style_uses_printable_colors(current_style());
    if (m_use_printable_colors != nullptr)
        m_use_printable_colors->SetValue(multicolor);
    if (m_import_color_mode != nullptr)
        m_import_color_mode->SetSelection(multicolor ? 0 : 2);
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::refresh_style_recommendation()
{
    if (m_style_recommendation_panel == nullptr)
        return;
    const bool show = has_image_input() &&
        (m_style_recommendation_loading || m_style_recommendation_available);
    m_style_recommendation_panel->Show(show);
    if (!show)
        return;

    const bool alternatives_visible = m_style_recommendation_available &&
        m_style_recommendation.alternatives.size() == m_style_recommendation_alternatives.size();
    if (m_style_recommendation_loading) {
        m_style_recommendation_title->SetLabel(_L("正在推荐风格..."));
        m_style_recommendation_reason->SetLabel(_L("只在本机分析，不会调用付费服务。"));
    } else if (m_style_recommendation_available) {
        m_style_recommendation_title->SetLabel(
            _L("推荐：") + style_label(m_style_recommendation.primary));
        m_style_recommendation_reason->SetLabel(
            style_recommendation_reason(m_style_recommendation.reason));
    } else {
        m_style_recommendation_title->SetLabel(_L("风格推荐暂不可用"));
        m_style_recommendation_reason->SetLabel(_L("可直接使用上方风格选择，不影响继续生成。"));
    }
    m_style_recommendation_reason->Wrap(FromDIP(290));
    m_style_recommendation_alternative_label->Show(alternatives_visible);
    for (size_t index = 0; index < m_style_recommendation_alternatives.size(); ++index) {
        wxButton* button = m_style_recommendation_alternatives[index];
        button->Show(alternatives_visible);
        if (alternatives_visible)
            button->SetLabel(style_label(m_style_recommendation.alternatives[index]));
        button->Enable(alternatives_visible && !m_busy);
    }
    m_style_recommendation_panel->Layout();
}

bool ModelGenerationPanel::use_printable_colors() const
{
    // Color constraints belong to post-generation Orca material matching.
    return false;
}

std::string ModelGenerationPanel::current_style() const
{
    if (m_style != nullptr && m_style->GetSelection() == wxNOT_FOUND)
        return {};
    return selected_style(m_style == nullptr ? 0 : m_style->GetSelection(),
                          m_stylized_style == nullptr ? 1 : m_stylized_style->GetSelection());
}

std::string ModelGenerationPanel::current_custom_style() const
{
    if (m_custom_style == nullptr || current_style() != "custom")
        return {};
    wxString value = m_custom_style->GetValue();
    value.Trim(true).Trim(false);
    return value.ToUTF8().data();
}

wxString ModelGenerationPanel::current_style_label() const
{
    return style_label(current_style());
}

int ModelGenerationPanel::current_face_limit() const
{
    return current_generation_profile() == "performance" ? 300000 : 1000000;
}

std::string ModelGenerationPanel::current_generation_profile() const
{
    return m_quality != nullptr && m_quality->GetSelection() == 1 ? "performance" : "quality";
}

wxString ModelGenerationPanel::current_generation_profile_label() const
{
    return current_generation_profile() == "performance" ? _L("高性能") : _L("高质量（推荐）");
}

AIModelGenerationClient::ImagePrintSettings ModelGenerationPanel::current_print_settings() const
{
    AIModelGenerationClient::ImagePrintSettings settings;
    if (m_print_width != nullptr) settings.width_mm = m_print_width->GetValue();
    if (m_nozzle_size != nullptr) settings.nozzle_mm = m_nozzle_size->GetValue();
    if (m_line_width != nullptr) settings.line_width_mm = m_line_width->GetValue();
    if (m_minimum_feature != nullptr) settings.minimum_feature_mm = m_minimum_feature->GetValue();
    static constexpr std::array<const char*, 4> shadows {"blue", "red", "green", "white"};
    const int selection = m_shadow_color == nullptr ? 0 : m_shadow_color->GetSelection();
    settings.shadow_color = shadows[selection >= 0 && selection < int(shadows.size()) ? selection : 0];
    return settings;
}

bool ModelGenerationPanel::has_image_input() const
{
    return !m_selected_image_path.empty();
}

bool ModelGenerationPanel::job_uses_image() const
{
    return !m_job_image_path.empty();
}

bool ModelGenerationPanel::job_inputs_match() const
{
    return job_base_inputs_match() &&
           (!use_printable_colors() || m_awaiting_palette_confirmation || current_palette_roles() == m_job_palette_roles);
}

bool ModelGenerationPanel::job_base_inputs_match() const
{
    const auto settings = current_print_settings();
    const bool print_matches = std::abs(settings.width_mm - m_job_print_settings.width_mm) < 0.001 &&
        std::abs(settings.nozzle_mm - m_job_print_settings.nozzle_mm) < 0.001 &&
        std::abs(settings.line_width_mm - m_job_print_settings.line_width_mm) < 0.001 &&
        std::abs(settings.minimum_feature_mm - m_job_print_settings.minimum_feature_mm) < 0.001 &&
        settings.shadow_color == m_job_print_settings.shadow_color;
    const bool palette_count_matches = !use_printable_colors() || m_palette_source == nullptr || m_palette_source->GetSelection() != 2 ||
                                       current_palette_color_count() == m_job_palette_color_count;
    return m_job_id.empty() || (m_prompt->GetValue() == m_job_prompt && m_selected_image_path == m_job_image_path &&
                                 current_style() == m_job_style && current_custom_style() == m_job_custom_style &&
                                 palette_count_matches && print_matches);
}

void ModelGenerationPanel::remove_custom_color(const std::string& color)
{
    if (m_busy || m_palette_source->GetSelection() == 0)
        return;
    const auto item = std::find(m_custom_palette.begin(), m_custom_palette.end(), color);
    if (item != m_custom_palette.end())
        m_custom_palette.erase(item);
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::replace_recommended_color(size_t index)
{
    if (m_busy || m_palette_source->GetSelection() != 2 || index >= m_custom_palette.size())
        return;
    wxColourData data;
    data.SetChooseFull(true);
    data.SetColour(wxColour(from_u8(m_custom_palette[index])));
    wxColourDialog dialog(this, &data);
    if (dialog.ShowModal() != wxID_OK)
        return;
    std::string replacement = dialog.GetColourData().GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    std::transform(replacement.begin(), replacement.end(), replacement.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    const auto duplicate = std::find(m_custom_palette.begin(), m_custom_palette.end(), replacement);
    if (duplicate != m_custom_palette.end() && size_t(std::distance(m_custom_palette.begin(), duplicate)) != index) {
        show_input_hint(_L("这个颜色已经在当前目标色板中。"));
        return;
    }
    const std::string previous = m_custom_palette[index];
    m_custom_palette[index] = replacement;
    for (auto& [role, color] : m_palette_roles)
        if (color == previous) color = replacement;
    for (auto& color : m_palette_recommendation.colors)
        if (color.hex == previous) color.hex = replacement;
    if (std::find(m_user_adjusted_palette_colors.begin(), m_user_adjusted_palette_colors.end(), replacement) ==
        m_user_adjusted_palette_colors.end())
        m_user_adjusted_palette_colors.emplace_back(replacement);
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::refresh_palette_recommendation()
{
    if (m_palette_recommendation_panel == nullptr || m_palette_source == nullptr)
        return;
    const bool ai_source = use_printable_colors() && m_palette_source->GetSelection() == 2;
    m_palette_recommendation_panel->Show(ai_source);
    if (!ai_source)
        return;

    const bool stale = !m_restoring_input && !m_job_id.empty() && !job_base_inputs_match();
    m_palette_color_count->Enable(!m_busy && current_style() != "sculpture");
    if (current_style() == "sculpture") m_palette_color_count->SetSelection(0);
    if (m_busy && !m_awaiting_confirmation) {
        const bool generating_model =
            m_job_phase == "preparing_multiview" || m_job_phase == "generating" ||
            m_job_phase == "texturing" || m_job_phase == "converting" ||
            m_job_phase == "downloading_artifact" || m_job_phase == "checking_model";
        m_palette_recommendation_summary->SetLabel(
            !m_palette_recommendation_confirmed
                ? _L("AI 正在推荐易区分、适合打印的配色...")
                : generating_model
                ? _L("已采用当前配色；正在生成并检查 3D 模型...")
                : _L("正在生成并检查图片预览..."));
    }
    else if (m_palette_recommendation.available) {
        wxString text = _L("推荐配色已显示在上方色块中，可点击色块删除或添加新颜色。");
        if (stale)
            text = _L("输入已变化；可继续使用当前配色，或重新推荐。");
        else if (m_palette_recommendation_confirmed)
            text = _L("已采用当前配色；生成预览后仍可返回调整。");
        m_palette_recommendation_summary->SetLabel(text);
    } else {
        m_palette_recommendation_summary->SetLabel(
            m_palette_recommendation_confirmed
                ? _L("已恢复上次 AI 配色；可继续换图或生成 3D。")
                : _L("点击“AI 推荐并生成”将进行配色推荐和 1 次 AI 生图，消耗 API 额度；不会修改耗材槽。"));
    }
    m_palette_recommendation_summary->Wrap(FromDIP(300));

    const std::vector<std::string> palette = current_palette();
    for (size_t index = 0; index < m_palette_recommendation_cards.size(); ++index) {
        m_palette_recommendation_cards[index]->Show(false);
        if (index >= palette.size())
            continue;
        const std::string& hex = palette[index];
        m_palette_recommendation_swatches[index]->SetName("ai_content_color");
        m_palette_recommendation_swatches[index]->SetBackgroundColour(wxColour(from_u8(hex)));
        const auto detail = std::find_if(
            m_palette_recommendation.colors.begin(), m_palette_recommendation.colors.end(),
            [&hex](const AIModelGenerationClient::PaletteRecommendationColor& color) { return color.hex == hex; });
        wxString label = from_u8(hex);
        if (detail != m_palette_recommendation.colors.end()) {
            label += _L(" · ") + from_u8(detail->name) + _L(" · ") + from_u8(detail->usage) +
                     "\n" + from_u8(detail->reason);
        } else {
            label += _L(" · 用户添加的目标色");
        }
        if (std::find(m_user_adjusted_palette_colors.begin(), m_user_adjusted_palette_colors.end(), hex) !=
            m_user_adjusted_palette_colors.end())
            label += _L("（用户已调整）");
        m_palette_recommendation_details[index]->SetLabel(label);
        m_palette_recommendation_details[index]->Wrap(FromDIP(230));
        m_palette_recommendation_replace[index]->Enable(!m_busy);
        m_palette_recommendation_remove[index]->Enable(!m_busy && palette.size() > 1);
    }
    const bool valid_input = !m_prompt->GetValue().empty() || has_image_input();
    m_recommend_palette->SetLabel(m_palette_recommendation.available ? _L("重新推荐配色") : _L("AI 推荐配色"));
    m_recommend_palette->Enable(m_service_available && !m_busy && valid_input);
    m_recommend_palette->Show(!m_busy);
    m_confirm_recommended_palette->Show(false);
    m_confirm_recommended_palette->SetLabel(stale ? _L("继续使用此配色") : _L("确认配色并生成预览"));
    m_confirm_recommended_palette->Enable(
        m_service_available && !m_busy && m_awaiting_palette_confirmation && !palette.empty());
    m_palette_recommendation_panel->Layout();
}

void ModelGenerationPanel::refresh_palette()
{
    if (m_palette_sizer == nullptr || m_palette_summary == nullptr)
        return;
    const std::vector<std::string> palette = current_palette();
    refresh_palette_roles(palette);
    const bool enabled = use_printable_colors();
    const bool custom = m_palette_source->GetSelection() != 0;
    const bool palette_changed = palette != m_palette || custom != m_palette_is_custom;
    if (palette_changed) {
        m_palette = palette;
        m_palette_is_custom = custom;
        m_palette_sizer->Clear(true);
        for (const std::string& color : m_palette) {
            auto* swatch = new wxPanel(m_palette_panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)), wxBORDER_SIMPLE);
            swatch->SetMinSize(wxSize(FromDIP(24), FromDIP(24)));
            swatch->SetName("ai_content_color");
            swatch->SetBackgroundColour(wxColour(wxString::FromUTF8(color)));
            swatch->SetToolTip(wxString::FromUTF8(color) + (custom ? _L(" · 点击移除") : wxString()));
            if (custom) {
                swatch->SetCursor(wxCursor(wxCURSOR_HAND));
                swatch->Bind(wxEVT_LEFT_UP, [this, color](wxMouseEvent&) { remove_custom_color(color); });
            }
            m_palette_sizer->Add(swatch);
        }
    }
    if (!enabled) {
        m_palette_summary->SetLabel(current_style() == "sculpture"
            ? _L("单材质写实造型，不限定具体色值。")
            : _L("不限制颜色数量，优先生成高质量、适合 3D 建模的设计图。"));
        m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    } else if (m_palette.empty() && m_palette_source->GetSelection() == 2) {
        m_palette_summary->SetLabel(_L("尚未生成 AI 设计目标色。"));
        m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    } else if (m_palette.empty()) {
        m_palette_summary->SetLabel(_L("当前没有配置有效的耗材颜色。"));
        m_palette_summary->SetForegroundColour(wxColour(180, 55, 55));
    } else if (!m_job_palette.empty() && m_palette != m_job_palette) {
        m_palette_summary->SetLabel(_L("耗材颜色已变化，请重新生成预览以使用当前色板。"));
        m_palette_summary->SetForegroundColour(wxColour(180, 55, 55));
    } else if (m_palette_source->GetSelection() == 2) {
        m_palette_summary->SetLabel(wxString::Format(
            _L("%llu 种 AI 设计目标色 · 导入时由你匹配实际耗材"),
            static_cast<unsigned long long>(m_palette.size())));
        m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    } else if (m_palette_source->GetSelection() == 1) {
        m_palette_summary->SetLabel(wxString::Format(_L("%llu 种自定义颜色 · 点击色块可移除"),
                                                     static_cast<unsigned long long>(m_palette.size())));
        m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    } else {
        const size_t valid_slots = valid_project_slots().size();
        const size_t compatible_slots = compatible_project_slots().size();
        if (compatible_slots < valid_slots) {
            m_palette_summary->SetLabel(wxString::Format(
                _L("已选择 %llu 种兼容耗材色（最多 6 种）\n已排除 %llu 个不兼容或超出上限的槽位"),
                static_cast<unsigned long long>(m_palette.size()),
                static_cast<unsigned long long>(valid_slots - compatible_slots)));
        } else {
            m_palette_summary->SetLabel(wxString::Format(_L("当前耗材：%llu 种颜色"),
                                                         static_cast<unsigned long long>(m_palette.size())));
        }
        m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    }
    if (enabled && palette.size() > 1 && minimum_palette_distance(palette) < 12.0) {
        m_palette_summary->SetLabel(m_palette_summary->GetLabel() +
                                    _L("\n提示：部分耗材颜色非常接近，打印后色区可能不易区分。"));
        m_palette_summary->SetForegroundColour(wxColour(174, 112, 22));
    }
    m_palette_source->Show(true);
    m_palette_panel->Show(enabled);
    m_palette_roles_panel->Show(enabled && !m_palette.empty());
    m_custom_color_panel->Show(enabled && custom);
    refresh_palette_recommendation();
    m_palette_panel->Layout();
    m_palette_panel->GetParent()->Layout();
}

void ModelGenerationPanel::reset(bool remove_remote)
{
    ++m_design_history_sequence;
    m_design_history_loading = false;
    m_restoring_input = false;
    m_poll_timer.Stop();
    m_client.cancel_current();
    const std::string old_job = m_job_id;
    const bool keep_assets = !old_job.empty() && has_persisted_generation_assets(generated_models_root(), old_job);
    ++m_sequence;
    cleanup_files();
    m_job_id.clear();
    m_job_phase.clear();
    m_job_provider_name.clear();
    m_job_provider_task_id.clear();
    m_job_provider_conversion_task_id.clear();
    m_job_palette.clear();
    m_job_palette_roles.clear();
    m_job_palette_color_count = Slic3r::AI::kLegacyDefaultTargetPaletteColors;
    m_job_use_printable_colors = false;
    m_job_prompt.clear();
    m_job_style.clear();
    m_job_custom_style.clear();
    m_job_face_limit = 1000000;
    m_job_generation_profile = "quality";
    m_job_image_path.clear();
    m_job_preview_expected = false;
    m_artifact_format.clear();
    m_artifact_color_encoding.clear();
    m_busy = false;
    m_poll_connection_failures = 0;
    m_awaiting_confirmation = false;
    m_awaiting_palette_confirmation = false;
    m_palette_recommendation_confirmed = false;
    m_ready = false;
    m_artifact_download_started = false;
    m_model_preview_ready = false;
    m_library_model_loaded = false;
    m_displayed_model_path.clear();
    m_displayed_model_job_id.clear();
    m_displayed_model_palette.clear();
    m_displayed_model_palette_roles.clear();
    clear_model_quality();
    if (m_model_preview != nullptr)
        m_model_preview->clear();
    if (m_preview_book != nullptr)
        m_preview_book->SetSelection(0);
    if (m_model_stats != nullptr)
        m_model_stats->SetLabel(_L("模型生成后将在这里显示"));
    if (m_model_preview_message != nullptr)
        m_model_preview_message->SetLabel(_L("生成完成后可拖动旋转模型，并使用滚轮缩放。"));
    m_style_preview_ready = false;
    m_raw_preview_available = false;
    m_model_reference_available = false;
    m_strict_preview_available = false;
    m_model_views_available = false;
    m_heatmap_available = false;
    m_palette_quality_ok = true;
    m_material_fragmentation_ok = true;
    m_model_input_eligible = true;
    m_model_input_primary_blocker.clear();
    m_meaningful_palette_count = 0;
    m_meaningful_subject_color_count = 0;
    m_generation_progress->SetValue(0);
    update_workflow();
    m_status->SetLabel(_L("空闲"));
    m_prepared_prompt->Clear();
    set_preview_empty(_L("请先输入描述或选择参考图。"));
    if (!m_selected_image_path.empty())
        show_selected_image_preview();
    m_result_summary->SetLabel(_L("尚未生成模型。"));
    if (remove_remote && !old_job.empty() && !keep_assets)
        m_client.remove(old_job, [] {}, [](std::string) {});
    refresh_controls();
}

void ModelGenerationPanel::cleanup_files()
{
    m_preview_path.clear();
    m_preview_output = "preview";
    m_reference_image_path.clear();
    m_raw_preview_path.clear();
    m_artifact_path.clear();
    m_color_intent_path.clear();
    m_color_intent_schema.clear();
    m_color_intent_sha256.clear();
}

void ModelGenerationPanel::load_library_entries()
{
    const boost::filesystem::path root = generated_models_root();
    const boost::filesystem::path downloads = root / "downloads";
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(root, ec)) {
        m_library_entries.clear();
        refresh_library();
        return;
    }

    std::map<std::string, boost::filesystem::path> models;
    for (boost::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        boost::system::error_code entry_ec;
        if (!boost::filesystem::is_directory(it->path(), entry_ec))
            continue;
        const std::string job_id = it->path().filename().string();
        if (job_id == "downloads" || job_id.rfind("attempt-", 0) == 0)
            continue;
        boost::filesystem::path model_path = it->path() / "model.glb";
        if (!is_nonempty_model(model_path)) model_path = it->path() / "model-vertex-color.obj";
        if (boost::filesystem::is_regular_file(model_path, entry_ec) &&
            boost::filesystem::file_size(model_path, entry_ec) > 0 && !entry_ec)
            models.emplace(job_id, model_path);
    }
    ec.clear();
    if (boost::filesystem::is_directory(downloads, ec)) {
        for (boost::filesystem::directory_iterator it(downloads, ec), end; !ec && it != end; it.increment(ec)) {
            const boost::filesystem::path path = it->path();
            boost::system::error_code entry_ec;
            if (!boost::filesystem::is_regular_file(path, entry_ec) || !AI::is_model_artifact(path))
                continue;
            const std::string job_id = download_job_id(path);
            if (job_id.rfind("finish-", 0) == 0 && !read_json(library_metadata_path(job_id)).is_object())
                continue; // Unaccepted finishing previews are never history entries.
            if (!job_id.empty() && boost::filesystem::file_size(path, entry_ec) > 0 && !entry_ec)
                models.emplace(job_id, path);
        }
    }

    // A finishing OBJ stays beside its source so relative materials remain
    // valid. Accepted metadata, not its temporary filename, registers it.
    const auto records = library_metadata_path("placeholder").parent_path();
    ec.clear();
    if (boost::filesystem::is_directory(records, ec)) {
        for (boost::filesystem::directory_iterator it(records, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() != ".json") continue;
            const auto data = read_json(it->path());
            if (!data.is_object() || data.value("source", std::string()) != "local_finishing") continue;
            const auto id = data.value("job_id", std::string());
            const auto path = root / data.value("model_path", std::string());
            if (id.rfind("finish-", 0) == 0 && path_is_inside(root, path) && is_nonempty_model(path))
                models[id] = path;
        }
    }
    std::vector<GeneratedModelEntry> entries;
    entries.reserve(models.size());
    ec.clear();
    for (boost::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const auto job_id = it->path().filename().string();
        if (models.count(job_id)) continue;
        const auto design = read_design_history_entry(root, job_id);
        if (!design) continue;
        GeneratedModelEntry entry;
        entry.design_only = true;
        entry.job_id = job_id;
        entry.generated_at = design->generated_at;
        entry.reference_image_path = design->input_path;
        entry.preview_path = design->preview_path;
        entry.ai_image_path = design->raw_preview_path;
        entry.title = from_u8(design->prompt);
        if (entry.title.empty()) entry.title = _L("AI 设计图 ") + from_u8(job_id.substr(0, 8));
        if (entry.title.length() > 32) entry.title = entry.title.Left(32) + _L("…");
        const wxDateTime generated(entry.generated_at);
        entry.details = generated.IsValid() ? generated.FormatISODate() + " " + generated.FormatISOTime() : _L("未知时间");
        entry.details += design->state == "awaiting_confirmation" ? _L(" · 设计待确认")
            : design->state == "stopped" ? _L(" · 已停止，设计图已保留") : _L(" · 未完成，设计图已保留");
        entry.details += _L("\n素材：");
        entry.details += entry.reference_image_path.empty() ? _L("无参考图") : _L("原图 ✓");
        entry.details += _L(" · AI 图 ✓ · 尚无 3D 模型");
        entries.emplace_back(std::move(entry));
    }
    for (const auto& [job_id, model_path] : models) {
        boost::system::error_code entry_ec;
        GeneratedModelEntry entry;
        entry.job_id = job_id;
        entry.model_path = model_path;
        entry.generated_at = boost::filesystem::last_write_time(model_path, entry_ec);
        if (entry_ec)
            entry.generated_at = 0;

        const nlohmann::json metadata = read_json(library_metadata_path(job_id));
        if (metadata.is_object()) {
            entry.generated_at = metadata.value("generated_at", entry.generated_at);
            entry.imported_at = metadata.value("imported_at", std::time_t {0});
            entry.triangle_count = metadata.value("triangle_count", size_t {0});
            entry.load_seconds = metadata.value("load_seconds", 0.0);
            entry.print_feedback = metadata.value("print_feedback", std::string());
            entry.use_printable_colors = metadata.value("use_printable_colors", false);
            const std::string provider = metadata.value("provider", std::string());
            const std::string provider_task_id = metadata.value("provider_task_id", std::string());
            const std::string provider_conversion_task_id =
                metadata.value("provider_conversion_task_id", std::string());
            if (provider == "tripo" && valid_provider_task_id(provider_task_id)) {
                entry.provider_name = provider;
                entry.provider_task_id = provider_task_id;
                if (valid_provider_task_id(provider_conversion_task_id))
                    entry.provider_conversion_task_id = provider_conversion_task_id;
            }
            std::string prompt = metadata.value("prompt", std::string());
            if (prompt == INTERNAL_DEFAULT_IMAGE_INSTRUCTION)
                prompt.clear();
            if (!prompt.empty()) {
                entry.title = wxString::FromUTF8(prompt);
                if (entry.title.length() > 32)
                    entry.title = entry.title.Left(32) + _L("…");
            }
            const auto palette = metadata.find("palette");
            if (palette != metadata.end() && palette->is_array()) {
                for (const auto& color : *palette) {
                    if (color.is_string())
                        entry.palette.push_back(color.get<std::string>());
                }
            }
            const auto roles = metadata.find("palette_roles");
            if (roles != metadata.end() && roles->is_object()) {
                for (const char* role : PALETTE_ROLE_IDS) {
                    const auto color = roles->find(role);
                    if (color != roles->end() && color->is_string())
                        entry.palette_roles.emplace(role, color->get<std::string>());
                }
            }
            entry.reference_image_path = library_image_path(metadata, "reference_image_path", root);
            entry.ai_image_path = library_image_path(metadata, "ai_image_path", root);
            const auto color_intent_path = metadata.find("color_intent_path");
            if (color_intent_path != metadata.end() && color_intent_path->is_string()) {
                const boost::filesystem::path candidate = root / color_intent_path->get<std::string>();
                if (path_is_inside(root, candidate) && boost::filesystem::is_regular_file(candidate, entry_ec))
                    entry.color_intent_path = candidate;
            }
            const auto color_intent_schema = metadata.find("color_intent_schema");
            const auto color_intent_sha256 = metadata.find("color_intent_sha256");
            if (color_intent_schema != metadata.end() && color_intent_schema->is_string())
                entry.color_intent_schema = color_intent_schema->get<std::string>();
            if (color_intent_sha256 != metadata.end() && color_intent_sha256->is_string())
                entry.color_intent_sha256 = color_intent_sha256->get<std::string>();
        }

        const boost::filesystem::path job_preview = root / job_id / "preview.png";
        const boost::filesystem::path download_preview = temp_path(job_id, "png");
        entry_ec.clear();
        if (boost::filesystem::is_regular_file(job_preview, entry_ec))
            entry.preview_path = job_preview;
        else {
            entry_ec.clear();
            if (boost::filesystem::is_regular_file(download_preview, entry_ec))
                entry.preview_path = download_preview;
        }
        if (entry.reference_image_path.empty()) {
            const boost::filesystem::path legacy_input = temp_path(job_id + "-input", "png");
            if (is_supported_image(legacy_input) && path_is_inside(root, legacy_input))
                entry.reference_image_path = legacy_input;
        }
        if (entry.ai_image_path.empty()) {
            const boost::filesystem::path legacy_raw = temp_path(job_id + "-raw", "png");
            if (is_supported_image(legacy_raw) && path_is_inside(root, legacy_raw))
                entry.ai_image_path = legacy_raw;
            else if (is_supported_image(entry.preview_path) && path_is_inside(root, entry.preview_path))
                entry.ai_image_path = entry.preview_path;
        }

        if (entry.palette.empty()) {
            const nlohmann::json preview_colors = read_json(root / job_id / "preview-colors.json");
            if (preview_colors.is_object() && preview_colors.value("palette_constrained", true)) {
                const auto pixels = preview_colors.find("palette_pixels");
                if (pixels != preview_colors.end() && pixels->is_object()) {
                    for (auto color = pixels->begin(); color != pixels->end(); ++color)
                        entry.palette.push_back(color.key());
                }
            }
            entry.use_printable_colors = !entry.palette.empty();
        }
        for (auto role = entry.palette_roles.begin(); role != entry.palette_roles.end();) {
            const bool matches_palette = std::any_of(
                entry.palette.begin(), entry.palette.end(), [&role](const std::string& color) {
                    return same_palette_color(color, role->second);
                });
            if (!matches_palette)
                role = entry.palette_roles.erase(role);
            else
                ++role;
        }
        if (entry.palette_roles.empty())
            entry.palette_roles = automatic_palette_roles(entry.palette);

        if (entry.title.empty())
            entry.title = _L("AI 模型 ") + wxString::FromUTF8(job_id.substr(0, std::min<size_t>(8, job_id.size())));
        wxDateTime generated(entry.generated_at);
        const wxString date = generated.IsValid()
            ? generated.FormatISODate() + " " + generated.FormatISOTime()
            : _L("未知时间");
        entry_ec.clear();
        const auto model_size = boost::filesystem::file_size(model_path, entry_ec);
        const double megabytes = entry_ec ? 0.0 : double(model_size) / (1024.0 * 1024.0);
        entry.details = date + wxString::Format(_L(" · %.1f MB · "), megabytes) +
            (entry.use_printable_colors
                ? wxString::Format(_L("%llu 种可打印颜色"), static_cast<unsigned long long>(entry.palette.size()))
                : _L("自然颜色"));
        if (entry.triangle_count > 0)
            entry.details += wxString::Format(
                _L(" · %.1f 万面"), static_cast<double>(entry.triangle_count) / 10000.0);
        if (entry.load_seconds > 0.0)
            entry.details += wxString::Format(_L(" · 上次加载 %.2f 秒"), entry.load_seconds);
        entry.details += _L("\n素材：");
        entry.details += entry.reference_image_path.empty() ? _L("原图未保存") : _L("原图 ✓");
        entry.details += _L(" · ");
        entry.details += entry.ai_image_path.empty() ? _L("AI 图未保存") : _L("AI 图 ✓");
        if (!entry.color_intent_path.empty() || !entry.color_intent_schema.empty() ||
            !entry.color_intent_sha256.empty())
            entry.details += AI::is_valid_color_intent_manifest_ref(
                {entry.color_intent_path.string(), entry.color_intent_schema, entry.color_intent_sha256})
                ? _L(" · 颜色意图 ✓") : _L(" · 颜色意图无效");
        if (entry.imported_at > 0) {
            entry.details += _L("\n已导入准备页");
            if (entry.print_feedback == "success")
                entry.details += _L(" · 打印反馈：成功");
            else if (entry.print_feedback == "issue")
                entry.details += _L(" · 打印反馈：有问题");
            else
                entry.details += _L(" · 尚未记录打印结果");
        }

        entries.emplace_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const GeneratedModelEntry& lhs, const GeneratedModelEntry& rhs) {
        if (lhs.generated_at != rhs.generated_at)
            return lhs.generated_at > rhs.generated_at;
        return lhs.job_id < rhs.job_id;
    });
    m_library_entries = std::move(entries);
    refresh_library();
}

void ModelGenerationPanel::save_library_entry(size_t artifact_size, size_t triangle_count, double width,
                                               double depth, double height, size_t color_count,
                                               double load_seconds)
{
    if (m_job_id.empty() || m_displayed_model_path.empty())
        return;
    const boost::filesystem::path metadata_path = library_metadata_path(m_job_id);
    const nlohmann::json previous = read_json(metadata_path);
    std::time_t generated_at = std::time(nullptr);
    // Automatic job recovery also saves this entry. Keep its first recorded generation time.
    if (previous.is_object()) {
        const auto saved_time = previous.find("generated_at");
        if (saved_time != previous.end() && saved_time->is_number_integer()) {
            const auto timestamp = saved_time->get<std::time_t>();
            if (timestamp > 0)
                generated_at = timestamp;
        }
    }
    const boost::filesystem::path root = generated_models_root();
    const boost::filesystem::path reference_image = archive_library_image(
        !m_reference_image_path.empty() ? m_reference_image_path : m_job_image_path,
        m_job_id, "reference");
    const boost::filesystem::path ai_image = archive_library_image(
        !m_raw_preview_path.empty() ? m_raw_preview_path : m_preview_path,
        m_job_id, "ai");
    if (!reference_image.empty())
        m_reference_image_path = reference_image;
    if (!ai_image.empty())
        m_raw_preview_path = ai_image;
    // Archive the presentation separately; raw provider output remains intact.
    const wxImage& display = m_model_reference_image.IsOk() ? m_model_reference_image : m_clean_preview_image;
    if (!ai_image.empty() && display.IsOk()) {
        auto display_path = ai_image; display_path += ".display.png";
        if (display.SaveFile(display_path.wstring(), wxBITMAP_TYPE_PNG)) {
            m_history_display_source = ai_image;
            m_history_display_image = display.Copy();
        }
    }
    nlohmann::json metadata {
        {"schema_version", 6},
        {"job_id", m_job_id},
        {"model_path", m_displayed_model_path.lexically_relative(root).generic_string()},
        {"source", job_uses_image() ? (m_job_prompt.empty() ? "image" : "text_image") : "text"},
        {"style", m_job_style},
        {"custom_style", m_job_custom_style},
        {"generation_profile", m_job_generation_profile},
        {"face_limit", m_job_face_limit},
        {"prompt", std::string(m_job_prompt.ToUTF8().data())},
        {"palette", m_job_palette},
        {"palette_roles", m_job_palette_roles},
        {"use_printable_colors", m_job_use_printable_colors},
        {"generated_at", generated_at},
        {"artifact_size", artifact_size},
        {"triangle_count", triangle_count},
        {"color_count", color_count},
        {"load_seconds", load_seconds},
        {"dimensions", {width, depth, height}}
    };
    // Reloading the generated artifact refreshes its statistics, not the user's
    // import and print history for this same job.
    if (previous.is_object()) {
        for (const char* key : {"imported_at", "print_feedback", "print_feedback_at"}) {
            const auto saved = previous.find(key);
            if (saved != previous.end())
                metadata[key] = *saved;
        }
    }
    if (!m_job_provider_task_id.empty()) {
        metadata["provider"] = m_job_provider_name;
        metadata["provider_task_id"] = m_job_provider_task_id;
        if (!m_job_provider_conversion_task_id.empty())
            metadata["provider_conversion_task_id"] = m_job_provider_conversion_task_id;
    }
    if (!m_preview_path.empty())
        metadata["preview_path"] = m_preview_path.lexically_relative(root).generic_string();
    if (!reference_image.empty())
        metadata["reference_image_path"] = reference_image.lexically_relative(root).generic_string();
    if (!ai_image.empty())
        metadata["ai_image_path"] = ai_image.lexically_relative(root).generic_string();
    if (!m_color_intent_path.empty() && path_is_inside(root, m_color_intent_path)) {
        metadata["color_intent_path"] = m_color_intent_path.lexically_relative(root).generic_string();
        metadata["color_intent_schema"] = m_color_intent_schema;
        metadata["color_intent_sha256"] = m_color_intent_sha256;
    }

    boost::filesystem::ofstream stream(metadata_path);
    if (!stream) {
        BOOST_LOG_TRIVIAL(warning) << "Unable to write generated model library metadata for " << m_job_id;
    } else {
        stream << metadata.dump(2);
        stream.close();
    }
    load_library_entries();
}

void ModelGenerationPanel::load_library_entry(const boost::filesystem::path& model_path,
                                               const boost::filesystem::path& reference_image_path,
                                               const boost::filesystem::path& ai_image_path,
                                               const std::vector<std::string>& palette,
                                               const AIModelGenerationClient::PaletteRoles& palette_roles,
                                               bool use_printable_colors,
                                               const boost::filesystem::path& color_intent_path,
                                               const std::string& color_intent_schema,
                                               const std::string& color_intent_sha256,
                                               const std::string& job_id, const wxString& title)
{
    if (m_busy || m_model_preview == nullptr)
        return;
    if (model_path.empty()) {
        load_design_library_entry(job_id);
        return;
    }
    // Clicking a history entry is an explicit context switch.  Detach the
    // current preview below, but do not delete its remote job or source input.
    // This keeps history loading to one action even when a restored image
    // preview is still waiting for its paid-generation decision.
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(model_path, ec)) {
        m_status->SetLabel(_L("历史模型文件已不存在。"));
        return;
    }

    const wxString previous_model_stats = m_model_stats->GetLabel();
    m_status->SetLabel(_L("正在加载历史模型：") + title);
    m_model_stats->SetLabel(_L("正在解析模型..."));
    load_model_preview_async(model_path, palette,
        [=](size_t triangle_count, Vec3d dimensions, size_t color_count, double load_seconds) {
    // Successful explicit history navigation starts a fresh editing context,
    // including when the user selects the same source file again.
    if (!m_finishing_candidate.empty()) {
        boost::system::error_code ignored;
        if (m_finishing_candidate != model_path) boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
    }
    m_finishing_options.selected_faces.clear();
    m_finishing_before = false;
    m_finishing_undo_path.clear(); m_finishing_redo_path.clear();
    const wxImage reference_image = reference_image_path.empty() ? wxImage() : wxImage(reference_image_path.wstring());
    const wxImage ai_image = ai_image_path.empty() ? wxImage() : wxImage(ai_image_path.wstring());
    m_history_display_image = load_model_image_display_copy(ai_image_path);
    m_history_display_source = ai_image_path;
    nlohmann::json metadata = read_json(library_metadata_path(job_id));
    if (metadata.is_object()) {
        metadata["schema_version"] = std::max(4, metadata.value("schema_version", 0));
        metadata["triangle_count"] = triangle_count;
        metadata["color_count"] = color_count;
        metadata["load_seconds"] = load_seconds;
        metadata["dimensions"] = {dimensions.x(), dimensions.y(), dimensions.z()};
        if (!write_json(library_metadata_path(job_id), metadata))
            BOOST_LOG_TRIVIAL(warning) << "Unable to update model load metrics for " << job_id;
    }

    m_poll_timer.Stop();
    m_client.cancel_current();
    ++m_sequence;
    m_job_id.clear();
    m_job_palette = palette;
    m_job_palette_roles = palette_roles.empty() ? automatic_palette_roles(palette) : palette_roles;
    m_job_use_printable_colors = use_printable_colors;
    m_custom_palette = palette;
    m_palette_roles = m_job_palette_roles;
    m_palette_roles_source = palette;
    m_palette_source->SetSelection(use_printable_colors && !palette.empty() ? 2 : 1);
    m_palette_recommendation_confirmed = !palette.empty();
    m_awaiting_palette_confirmation = false;
    // Replace the visible input atomically after the new model has loaded.
    // Legacy records may have no input context; never borrow it from the last model.
    const auto input_string = [&metadata](const char* key) {
        const auto value = metadata.find(key);
        return value != metadata.end() && value->is_string() ? value->get<std::string>() : std::string();
    };
    std::string saved_prompt = input_string("prompt");
    if (saved_prompt == INTERNAL_DEFAULT_IMAGE_INSTRUCTION || input_string("source") == "local_finishing")
        saved_prompt.clear();
    m_job_prompt = from_u8(saved_prompt);
    m_job_style = input_string("style");
    m_job_custom_style = input_string("custom_style");
    m_prompt->ChangeValue(m_job_prompt);
    m_prepared_prompt->ChangeValue(wxString());
    m_custom_style->ChangeValue(from_u8(m_job_custom_style));
    m_style->SetSelection(m_job_style.empty() ? wxNOT_FOUND : style_selection(m_job_style));
    m_stylized_style->SetSelection(m_job_style.empty() ? wxNOT_FOUND : stylized_style_selection(m_job_style));
    m_style->SetToolTip(m_job_style.empty() ? _L("该历史记录未保存风格；再次生成前请选择风格。") : wxString());
    m_style_user_selected = !m_job_style.empty();
    ++m_style_recommendation_sequence;
    m_style_recommendation_loading = false;
    m_style_recommendation_available = false;
    m_style_recommendation = {};
    m_restoring_input = false;
    m_job_image_path = reference_image.IsOk() ? reference_image_path : boost::filesystem::path();
    m_selected_image_path = m_job_image_path;
    m_selected_image->SetLabel(m_selected_image_path.empty()
        ? _L("该历史记录没有可用参考图") : wxString(m_selected_image_path.filename().wstring()));
    m_job_preview_expected = reference_image.IsOk() || ai_image.IsOk();
    m_reference_image_path = reference_image.IsOk() ? reference_image_path : boost::filesystem::path();
    m_raw_preview_path = ai_image.IsOk() ? ai_image_path : boost::filesystem::path();
    m_reference_image = reference_image.IsOk() ? reference_image.Copy() : wxImage();
    m_raw_preview_image = ai_image.IsOk() ? ai_image.Copy() : wxImage();
    m_model_reference_image = wxImage();
    m_strict_preview_image = wxImage();
    m_model_views_image = wxImage();
    m_clean_preview_image = ai_image.IsOk() ? ai_image.Copy() : wxImage();
    m_heatmap_image = wxImage();
    m_raw_preview_available = ai_image.IsOk();
    m_model_reference_available = false;
    m_strict_preview_available = false;
    m_model_views_available = false;
    m_heatmap_available = false;
    m_style_preview_ready = ai_image.IsOk();
    m_style_preview_placeholder = ai_image.IsOk()
        ? wxString()
        : _L("该历史记录未保存 AI 生成图");
    if (m_preview_stage != nullptr)
        m_preview_stage->SetSelection(0);
    m_artifact_path = model_path;
    m_artifact_format = AI::model_artifact_format(model_path);
    m_artifact_color_encoding = m_artifact_format == "glb" ? "textures_or_vertex_colors" : "vertex_colors";
    m_color_intent_path = color_intent_path;
    m_color_intent_schema = color_intent_schema;
    m_color_intent_sha256 = color_intent_sha256;
    m_busy = false;
    m_awaiting_confirmation = false;
    m_ready = true;
    m_artifact_download_started = true;
    if (m_use_printable_colors != nullptr)
        m_use_printable_colors->SetValue(use_printable_colors);
    m_displayed_model_path = model_path;
    m_displayed_model_job_id = job_id;
    m_displayed_model_palette = palette;
    m_displayed_model_palette_roles = m_job_palette_roles;
    m_model_preview_ready = true;
    if (m_finishing_status != nullptr)
        m_finishing_status->SetLabel(m_artifact_format == "glb"
            ? _L("GLB 原贴图保留在原文件；美颜新版本使用顶点色。")
            : _L("当前历史模型已加载，处理结果将另存为新版本。"));
    show_model_comparison();
    m_library_model_loaded = true;
    clear_model_quality();
    update_progress(100, 4, _L("检查并导入"));
    m_model_stats->SetLabel(wxString::Format(
        _L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm\n%s"),
        static_cast<unsigned long long>(triangle_count), static_cast<unsigned long long>(color_count),
        dimensions.x(), dimensions.y(), dimensions.z(),
        model_load_summary(triangle_count, load_seconds).c_str()));
    m_model_preview_message->SetLabel(_L("历史模型已自动摆正；可同时对照原图和 AI 图后再导入。"));
    m_status->SetLabel(_L("已加载历史模型：") + title);
    m_result_summary->SetLabel(_L("历史模型已加载到结果对照，可继续检查图片并导入准备页。"));
    m_preview_message->SetLabel(reference_image.IsOk() && ai_image.IsOk()
        ? _L("历史素材已恢复：原图与 AI 生成图可同屏对照。")
        : reference_image.IsOk()
            ? _L("已恢复原图；该历史记录未保存 AI 生成图。")
            : ai_image.IsOk()
                ? _L("已恢复 AI 生成图；该任务没有可用原图。")
                : _L("旧历史记录未保存关联图片，3D 模型仍可正常预览。"));
    if (m_preview_book != nullptr)
        m_preview_book->SetSelection(0);
    apply_preview_stage(true);
    m_model_preview->refresh();
    refresh_controls();
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_status(job_id,
        [weak, sequence, job_id](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                if (!status.palette_roles.empty() && status.palette == weak->m_displayed_model_palette) {
                    weak->m_job_palette_roles = status.palette_roles;
                    weak->m_displayed_model_palette_roles = status.palette_roles;
                }
                weak->update_library_provider_tasks(job_id, status);
                weak->apply_model_quality(status.model_quality);
                weak->apply_visual_quality(status.visual_quality);
                weak->apply_model_refinement(status.refinement);
                weak->refresh_controls();
            });
        },
        [weak, sequence, job_id](std::string) {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->refresh_model_quality_card();
                weak->refresh_controls();
            });
        });
    }, [this, previous_model_stats](std::string error) {
        m_model_stats->SetLabel(previous_model_stats);
        m_status->SetLabel(_L("历史模型加载失败，保留当前模型与预览。"));
        m_result_summary->SetLabel(from_u8(error));
        refresh_controls();
    }, library_metadata_path(job_id));
}

void ModelGenerationPanel::update_library_provider_tasks(
    const std::string& job_id, const AIModelGenerationClient::JobStatus& status)
{
    if (job_id.empty() || status.provider_name != "tripo" ||
        !valid_provider_task_id(status.provider_task_id))
        return;
    const boost::filesystem::path metadata_path = library_metadata_path(job_id);
    nlohmann::json metadata = read_json(metadata_path);
    if (!metadata.is_object())
        metadata = nlohmann::json::object();
    const bool unchanged = metadata.value("provider", std::string()) == status.provider_name &&
        metadata.value("provider_task_id", std::string()) == status.provider_task_id &&
        metadata.value("provider_conversion_task_id", std::string()) == status.provider_conversion_task_id;
    if (unchanged)
        return;
    metadata["schema_version"] = std::max(5, metadata.value("schema_version", 0));
    metadata["job_id"] = job_id;
    metadata["provider"] = status.provider_name;
    metadata["provider_task_id"] = status.provider_task_id;
    if (!status.provider_conversion_task_id.empty())
        metadata["provider_conversion_task_id"] = status.provider_conversion_task_id;
    if (!write_json(metadata_path, metadata)) {
        BOOST_LOG_TRIVIAL(warning) << "Unable to backfill provider task id for " << job_id;
        return;
    }
    load_library_entries();
}

void ModelGenerationPanel::update_library_import_status(const std::string& job_id)
{
    if (job_id.empty())
        return;
    const boost::filesystem::path metadata_path = library_metadata_path(job_id);
    nlohmann::json metadata = read_json(metadata_path);
    if (!metadata.is_object())
        metadata = nlohmann::json::object();
    const std::time_t now = std::time(nullptr);
    metadata["schema_version"] = std::max(4, metadata.value("schema_version", 0));
    metadata["job_id"] = job_id;
    metadata["imported_at"] = now;
    metadata.erase("auto_slice_requested");
    metadata.erase("slice_requested_at");
    if (!write_json(metadata_path, metadata))
        BOOST_LOG_TRIVIAL(warning) << "Unable to update generated model import status for " << job_id;
}

void ModelGenerationPanel::record_library_print_feedback(const std::string& job_id,
                                                          const std::string& feedback)
{
    if (job_id.empty() || (feedback != "success" && feedback != "issue"))
        return;
    const boost::filesystem::path metadata_path = library_metadata_path(job_id);
    nlohmann::json metadata = read_json(metadata_path);
    if (!metadata.is_object())
        return;
    metadata["schema_version"] = std::max(4, metadata.value("schema_version", 0));
    metadata["print_feedback"] = feedback;
    metadata["print_feedback_at"] = std::time(nullptr);
    if (!write_json(metadata_path, metadata)) {
        m_status->SetLabel(_L("无法保存打印结果，请检查 generated_models 是否可写。"));
        return;
    }
    m_client.record_journey_event(
        feedback == "success" ? "print_feedback_success" : "print_feedback_issue", job_id);
    m_status->SetLabel(feedback == "success"
        ? _L("已记录实际打印结果：成功。")
        : _L("已记录实际打印结果：有问题，建议保留模型用于复盘。"));
    load_library_entries();
}

void ModelGenerationPanel::delete_library_entry(const GeneratedModelEntry& entry)
{
    if (m_busy || m_finishing_running || !m_finishing_candidate.empty())
        return;
    MessageDialog confirm(
        this,
        _L("要删除这个历史资产的本地文件、预览和元数据吗？\n\n此操作不会取消远端任务，也无法撤销。"),
        _L("删除本地资产"), wxYES_NO | wxICON_WARNING);
    if (confirm.ShowModal() != wxID_YES)
        return;

    const boost::filesystem::path root = generated_models_root();
    const auto asset_path = entry.design_only ? entry.preview_path : entry.model_path;
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(root, ec) || !path_is_inside(root, asset_path)) {
        m_status->SetLabel(_L("删除已阻止：模型路径不在 generated_models 中。"));
        return;
    }
    if (entry.design_only && !read_design_history_entry(root, entry.job_id)) {
        m_status->SetLabel(_L("设计记录已变化，请刷新历史后重试。"));
        return;
    }

    bool displayed_model_deleted = false;
    ec.clear();
    if (!m_displayed_model_path.empty())
        displayed_model_deleted = boost::filesystem::equivalent(
            m_displayed_model_path, entry.model_path, ec) && !ec;

    const boost::filesystem::path model_parent = entry.design_only ? root / entry.job_id : asset_path.parent_path();
    const boost::filesystem::path downloads = root / "downloads";
    ec.clear();
    const bool downloaded_model = boost::filesystem::is_directory(downloads, ec) &&
        boost::filesystem::equivalent(model_parent, downloads, ec) && !ec;
    size_t removed_count = 0;
    const bool shared_directory = std::any_of(m_library_entries.begin(), m_library_entries.end(),
        [&](const GeneratedModelEntry& other) {
            return other.job_id != entry.job_id && (path_is_inside(model_parent, other.model_path) ||
                path_is_inside(model_parent, other.preview_path) || path_is_inside(model_parent, other.reference_image_path) ||
                path_is_inside(model_parent, other.ai_image_path) || path_is_inside(model_parent, other.color_intent_path));
        });
    if (!shared_directory && !downloaded_model && model_parent.filename().string() == entry.job_id &&
        path_is_inside(root, model_parent)) {
        ec.clear();
        removed_count = boost::filesystem::remove_all(model_parent, ec);
    }
    std::vector<boost::filesystem::path> targets {
        entry.model_path,
        entry.color_intent_path,
        library_metadata_path(entry.job_id),
        temp_path(entry.job_id, "png"),
        temp_path(entry.job_id + "-raw", "png"),
        temp_path(entry.job_id + "-strict", "png"),
        temp_path(entry.job_id + "-clean", "png"),
        temp_path(entry.job_id + "-heatmap", "png"),
        temp_path(entry.job_id + "-input", "png")
    };
    if (entry.design_only) {
        targets.push_back(model_parent / "job.json");
        for (const auto& path : {entry.preview_path, entry.ai_image_path, entry.reference_image_path})
            if (path_is_inside(model_parent, path)) targets.push_back(path);
    }
    for (const boost::filesystem::path& image_path : {entry.reference_image_path, entry.ai_image_path}) {
        if (is_archived_library_image(image_path, entry.job_id))
            targets.push_back(image_path);
    }
    for (const boost::filesystem::path& target : targets) {
        if (std::any_of(m_library_entries.begin(), m_library_entries.end(), [&](const GeneratedModelEntry& other) {
            if (other.job_id == entry.job_id) return false;
            for (const auto& shared : {other.model_path, other.preview_path, other.reference_image_path,
                                      other.ai_image_path, other.color_intent_path}) {
                boost::system::error_code shared_ec;
                if (!target.empty() && !shared.empty() &&
                    boost::filesystem::equivalent(target, shared, shared_ec) && !shared_ec) return true;
            }
            return false;
        })) continue;
        boost::system::error_code target_ec;
        if (!boost::filesystem::exists(target, target_ec))
            continue;
        if (!path_is_inside(root, target)) {
            ec = boost::system::errc::make_error_code(boost::system::errc::permission_denied);
            break;
        }
        if (boost::filesystem::remove(target, target_ec))
            ++removed_count;
        if (target_ec) {
            ec = target_ec;
            break;
        }
    }

    if (ec) {
        m_status->SetLabel(_L("本地模型删除不完整，请检查文件权限。"));
        load_library_entries();
        return;
    }
    if (displayed_model_deleted) {
        if (m_model_preview != nullptr)
            m_model_preview->clear();
        m_displayed_model_path.clear();
        m_displayed_model_job_id.clear();
        m_displayed_model_palette.clear();
        m_displayed_model_palette_roles.clear();
        m_artifact_path.clear();
        m_color_intent_path.clear();
        m_color_intent_schema.clear();
        m_color_intent_sha256.clear();
        m_reference_image_path.clear();
        m_raw_preview_path.clear();
        m_reference_image = wxImage();
        m_raw_preview_image = wxImage();
        m_clean_preview_image = wxImage();
        m_style_preview_image = wxImage();
        m_style_preview_ready = false;
        m_model_preview_ready = false;
        m_library_model_loaded = false;
        if (m_job_id.empty())
            m_ready = false;
        m_model_stats->SetLabel(_L("模型尚未加载"));
        m_model_preview_message->SetLabel(_L("已删除当前显示的本地模型。"));
    }
    if (entry.design_only) {
        if (m_job_id == entry.job_id) {
            m_selected_image_path.clear();
            reset(false);
        }
        // Forget the stopped/completed local job as well as its files, so it cannot
        // be offered for recovery until the sidecar is restarted.
        m_client.remove(entry.job_id, [] {}, [](std::string) {});
    }
    m_status->SetLabel(removed_count > 0
        ? _L("历史模型的本地文件已删除，无法撤销。")
        : _L("没有找到可删除的本地文件。"));
    load_library_entries();
    refresh_controls();
}

void ModelGenerationPanel::show_selected_image_preview()
{
    if (m_selected_image_path.empty())
        return;
    wxImage image(m_selected_image_path.wstring());
    if (!image.IsOk())
        return;
    m_reference_image = image;
    m_reference_image_path = m_selected_image_path;
    m_raw_preview_path.clear();
    m_raw_preview_image = wxImage();
    m_model_reference_image = wxImage();
    m_strict_preview_image = wxImage();
    m_model_views_image = wxImage();
    m_clean_preview_image = wxImage();
    m_heatmap_image = wxImage();
    m_style_preview_image = wxImage();
    m_style_preview_bitmap = wxNullBitmap;
    m_preview_zoom_factor = 1.0;
    m_style_preview_placeholder = _L("等待生成 AI 图");
    if (m_preview_stage != nullptr)
        m_preview_stage->SetSelection(0);
    m_preview_kind->SetLabel(_L("结果对照"));
    m_preview_message->SetLabel(
        wxString::Format(_L("原图 %d × %d px  ·  等待生成 AI 图"), image.GetWidth(), image.GetHeight()));
    update_preview_view(true);
}

void ModelGenerationPanel::apply_preview_stage(bool center)
{
    const bool show_views = m_preview_stage != nullptr && m_preview_stage->GetSelection() == 1;
    const wxImage* selected = nullptr;
    if (show_views && m_model_views_image.IsOk()) selected = &m_model_views_image;
    else if (!show_views && m_model_reference_image.IsOk()) selected = &m_model_reference_image;
    else if (!show_views && m_clean_preview_image.IsOk()) selected = &m_clean_preview_image;
    else if (!show_views && m_raw_preview_image.IsOk()) selected = &m_raw_preview_image;
    const bool has_display = !show_views && m_history_display_image.IsOk() &&
        m_history_display_source == m_raw_preview_path;
    if (has_display) selected = &m_history_display_image;
    m_style_preview_image = selected == nullptr ? wxImage() : selected->Copy();
    m_style_preview_bitmap = wxNullBitmap;
    if (selected != nullptr) m_style_preview_placeholder.clear();
    else if (show_views) m_style_preview_placeholder = _L("正在加载模型多视图...");
    else if (m_raw_preview_available) m_style_preview_placeholder = _L("正在加载 AI 设计图...");
    if (m_preview_stage != nullptr) m_preview_stage->Enable(m_model_views_available);
    if (m_preview_stage_hint != nullptr) {
        m_preview_stage_hint->SetLabel(show_views
            ? _L("使用“展开图片”放大多视图，检查各个角度的形体与结构。")
            : m_ready ? _L("对照原图与 AI 设计图查看模型；调整请进入3D美颜，重新设计请选“重新开始”。")
                      : _L("确认主体、姿态与细节。生成 3D 时使用已确认的 AI 设计图。"));
        m_preview_stage_hint->Wrap(FromDIP(760));
    }
    update_preview_view(center);
    if (m_preview_area != nullptr) m_preview_area->Update();
}

void ModelGenerationPanel::update_preview_view(bool center)
{
    if (m_preview_area == nullptr || m_updating_preview)
        return;
    m_updating_preview = true;
    if (!m_reference_image.IsOk() && !m_style_preview_image.IsOk() && !m_model_preview_ready) {
        m_reference_bitmap = wxNullBitmap;
        m_style_preview_bitmap = wxNullBitmap;
        m_reference_preview_pane = wxRect();
        m_style_preview_pane = wxRect();
        m_preview_area->SetVirtualSize(m_preview_area->GetClientSize());
        m_preview_area->Refresh();
        m_updating_preview = false;
        return;
    }

    const wxSize client = m_preview_area->GetClientSize();
    const int padding = FromDIP(16);
    const int gap = FromDIP(16);
    const int label_height = FromDIP(32);
    const wxImage* comparison_image = m_reference_image.IsOk() ? &m_reference_image : nullptr;
    const bool show_views = m_preview_stage != nullptr && m_preview_stage->GetSelection() == 1;
    const bool comparison = !show_views && (m_reference_image.IsOk() || m_style_preview_image.IsOk() || m_model_preview_ready);
    const int base_pane_width = comparison
        ? std::max(1, (client.GetWidth() - 2 * padding - gap) / 2)
        : std::max(1, client.GetWidth() - 2 * padding);
    const int base_image_height = std::max(1, client.GetHeight() - 2 * padding - label_height);

    auto update_bitmap = [&](const wxImage& image, wxBitmap& bitmap) {
        if (!image.IsOk()) {
            bitmap = wxNullBitmap;
            return;
        }
        const double fit_scale = std::min({ 1.0,
            double(base_pane_width) / image.GetWidth(),
            double(base_image_height) / image.GetHeight() });
        double scale = fit_scale * m_preview_zoom_factor;
        scale = std::min(scale, double(MAX_PREVIEW_BITMAP_DIMENSION) / image.GetWidth());
        scale = std::min(scale, double(MAX_PREVIEW_BITMAP_DIMENSION) / image.GetHeight());
        const int width = std::max(1, int(std::lround(image.GetWidth() * scale)));
        const int height = std::max(1, int(std::lround(image.GetHeight() * scale)));
        if (!bitmap.IsOk() || bitmap.GetWidth() != width || bitmap.GetHeight() != height)
            bitmap = wxBitmap(image.Scale(width, height, wxIMAGE_QUALITY_HIGH));
    };

    if (comparison_image != nullptr)
        update_bitmap(*comparison_image, m_reference_bitmap);
    else
        m_reference_bitmap = wxNullBitmap;
    update_bitmap(m_style_preview_image, m_style_preview_bitmap);

    if (comparison) {
        const int reference_width = std::max(base_pane_width,
            m_reference_bitmap.IsOk() ? m_reference_bitmap.GetWidth() : 0);
        const int result_width = std::max(base_pane_width,
            m_style_preview_bitmap.IsOk() ? m_style_preview_bitmap.GetWidth() : 0);
        const int image_height = std::max({ base_image_height,
            m_reference_bitmap.IsOk() ? m_reference_bitmap.GetHeight() : 0,
            m_style_preview_bitmap.IsOk() ? m_style_preview_bitmap.GetHeight() : 0 });
        const int pane_height = label_height + image_height;
        m_reference_preview_pane = wxRect(padding, padding, reference_width, pane_height);
        m_style_preview_pane = wxRect(padding + reference_width + gap, padding, result_width, pane_height);
    } else {
        const wxBitmap& bitmap = m_style_preview_bitmap.IsOk() ? m_style_preview_bitmap : m_reference_bitmap;
        const int pane_width = std::max(base_pane_width, bitmap.IsOk() ? bitmap.GetWidth() : 0);
        const int image_height = std::max(base_image_height, bitmap.IsOk() ? bitmap.GetHeight() : 0);
        if (!show_views && m_reference_image.IsOk()) {
            m_reference_preview_pane = wxRect(padding, padding, pane_width, label_height + image_height);
            m_style_preview_pane = wxRect();
        } else {
            m_reference_preview_pane = wxRect();
            m_style_preview_pane = wxRect(padding, padding, pane_width, label_height + image_height);
        }
    }

    const wxRect content = m_style_preview_pane.IsEmpty() ? m_reference_preview_pane : m_style_preview_pane;
    const int virtual_width = std::max(client.GetWidth(), content.GetRight() + padding + 1);
    const int virtual_height = std::max(client.GetHeight(), content.GetBottom() + padding + 1);
    m_preview_area->SetVirtualSize(virtual_width, virtual_height);
    if (center) {
        int pixels_per_unit_x = 1;
        int pixels_per_unit_y = 1;
        m_preview_area->GetScrollPixelsPerUnit(&pixels_per_unit_x, &pixels_per_unit_y);
        const int scroll_x = std::max(0, virtual_width - client.GetWidth()) / std::max(1, 2 * pixels_per_unit_x);
        const int scroll_y = std::max(0, virtual_height - client.GetHeight()) / std::max(1, 2 * pixels_per_unit_y);
        m_preview_area->Scroll(scroll_x, scroll_y);
    }
    m_preview_zoom->SetLabel(wxString::Format("%d%%", int(std::lround(m_preview_zoom_factor * 100.0))));
    m_preview_area->Refresh();
    m_updating_preview = false;
}

void ModelGenerationPanel::set_preview_zoom(double zoom)
{
    if (!m_reference_image.IsOk() && !m_style_preview_image.IsOk())
        return;
    m_preview_zoom_factor = std::clamp(zoom, MIN_PREVIEW_ZOOM, MAX_PREVIEW_ZOOM);
    update_preview_view(true);
    refresh_controls();
}

void ModelGenerationPanel::update_progress(int value, int step, const wxString& phase)
{
    m_generation_progress->Show();
    m_progress_percent->Show();
    value = std::clamp(value, 0, 100);
    step = std::clamp(step, 1, 4);
    m_generation_progress->SetValue(value);
    m_workflow_phase->SetLabel(phase);
    m_workflow_phase->SetToolTip(wxString::Format(_L("第 %d 步，共 4 步"), step));
    m_progress_percent->SetLabel(wxString::Format("%d%%", value));
    for (size_t index = 0; index < m_step_labels.size(); ++index) {
        if (m_step_labels[index] == nullptr)
            continue;
        const int label_step = int(index) + 1;
        const bool active = label_step == step;
        const bool complete = label_step < step;
        m_step_labels[index]->SetForegroundColour(active || complete ? wxColour(24, 112, 105)
                                                                     : wxColour(132, 143, 145));
        wxFont font = m_step_labels[index]->GetFont();
        font.SetWeight(active ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        m_step_labels[index]->SetFont(font);
    }
}

void ModelGenerationPanel::update_workflow(const AIModelGenerationClient::JobStatus* status)
{
    const bool image_mode = m_job_preview_expected ||
                            (m_job_id.empty() && (!m_prompt->GetValue().empty() || has_image_input()));
    wxString phase = _L("输入");
    wxString guidance = _L("输入文字、图片，或同时使用两者");
    int step = 1;
    int progress = 0;
    if (status != nullptr) {
        progress = display_progress(*status);
        if (status->state == "recommending_palette") {
            phase = _L("推荐打印配色");
            guidance = _L("AI 正在分析主体、风格和适合打印的大色区");
            step = 1;
        } else if (status->state == "awaiting_palette_confirmation") {
            phase = _L("确认目标配色");
            guidance = _L("修改或确认设计目标色，再生成图片预览");
            step = 1;
        } else if (status->state == "preprocessing") {
            phase = image_mode ? _L("生成AI 设计图") : _L("准备提示词");
            guidance = image_mode ? _L("AI 正在生成高质量设计图") : _L("AI 正在整理 3D 提示词");
            step = 2;
        } else if (status->phase == "preparing_multiview") {
            phase = _L("准备写实四视图");
            guidance = _L("正在核对人脸、姿态和材料边界；通过后才会提交付费任务");
            step = 3;
        } else if (status->state == "awaiting_confirmation" && status->phase == "multiview_retry") {
            phase = _L("四视图需重试");
            guidance = _L("当前预览已保留，可直接重试；尚未创建付费 Tripo 任务");
            step = 3;
        } else if (status->state == "awaiting_confirmation") {
            phase = image_mode ? _L("确认AI 设计图") : _L("确认提示词");
            guidance = image_mode ? _L("分别确认形体依据与材质分区，并选择 3D 模型精度") : _L("确认提示词并选择 3D 模型精度");
            step = 2;
        } else if (status->phase == "generating") {
            phase = _L("生成模型");
            guidance = _L("正在生成 3D 模型，可在这里查看进度");
            step = 3;
        } else if (status->phase == "texturing") {
            phase = _L("保留造型并上色");
            guidance = _L("正在保留历史模型的脸和姿态，并应用当前确认颜色");
            step = 3;
        } else if (status->phase == "converting" || status->phase == "downloading_artifact") {
            phase = _L("优化模型");
            guidance = _L("正在优化并下载 3D 模型");
            step = 3;
        } else if (status->phase == "checking_model") {
            phase = _L("修复材料并检查结构");
            guidance = _L("正在清理串色和杂色，并检查拓扑、底座与薄壁");
            step = 4;
        } else if (status->phase == "checking_visual") {
            phase = _L("对照原图检查外观");
            guidance = _L("正在核对人脸相似度、主体完整性和材料归属");
            step = 4;
        } else if (status->state == "ready") {
            phase = _L("检查并导入");
            guidance = _L("检查右侧 3D 模型，然后选择导入方式");
            step = 4;
        } else if (status->state == "stopping") {
            phase = _L("停止生成");
            guidance = _L("正在安全停止当前生成任务");
            step = 3;
        } else if (status->state == "failed") {
            phase = status->progress >= 10 ? _L("生成未完成") : _L("预览未完成");
            guidance = _L("请查看失败原因后重试；已确认的图片不会自动丢失");
            step = status->progress >= 10 ? 3 : 2;
        }
    }
    m_workflow_phase->SetLabel(phase);
    m_workflow_steps->SetLabel(guidance);
    update_progress(progress, step, phase);
}

void ModelGenerationPanel::set_preview_empty(const wxString& message)
{
    m_reference_image_path.clear();
    m_raw_preview_path.clear();
    m_reference_image = wxImage();
    m_raw_preview_image = wxImage();
    m_model_reference_image = wxImage();
    m_strict_preview_image = wxImage();
    m_model_views_image = wxImage();
    m_clean_preview_image = wxImage();
    m_heatmap_image = wxImage();
    m_style_preview_image = wxImage();
    m_reference_bitmap = wxNullBitmap;
    m_style_preview_bitmap = wxNullBitmap;
    m_reference_preview_pane = wxRect();
    m_style_preview_pane = wxRect();
    m_style_preview_placeholder.clear();
    if (m_preview_stage != nullptr)
        m_preview_stage->SetSelection(0);
    m_preview_metrics_available = false;
    m_preview_changed_pixel_ratio = 0.0;
    m_preview_minimum_feature_px = 0;
    if (m_preview_stage_hint != nullptr)
        m_preview_stage_hint->SetLabel(_L("生成后可在这里确认图片效果。"));
    if (m_preview_technical_details != nullptr)
        m_preview_technical_details->SetLabel(
            _L("完成预览后会显示颜色映射和小色块处理数据。"));
    m_preview_zoom_factor = 1.0;
    if (m_preview_kind != nullptr)
        m_preview_kind->SetLabel(_L("暂无预览"));
    if (m_preview_zoom != nullptr)
        m_preview_zoom->SetLabel("100%");
    m_preview_message->SetLabel(message);
    update_preview_view();
}

} // namespace Slic3r::GUI
