#include "slic3r/GUI/ModelGenerationPanel.hpp"

#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "libslic3r/Geometry.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"

#include <boost/filesystem.hpp>

#include <wx/notebook.h>
#include <wx/stattext.h>
#include <wx/weakref.h>

#include <chrono>
#include <utility>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

wxWindow* ModelGenerationPanel::build_import_settings(wxWindow* parent)
{
    m_import_settings_panel = new wxPanel(parent);
    m_import_settings_panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(section_label(m_import_settings_panel, _L("导入设置")), 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* color_row = new wxBoxSizer(wxHORIZONTAL);
    color_row->Add(new wxStaticText(m_import_settings_panel, wxID_ANY, _L("颜色处理")),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    m_import_color_mode = new wxChoice(m_import_settings_panel, wxID_ANY);
    m_import_color_mode->Append(_L("完整颜色匹配（支持叠色，推荐）"));
    m_import_color_mode->Append(_L("自动匹配当前耗材"));
    m_import_color_mode->Append(_L("单色导入"));
    m_import_color_mode->Append(_L("简单匹配耗材槽"));
    m_import_color_mode->SetSelection(0);
    m_import_color_mode->SetToolTip(
        _L("默认打开完整颜色匹配窗口，可预览并选择叠色方案后确认导入；自动匹配仅使用当前物理耗材；单色导入忽略模型颜色。"));
    color_row->Add(m_import_color_mode, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(color_row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));

    auto* source_row = new wxBoxSizer(wxHORIZONTAL);
    source_row->Add(new wxStaticText(m_import_settings_panel, wxID_ANY, _L("配色来源")),
                    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    m_import_color_source = new wxChoice(m_import_settings_panel, wxID_ANY);
    m_import_color_source->Append(_L("沿用当前试色（如已开启）"));
    m_import_color_source->Append(_L("从模型原色重新配色"));
    m_import_color_source->SetSelection(0);
    m_import_color_source->SetToolTip(_L("未开启试色时，直接从原色开始。重新配色可在匹配窗口调整目标颜色数量；两种方式都保留已保存的局部改色。目标颜色数量不等于实体耗材数量。"));
    source_row->Add(m_import_color_source, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(source_row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    m_import_settings_panel->SetSizer(sizer);
    return m_import_settings_panel;
}

void ModelGenerationPanel::load_model_preview_async(const boost::filesystem::path& path,
    const std::vector<std::string>& palette,
    std::function<void(size_t, Vec3d, size_t, double)> loaded,
    std::function<void(std::string)> failed, const boost::filesystem::path& metadata_path)
{
    if (m_shutdown || m_preview_loading) return;
    size_t triangles = 0, colors = 0; Vec3d dimensions;
    // Explicit history navigation restores persisted state, not unsaved cached edits.
    if (metadata_path.empty() && m_model_preview->try_load_cached_model(path, palette, triangles, dimensions, colors)) {
        loaded(triangles, dimensions, colors, 0.0);
        return;
    }
    if (m_preview_worker.joinable()) m_preview_worker.join();
    m_preview_loading = true; m_busy = true;
    refresh_controls();
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    try {
        m_preview_worker = std::thread([weak, path, palette, sequence, loaded, failed, metadata_path] {
            const auto start = std::chrono::steady_clock::now();
            auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
            std::string error;
            try { ModelPreview3D::prepare_model(path, *prepared, error, {}, metadata_path); }
            catch (const std::exception& e) { error = e.what(); }
            wxGetApp().CallAfter([weak, prepared, palette, sequence, start, loaded, failed, error]() mutable {
                if (!weak || weak->m_shutdown) return;
                auto* self = weak.get();
                if (self->m_preview_worker.joinable()) self->m_preview_worker.join();
                self->m_preview_loading = false;
                self->m_busy = false;
                if (sequence != self->m_sequence) { self->refresh_controls(); return; }
                size_t triangles = 0, colors = 0; Vec3d dimensions;
                if (!error.empty() || !self->m_model_preview->load_prepared_model(
                    std::move(*prepared), palette, triangles, dimensions, colors, error)) {
                    failed(error); return;
                }
                loaded(triangles, dimensions, colors,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            });
        });
    } catch (const std::exception& e) {
        m_preview_loading = false; m_busy = false;
        failed(e.what());
    }
}

void ModelGenerationPanel::download_model_preview(uint64_t sequence)
{
    if (!m_ready || m_job_id.empty() || m_shutdown)
        return;
    if ((m_artifact_format != "obj" && m_artifact_format != "glb")) {
        m_artifact_download_started = false;
        m_status->SetLabel(_L("支持预览和导入 OBJ、GLB 模型。"));
        m_result_summary->SetLabel(_L("当前生成结果不是受支持的 OBJ 或 GLB 格式。"));
        refresh_controls();
        return;
    }
    if (m_artifact_format == "obj" && m_artifact_color_encoding != "vertex_colors") {
        m_artifact_download_started = false;
        m_status->SetLabel(_L("生成的模型不包含受支持的顶点颜色。"));
        m_result_summary->SetLabel(_L("缺少颜色信息，无法继续彩色模型流程。"));
        refresh_controls();
        return;
    }

    m_artifact_path = temp_path(m_job_id, m_artifact_format);
    m_color_intent_path.clear();
    m_busy = true;
    update_progress(94, 4, _L("下载模型"));
    m_status->SetLabel(_L("正在下载并校验生成的模型..."));
    m_model_stats->SetLabel(_L("正在加载模型..."));
    m_model_preview_message->SetLabel(_L("下载完成后将在此处显示彩色 3D 预览。"));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_artifact(m_job_id, m_artifact_format, m_artifact_path,
        [weak, sequence](boost::filesystem::path path) mutable {
            if (!weak)
                return;
            wxGetApp().CallAfter([weak, sequence, path = std::move(path)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                weak->m_artifact_path = path;
                if (weak->m_color_intent_schema.empty()) {
                    weak->finish_model_preview_download(path, sequence);
                    return;
                }
                weak->m_status->SetLabel(_L("正在校验模型颜色意图与模型的绑定..."));
                weak->m_color_intent_path = temp_path(weak->m_job_id + "-color-intent", "json");
                weak->m_client.download_color_intent(
                    weak->m_job_id, weak->m_color_intent_schema, weak->m_color_intent_sha256,
                    path, weak->m_color_intent_path,
                    [weak, sequence, path](boost::filesystem::path manifest_path) mutable {
                        if (!weak)
                            return;
                        wxGetApp().CallAfter([weak, sequence, path = std::move(path),
                                              manifest_path = std::move(manifest_path)]() mutable {
                            if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                                return;
                            weak->m_color_intent_path = std::move(manifest_path);
                            weak->finish_model_preview_download(path, sequence);
                        });
                    },
                    [weak, sequence, path](std::string error) mutable {
                        if (!weak)
                            return;
                        wxGetApp().CallAfter([weak, sequence, path, error = std::move(error)]() {
                            if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                                return;
                            weak->m_color_intent_path.clear();
                            weak->m_color_intent_schema.clear(); weak->m_color_intent_sha256.clear();
                            weak->m_result_summary->SetLabel(_L("颜色清单不可用，将使用模型自身颜色：") + from_u8(error));
                            weak->finish_model_preview_download(path, sequence);
                        });
                    });
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak)
                return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                weak->m_busy = false;
                weak->m_artifact_download_started = false;
                weak->m_model_preview_ready = false;
                weak->m_status->SetLabel(_L("模型下载失败，请重试。"));
                weak->m_result_summary->SetLabel(_L("下载错误：") + from_u8(error));
                weak->m_model_stats->SetLabel(_L("模型尚未下载"));
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::finish_model_preview_download(const boost::filesystem::path& path, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    if (!m_color_intent_schema.empty() &&
        !AIModelGenerationClient::validate_color_intent_manifest_file(
            m_color_intent_path, m_color_intent_schema, m_color_intent_sha256, path)) {
        m_color_intent_path.clear();
        m_color_intent_schema.clear(); m_color_intent_sha256.clear();
        m_result_summary->SetLabel(_L("颜色清单不匹配，将使用模型自身颜色继续。"));
    }

    load_model_preview_async(path, m_job_palette,
        [this, path](size_t triangle_count, Vec3d dimensions, size_t color_count, double load_seconds) {
    m_artifact_path = path;
    m_displayed_model_path = path;
    m_displayed_model_job_id = m_job_id;
    m_displayed_model_palette = m_job_palette;
    m_displayed_model_palette_roles = m_job_palette_roles;
    m_busy = false;
    m_model_preview_ready = true;
    show_model_comparison();
    m_library_model_loaded = false;
    update_progress(100, 4, _L("检查并导入"));
    const bool visual_gate_blocked = m_visual_quality.available && !m_visual_quality.import_recommended;
    m_status->SetLabel(visual_gate_blocked
        ? _L("模型已生成，但人脸相似度或材料归属未通过；建议重新优化。")
        : _L("3D 模型已生成，请确认外观后再导入准备页。"));
    m_model_stats->SetLabel(wxString::Format(
        _L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm\n%s"),
        static_cast<unsigned long long>(triangle_count), static_cast<unsigned long long>(color_count),
        dimensions.x(), dimensions.y(), dimensions.z(), model_load_summary(triangle_count, load_seconds).c_str()));
    m_model_preview_message->SetLabel(
        _L("拖动模型旋转，滚轮缩放；点击“完整显示模型”恢复全貌。上方缩放按钮用于图片。"));
    m_result_summary->SetLabel(visual_gate_blocked
        ? _L("模型已可用。外观检查仅作提示，可继续导入或进行本地美颜。")
        : m_color_intent_path.empty()
            ? _L("模型已下载并通过解析，可继续导入准备页。")
            : _L("模型与颜色意图已校验，可继续导入准备页。"));
    const size_t artifact_size = boost::filesystem::file_size(path);
    save_library_entry(artifact_size, triangle_count, dimensions.x(), dimensions.y(),
                       dimensions.z(), color_count, load_seconds);
    if (m_preview_book != nullptr)
        m_preview_book->SetSelection(0);
    wxWeakRef<ModelGenerationPanel> weak(this);
    wxGetApp().CallAfter([weak]() {
        if (weak && weak->m_model_preview != nullptr)
            weak->m_model_preview->refresh();
    });
    refresh_controls();
    }, [this](std::string error) {
        m_busy = false;
        m_artifact_download_started = false;
        m_model_preview_ready = false;
        m_status->SetLabel(_L("模型解析失败，已保留本地文件。"));
        m_result_summary->SetLabel(_L("无法显示 3D 预览：") + from_u8(error));
        m_model_stats->SetLabel(_L("模型预览不可用"));
        m_model_preview_message->SetLabel(_L("请重试下载，或检查 generated_models/downloads 中的模型文件。"));
        refresh_controls();
    });
}

} // namespace Slic3r::GUI
