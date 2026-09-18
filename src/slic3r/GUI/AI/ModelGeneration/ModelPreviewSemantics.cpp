#include "ModelPreview3D.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/stdpaths.h>

namespace Slic3r::GUI {
void ModelPreview3D::update_semantic_coloring()
{
    if (!m_has_model || !m_semantic_source || !m_color_trial_enabled || !m_color_trial->semantic_optimization()) {
        if (m_semantic_controller) m_semantic_controller->cancel();
        m_color_trial->set_semantic_status(wxEmptyString, false);
        return;
    }
    if (!m_semantic_controller) {
        const auto executable = std::filesystem::path(wxStandardPaths::Get().GetExecutablePath().ToStdWstring());
        m_semantic_controller = std::make_unique<ModelSemanticColoring>(executable.parent_path() / "ai" / "portrait_semantics",
            std::filesystem::u8path(Slic3r::data_dir()) / "cache" / "portrait_semantics");
    }
    if (m_semantic_controller->request(m_semantic_source, m_color_trial->semantic_mapping_palette(), m_color_trial->semantic_palette(),
                                      m_color_trial->semantic_portrait_card(), m_face_color_overrides)) {
        m_semantic_ready = false;
        m_semantic_analysis.reset();
        if (m_context && m_canvas->SetCurrent(*m_context)) m_semantic_model.reset();
        m_automatic_face_colors.clear();
        m_automatic_subface_colors.clear();
        m_color_trial->set_semantic_status(_L("正在本机识别人像区域，可旋转模型或取消……"), true);
        m_semantic_timer.Start(100);
    }
}

void ModelPreview3D::finish_semantic_coloring()
{
    if (!m_semantic_controller) { m_semantic_timer.Stop(); return; }
    if (auto result = m_semantic_controller->poll()) {
        m_semantic_analysis = std::move(result->analysis);
        if (!result->error.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "Local semantic coloring unavailable: " << result->error;
            m_color_trial->set_semantic_status(_L("人像区域优化暂不可用，已沿用原有配色。"), false);
        } else if (result->geometry.is_empty()) {
            m_color_trial->set_semantic_status(_L("未找到足够可靠的人像区域，已沿用原有配色。"), false);
        } else if (m_context && m_canvas->SetCurrent(*m_context)) {
            auto model = std::make_unique<GLModel>();
            model->init_from(std::move(result->geometry));
            m_semantic_model = std::move(model);
            m_automatic_face_colors = std::move(result->automatic);
            m_automatic_subface_colors = std::move(result->automatic_subfaces);
            m_semantic_ready = true;
            m_color_trial->set_semantic_status(_L("已按人像区域优化；不明确的区域沿用原配色，可在局部改色中修正。"), false);
        } else {
            // The CPU result was consumed, but could not be adopted by the
            // preview. Invalidate the request so a later user action can retry;
            // do not keep the timer alive or reuse an older semantic surface.
            m_semantic_controller->cancel();
            m_semantic_ready = false;
            m_semantic_analysis.reset();
            m_automatic_face_colors.clear();
            m_automatic_subface_colors.clear();
            m_color_trial->set_semantic_status(_L("人像区域预览暂不可用，已沿用原有配色。切换试色后可重试。"), false);
        }
        BOOST_LOG_TRIVIAL(info) << "Local semantic coloring completed: ms=" << result->elapsed_ms
            << ", cache_hit=" << result->cache_hit << ", auto_faces=" << m_automatic_face_colors.size()
            << ", auto_subfaces=" << m_automatic_subface_colors.size()
            << ", added_triangles=" << result->subface_added_triangles
            << ", rejected_subfaces=" << result->subface_rejected_candidates
            << ", person=" << result->person_detected;
        m_canvas->Refresh(false);
    }
    if (!m_semantic_controller->busy()) m_semantic_timer.Stop();
    else m_color_trial->set_semantic_status(wxString::Format(_L("正在本机识别人像区域 · %d%%"), m_semantic_controller->progress()), true);
}
} // namespace Slic3r::GUI
