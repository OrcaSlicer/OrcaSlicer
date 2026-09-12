#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "ModelImageDisplayCopy.hpp"
#include "ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <algorithm>
#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/image.h>
#include <wx/notebook.h>
#include <wx/log.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {

void ModelGenerationPanel::load_design_library_entry(const std::string& job_id)
{
    if (m_busy || m_design_history_loading || m_finishing_running || m_shutdown) return;
    if (!m_service_available || !ModelGenerationPresentation::read_design_history_entry(
            ModelGenerationPresentation::generated_models_root(), job_id)) {
        m_status->SetLabel(_L("设计记录或本地 AI 服务不可用，当前内容已保留。"));
        return;
    }
    const uint64_t sequence = m_sequence;
    const uint64_t history_sequence = ++m_design_history_sequence;
    m_design_history_loading = true;
    m_busy = true;
    refresh_controls();
    m_status->SetLabel(_L("正在恢复历史设计图..."));
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_status(job_id,
        [weak, sequence, history_sequence, job_id](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, history_sequence, job_id, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    history_sequence != weak->m_design_history_sequence) return;
                weak->m_design_history_loading = false;
                weak->m_busy = false;
                if (status.id != job_id ||
                    (status.state != "awaiting_confirmation" && status.state != "stopped" && status.state != "failed") ||
                    (!status.preview_ready && !status.raw_preview_ready && !status.model_reference_ready) ||
                    (status.state == "awaiting_confirmation" && status.source == "image" && !status.input_ready)) {
                    weak->refresh_controls();
                    weak->m_status->SetLabel(_L("设计记录不完整或状态已变化，当前内容已保留。"));
                    return;
                }
                // Reopening the current pending design after reconnect is a
                // read refresh: keep local input and unsubmitted quality choices.
                if (status.id == weak->m_job_id && weak->m_awaiting_confirmation &&
                    status.state == "awaiting_confirmation" && !weak->m_model_preview_ready && weak->m_style_preview_ready) {
                    weak->handle_status(std::move(status), sequence);
                    if (weak->m_preview_book) weak->m_preview_book->SetSelection(0);
                    return;
                }
                // Commit navigation only after the persisted job was read successfully.
                // GET and restore keep stopped/failed states and never submit generation.
                if (!weak->m_finishing_candidate.empty()) {
                    boost::system::error_code ignored;
                    boost::filesystem::remove(weak->m_finishing_candidate, ignored);
                    weak->m_finishing_candidate.clear();
                }
                weak->m_finishing_options.selected_faces.clear();
                weak->m_finishing_before = false;
                weak->m_finishing_undo_path.clear();
                weak->m_finishing_redo_path.clear();
                weak->m_selected_image_path.clear();
                weak->reset(false);
                ++weak->m_style_recommendation_sequence;
                weak->m_style_recommendation_loading = false;
                weak->m_style_recommendation_available = false;
                weak->m_style_recommendation = {};
                weak->m_history_display_image = wxImage();
                weak->m_history_display_source.clear();
                weak->set_finishing_workbench(false);
                weak->restore_job(std::move(status), weak->m_sequence);
                if (weak->m_preview_book) weak->m_preview_book->SetSelection(0);
                weak->refresh_controls();
            });
        },
        [weak, sequence, history_sequence](std::string error) {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, history_sequence, error = std::move(error)] {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    history_sequence != weak->m_design_history_sequence) return;
                weak->m_design_history_loading = false;
                weak->m_busy = false;
                if (ModelGenerationPresentation::is_transient_sidecar_poll_error(error)) {
                    // A restarted sidecar has a new nonce. Discovery performs a
                    // fresh authenticated challenge; never replay a paid POST.
                    weak->set_service_availability(false, error);
                    weak->m_status->SetLabel(_L("服务连接已失效，当前内容已保留。\n正在重新检测，就绪后请重新打开设计。"));
                    if (weak->m_service_retry_handler) weak->m_service_retry_handler();
                    return;
                }
                weak->refresh_controls();
                weak->m_status->SetLabel(_L("历史设计加载失败，当前模型与输入已保留。"));
            });
        });
}

