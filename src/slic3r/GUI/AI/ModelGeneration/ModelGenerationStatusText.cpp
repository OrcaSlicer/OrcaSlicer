#include "ModelGenerationStatusText.hpp"

#include "slic3r/GUI/AIModelGenerationClient.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>

namespace Slic3r::GUI::ModelGenerationStatusText {

wxString localized_service_error(const std::string& error)
{
    wxString message = from_u8(error);
    if (message.Contains("2-million-face target requires detailed geometry"))
        return _L("200 万面需要精细几何。请在 3D 设置中选择精细几何，或改为 100 万面后重试。");
    if (message.Contains("2-million-face option requires Tripo v3.1"))
        return _L("当前 Tripo 模型版本不支持 200 万面，请改为 100 万面，或检查服务的模型版本设置。");
    if (message.Contains("Could not connect to the preprocessing service"))
        return _L("无法连接图片生成服务，请检查网络、代理和服务地址后重试。");
    if (message.Contains("preprocessing service is temporarily unavailable"))
        return _L("图片生成服务暂时不可用，请稍后重试。");
    if (message.Contains("preprocessing service is rate limiting"))
        return _L("图片生成请求过于频繁，请稍后重试。");
    if (message.Contains("preprocessing service rejected the request"))
        return _L("图片生成服务拒绝了请求，请检查图片和提示词后重试。");
    if (message.Contains("rejected the request") || message.Contains("Tripo rejected"))
        return _L("模型服务拒绝了当前图片或提示词。请调整内容后手动重试；程序不会自动创建新的付费任务。");
    if (message.Contains("rate limiting") || message.Contains("rate limited"))
        return _L("模型服务当前请求过多，请稍后手动重试；程序不会自动创建新的付费任务。");
    if (message.Contains("deadline expired") || message.Contains("timed out"))
        return _L("模型服务响应超时。请先确认服务端没有遗留任务，再手动重试以避免重复计费。");
    if (message.Contains("not reachable") || message.Contains("Couldn't connect") ||
        message.Contains("Failed to connect") || message.Contains("Connection refused"))
        return _L("无法连接本地 AI 服务，请确认正式服务已启动后重试。");
    if (message.Contains("large square cutout") || message.Contains("missing body region"))
        return _L("人物出现方形缺口或身体区域缺失，本次未提交付费 3D 任务；请点击“换一张”重新生成。");
    return _L("操作失败：") + message;
}

wxString localized_job_status(const AIModelGenerationClient::JobStatus& status)
{
    if (status.state == "recommending_palette")
        return wxString::Format(
            _L("AI 正在分析主体并推荐 %llu 种目标色..."),
            static_cast<unsigned long long>(status.palette_color_count));
    if (status.state == "awaiting_palette_confirmation")
        return _L("AI 配色已准备好，请修改或确认后生成图片预览。");
    if (status.state == "preprocessing" && status.phase == "image_generation")
        return _L("图片服务正在生成写实预览，通常需要 1–3 分钟，请保持页面打开...");
    if (status.state == "preprocessing" && status.phase == "checking_image")
        return _L("图片已生成，正在检查人脸、构图和 3D 输入清晰度...");
    if (status.state == "preprocessing" && status.phase == "printability_check")
        return _L("正在分离肤色、衣服、点缀色和轮廓色，并清理杂色...");
    if (status.state == "preprocessing")
        return _L("AI 正在准备提示词和图片预览...");
    if (status.state == "awaiting_confirmation" && status.phase == "multiview_retry")
        return _L("四视图准备未通过，本次尚未创建付费 3D 生成任务；当前图片已保留，可直接重试。");
    if (status.state == "awaiting_confirmation" && status.palette_quality_ok && status.model_input_eligible &&
        std::find(status.model_input_warnings.begin(), status.model_input_warnings.end(),
                  "reference_visual_review_unavailable") != status.model_input_warnings.end())
        return _L("图片已生成；AI 视觉复核暂不可用。本地输入检查已通过，请对照原图确认后继续。");
    if (status.state == "awaiting_confirmation")
        return status.palette_quality_ok && status.model_input_eligible
            ? _L("预览已准备完成，请确认后继续生成 3D 模型。")
            : _L("预览未通过 3D 输入检查，请按提示重新生成。");
    if (status.state == "queued")
        return _L("3D 生成任务已排队...");
    if (status.state == "running" && status.phase == "preparing_multiview")
        return _L("正在生成并核对写实人像四视图，尚未提交付费 3D 任务...");
    if (status.state == "running" && status.phase == "generating")
        return status.progress >= 69
            ? _L("远端正在收尾并封装高精度几何，可能停留数分钟；任务已保存，重启后会继续恢复...")
            : _L("AI 正在生成 3D 模型...");
    if (status.state == "running" && status.phase == "texturing")
        return status.progress >= 69
            ? _L("远端正在烘焙最终写实纹理，可能停留数分钟；请勿重复提交...")
            : _L("正在保留历史造型并重新生成写实颜色...");
    if (status.state == "running" && status.phase == "converting")
        return status.progress >= 94
            ? _L("远端 OBJ 转换正在封装最后文件，完成后会自动下载并检查...")
            : _L("正在转换高精度彩色 OBJ...");
    if (status.state == "running" && status.phase == "downloading_artifact")
        return _L("正在整理模型文件...");
    if (status.state == "running" && status.phase == "checking_model") {
        if (status.progress <= 96) return _L("正在下载并把纹理烘焙为模型顶点色...");
        if (status.progress == 97) return _L("正在生成四视图并分离肤色、衣物、头发和底座...");
        if (status.progress == 98) return _L("正在清理杂色小块并平滑材料边界...");
        return _L("正在检查拓扑、底座、薄壁和高面数模型；高质量档可能需要几分钟...");
    }
    if (status.state == "running" && status.phase == "checking_visual")
        return _L("正在生成五视图，对照原图检查人脸相似度和材料串色...");
    if (status.state == "ready")
        return status.visual_quality.available && !status.visual_quality.import_recommended
            ? _L("模型已完成，但人脸相似度或材料串色检查未通过；请先查看风险项。")
            : _L("3D 模型生成与质量检查完成。");
    if (status.state == "stopping")
        return _L("正在停止生成任务...");
    if (status.state == "stopped")
        return _L("生成已停止，可使用当前配色重新生成图片预览。");
    if (status.state == "cancelled")
        return _L("生成任务已取消。");
    if (status.state == "failed") {
        wxString message;
        if (status.provider_error_code == "image_rate_limited")
            message = _L("图片服务请求较多，请稍后点击“重新生成图片预览”。不会自动重复调用。");
        else if (status.provider_error_code == "image_auth_failed")
            message = _L("图片服务认证或模型权限异常，请检查服务配置后重试。");
        else if (status.provider_error_code == "image_rejected")
            message = _L("图片服务拒绝了请求，请检查图片格式、描述和模型参数；详细原因见诊断日志。");
        else if (status.provider_error_code == "image_download_failed")
            message = _L("服务已返回图片，但结果下载失败。请检查网络并保留诊断 ID；重新生图会再次消耗额度。");
        else if (status.provider_error_code == "image_service_unavailable")
            message = _L("图片服务暂时不可用，本次结果可能不明确。程序不会自动重试，请稍后手动重新生成。");
        else if (status.provider_error_code == "image_connection_failed")
            message = _L("连接图片服务失败，本次结果可能不明确。程序不会自动重试，请检查网络后手动重新生成。");
        else if (status.provider_error_code == "local_image_processing_failed")
            message = _L("图片已经生成，但本地可打印色检查失败。请点击“使用当前配色生成预览”重试；若再次失败请反馈任务编号。");
        else if (status.provider_error_ambiguous)
            message = _L("模型服务提交结果不明确，远端可能已创建付费任务。为避免重复计费，程序不会自动重试；请先到服务端确认任务状态。");
        else if (status.provider_error_code == "provider_rejected")
            message = _L("模型服务拒绝了当前图片或提示词。请调整受限内容后手动重试；程序不会自动创建新的付费任务。");
        else if (status.provider_error_code == "provider_rate_limited")
            message = _L("模型服务当前请求过多，请稍后手动重试；程序不会自动创建新的付费任务。");
        else if (status.provider_error_code == "provider_timeout")
            message = _L("模型服务响应超时。请确认服务端没有遗留任务后再手动重试，避免重复计费。");
        else if (status.provider_error_code == "provider_unavailable")
            message = _L("模型服务暂时不可用，请稍后手动重试；程序不会自动创建新的付费任务。");
        else
            message = status.message.empty() ? _L("生成任务失败。") : localized_service_error(status.message);
        if (!status.id.empty())
            message += "\n" + _L("诊断 ID：") + from_u8(status.id);
        return message;
    }
    return status.message.empty() ? _L("正在处理...") : from_u8(status.message);
}

wxString model_input_quality_label(const std::string& code)
{
    if (code == "subject_not_detected") return _L("未识别到清晰主体，请换一张主体明确的图片或重新生成。");
    if (code == "subject_too_small") return _L("主体太小，请放大主体后重新生成图片预览。");
    if (code == "subject_or_background_fills_frame") return _L("主体或背景铺满画面，请保留清晰留白后重新生成。");
    if (code == "subject_cropped") return _L("主体贴边且可能被裁切，请让完整轮廓出现在画面内。");
    if (code == "fragmented_subject") return _L("图片包含多个断开的主体或碎片，请重新生成一个连贯主体。");
    if (code == "excessive_semitransparency") return _L("主体半透明区域过多，不适合作为 3D 输入，请重新生成。");
    if (code == "background_not_isolated") return _L("背景过于复杂，请使用透明或纯色背景后重新生成。");
    if (code == "subject_has_rectangular_cutout") return _L("人物出现方形缺口或身体区域缺失，本次未提交付费 3D 任务；请点击“换一张”。");
    if (code == "portrait_shoulder_silhouette_unverified") return _L("肩颈轮廓仍有缺口或背景残影，本次未提交付费 3D 任务；请点击“换一张”。");
    if (code == "preview_identity_mismatch") return _L("生成图的人脸与原图差异较大，已阻止进入 3D；请点击“重新生成图片预览”。");
    if (code == "preview_face_geometry_drift") return _L("生成图的脸型或五官位置发生漂移，已阻止进入 3D；请重新生成。");
    if (code == "preview_age_expression_drift") return _L("生成图改变了人物年龄感或表情，已阻止进入 3D；请重新生成。");
    if (code == "preview_material_mixing") return _L("图片中仍有肤色、衣服、头发或底座串色，已阻止进入 3D；请重新生成。");
    if (code == "preview_base_mixing") return _L("底座中混入了其他材质色，已阻止进入 3D；请重新生成。");
    if (code == "preview_pose_clothing_drift") return _L("生成图改变了姿态或服装结构，已阻止进入 3D；请重新生成。");
    if (code == "preview_modeling_reference_unclear") return _L("当前图片不适合作为可靠的 3D 参考，请重新生成。");
    return _L("图片未通过 3D 输入检查，请按预览提示重新生成。");
}

} // namespace Slic3r::GUI::ModelGenerationStatusText