void ModelGenerationPanel::refresh_library()
{
    if (m_shutdown) return;
    if (m_library_sizer == nullptr || m_library_scroller == nullptr || !m_library_scroller->IsShownOnScreen()) {
        m_library_refresh_pending = true;
        return;
    }
    m_library_sizer->Clear(true);
    m_library_empty->Show(m_library_entries.empty());
    for (const GeneratedModelEntry& entry : m_library_entries) {
        auto* card = new wxPanel(m_library_scroller, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        wxWindow* format = nullptr;
        wxImage thumbnail;
        {
            wxLogNull suppress_missing_thumbnail;
            thumbnail = load_model_image_display_copy(entry.ai_image_path);
            if (!thumbnail.IsOk() && !entry.ai_image_path.empty()) thumbnail.LoadFile(entry.ai_image_path.wstring());
            if (!thumbnail.IsOk() && !entry.reference_image_path.empty()) thumbnail.LoadFile(entry.reference_image_path.wstring());
        }
        if (thumbnail.IsOk()) {
            const double scale = double(FromDIP(96)) / std::max(thumbnail.GetWidth(), thumbnail.GetHeight());
            format = new wxStaticBitmap(card, wxID_ANY, wxBitmap(thumbnail.Scale(
                std::max(1, int(thumbnail.GetWidth() * scale)), std::max(1, int(thumbnail.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH)));
            format->SetBackgroundColour(wxColour(160, 160, 160));
        } else {
            const wxString extension = wxString::FromUTF8(AI::model_artifact_format(entry.model_path)).Upper();
            format = new wxStaticText(card, wxID_ANY, _L("无缩略图") + "\n" + extension);
        }
        row->Add(format, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(12));
        auto* text = new wxBoxSizer(wxVERTICAL);
        auto* title = new wxStaticText(card, wxID_ANY, entry.title);
        wxFont title_font = title->GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(title_font);
        title->Wrap(FromDIP(320));
        text->Add(title, 0, wxBOTTOM, FromDIP(3));
        auto* details = new wxStaticText(card, wxID_ANY, entry.details);
        details->SetForegroundColour(wxColour(91, 104, 107));
        details->Wrap(FromDIP(320));
        text->Add(details, 0);
        if (!entry.provider_task_id.empty()) {
            // Use a normal button: the collapsible pane's custom header can
            // lose its caption when repainted inside the Windows model list.
            auto* diagnostics = new wxPanel(card);
            auto* diagnostics_sizer = new wxBoxSizer(wxVERTICAL);
            auto* toggle_details = new wxButton(diagnostics, wxID_ANY, _L("展开任务详情"));
            diagnostics_sizer->Add(toggle_details, 0, wxALIGN_LEFT);
            auto* task_parent = new wxPanel(diagnostics);
            auto* task_row = new wxBoxSizer(wxVERTICAL);
            auto* task_id = new wxStaticText(
                task_parent, wxID_ANY, _L("任务编号：") + wxString::FromUTF8(entry.provider_task_id));
            task_id->Wrap(FromDIP(260));
            task_id->SetForegroundColour(wxColour(31, 122, 116));
            task_id->SetToolTip(wxString::FromUTF8(entry.provider_task_id));
            auto* copy_task_id = new wxButton(
                task_parent, wxID_ANY, _L("复制"), wxDefaultPosition, wxSize(FromDIP(58), FromDIP(26)));
            copy_task_id->SetToolTip(_L("复制完整的 Tripo 3D Task ID"));
            copy_task_id->Bind(wxEVT_BUTTON, [this, provider_task_id = entry.provider_task_id](wxCommandEvent&) {
                bool copied = false;
                if (wxTheClipboard->Open()) {
                    copied = wxTheClipboard->SetData(
                        new wxTextDataObject(wxString::FromUTF8(provider_task_id)));
                    wxTheClipboard->Close();
                }
                m_status->SetLabel(copied ? _L("3D Task ID 已复制到剪贴板。")
                                          : _L("无法访问剪贴板，请稍后重试。"));
            });
            task_row->Add(task_id, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
            task_row->Add(copy_task_id, 0, wxALIGN_LEFT);
            task_parent->SetSizer(task_row);
            diagnostics_sizer->Add(task_parent, 0, wxEXPAND | wxTOP, FromDIP(8));
            task_parent->Hide();
            diagnostics->SetSizer(diagnostics_sizer);
            text->Add(diagnostics, 0, wxEXPAND | wxTOP, FromDIP(4));
            toggle_details->Bind(wxEVT_BUTTON, [this, card, diagnostics, task_parent, toggle_details](wxCommandEvent&) {
                const bool expanded = !task_parent->IsShown();
                task_parent->Show(expanded);
                toggle_details->SetLabel(expanded ? _L("收起任务详情") : _L("展开任务详情"));
                diagnostics->InvalidateBestSize(); diagnostics->Layout();
                card->InvalidateBestSize();
                card->Layout(); m_library_scroller->Layout(); m_library_scroller->FitInside();
            });
        }
        row->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(8));
        auto* actions = new wxBoxSizer(wxVERTICAL);
        auto* load = new wxButton(card, wxID_ANY, entry.design_only ? _L("打开设计") : _L("加载"),
                                  wxDefaultPosition, wxSize(FromDIP(104), -1));
        load->Bind(wxEVT_BUTTON,
            [this, model_path = entry.model_path, palette = entry.palette,
              palette_roles = entry.palette_roles, use_printable_colors = entry.use_printable_colors,
              reference_image_path = entry.reference_image_path, ai_image_path = entry.ai_image_path,
              color_intent_path = entry.color_intent_path, color_intent_schema = entry.color_intent_schema,
              color_intent_sha256 = entry.color_intent_sha256,
              job_id = entry.job_id, title_text = entry.title](wxCommandEvent&) {
                load_library_entry(model_path, reference_image_path, ai_image_path, palette, palette_roles,
                                   use_printable_colors, color_intent_path, color_intent_schema,
                                   color_intent_sha256, job_id, title_text);
            });
        actions->Add(load, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        auto* reuse_geometry = new wxButton(
            card, wxID_ANY, _L("复用造型"), wxDefaultPosition, wxSize(FromDIP(104), -1));
        reuse_geometry->SetToolTip(
            _L("保留这个历史模型的网格与脸部造型，使用当前确认图片重新生成颜色"));
        reuse_geometry->Enable(
            m_service_available && !m_busy && !m_job_id.empty() && m_job_preview_expected &&
            (m_ready || m_awaiting_confirmation) && entry.job_id != m_job_id);
        reuse_geometry->Bind(wxEVT_BUTTON, [this, job_id = entry.job_id, title_text = entry.title](wxCommandEvent&) {
            on_retexture_from_library(job_id, title_text);
        });
        actions->Add(reuse_geometry, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        reuse_geometry->Show(!entry.design_only);
        auto* remove = new wxButton(card, wxID_ANY, _L("删除本地"), wxDefaultPosition, wxSize(FromDIP(104), -1));
        remove->Bind(wxEVT_BUTTON, [this, entry](wxCommandEvent&) {
            delete_library_entry(entry);
        });
        actions->Add(remove, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        if (entry.imported_at > 0) {
            const wxString feedback_label = entry.print_feedback == "success"
                ? _L("打印成功 ✓")
                : entry.print_feedback == "issue" ? _L("打印有问题") : _L("记录打印结果");
            auto* feedback = new wxButton(
                card, wxID_ANY, feedback_label, wxDefaultPosition, wxSize(FromDIP(104), -1));
            feedback->SetToolTip(_L("由测试人员记录实际打印结果；不会从打印机自动推断"));
            feedback->Bind(wxEVT_BUTTON, [this, job_id = entry.job_id](wxCommandEvent&) {
                MessageDialog dialog(
                    this,
                    _L("请根据已经完成的真实打印记录结果。\n\n“打印成功”表示成品达到本次测试预期；“有问题”表示需要后续复盘。"),
                    _L("记录实际打印结果"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
                dialog.SetButtonLabel(wxID_YES, _L("打印成功"));
                dialog.SetButtonLabel(wxID_NO, _L("有问题"));
                const int result = dialog.ShowModal();
                if (result == wxID_YES)
                    record_library_print_feedback(job_id, "success");
                else if (result == wxID_NO)
                    record_library_print_feedback(job_id, "issue");
            });
            actions->Add(feedback, 0, wxEXPAND);
        }
        row->Add(actions, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(8));
        card->SetSizer(row);
        const auto bind_load = [this, model_path = entry.model_path, palette = entry.palette,
                                 palette_roles = entry.palette_roles,
                                 use_printable_colors = entry.use_printable_colors,
                                 reference_image_path = entry.reference_image_path,
                                 ai_image_path = entry.ai_image_path,
                                 color_intent_path = entry.color_intent_path,
                                 color_intent_schema = entry.color_intent_schema,
                                 color_intent_sha256 = entry.color_intent_sha256,
                                 job_id = entry.job_id,
                                title_text = entry.title](wxWindow* window) {
            window->SetCursor(wxCursor(wxCURSOR_HAND));
            window->SetToolTip(model_path.empty() ? _L("双击打开设计图") : _L("也可双击加载到 3D 模型预览"));
            window->Bind(wxEVT_LEFT_DCLICK, [this, model_path, reference_image_path, ai_image_path,
                                             palette, palette_roles, use_printable_colors, job_id,
                                             color_intent_path, color_intent_schema, color_intent_sha256,
                                             title_text](wxMouseEvent&) {
                load_library_entry(model_path, reference_image_path, ai_image_path, palette, palette_roles,
                                   use_printable_colors, color_intent_path, color_intent_schema,
                                   color_intent_sha256, job_id, title_text);
            });
        };
        bind_load(card);
        bind_load(format);
        format->SetToolTip(entry.design_only ? _L("双击恢复设计图及其输入，不会自动生成 3D。")
                                            : _L("关联设计图缩略图；双击加载实际 3D 模型。"));
        bind_load(title);
        bind_load(details);
        m_library_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }
    m_library_scroller->FitInside();
    m_library_scroller->Layout();
}

} // namespace Slic3r::GUI
