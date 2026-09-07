#include "ModelGenerationPresentation.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>
#include <wx/font.h>
#include <wx/image.h>
#include <wx/stattext.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace Slic3r::GUI::ModelGenerationPresentation {
namespace {

constexpr size_t MAX_IMAGE_SIZE = 20 * 1024 * 1024;
constexpr int MIN_SOURCE_IMAGE_EDGE = 64;
constexpr const char* GENERATED_MODEL_PREFIX = "orcaslicer-ai-";

} // namespace

wxString thin_local_region_metrics(
    const AIModelGenerationClient::ModelQuality::ThinLocalRegion& region,
    bool threshold_available,
    double minimum_wall_thickness_mm)
{
    wxString metrics;
    const auto append = [&metrics](const wxString& item) {
        if (!metrics.empty())
            metrics += _L(" · ");
        metrics += item;
    };
    if (std::isfinite(region.minimum_thickness_mm) && region.minimum_thickness_mm > 0.0) {
        wxString thickness = wxString::Format(_L("最薄 %.3f mm"), region.minimum_thickness_mm);
        if (threshold_available && std::isfinite(minimum_wall_thickness_mm) &&
            minimum_wall_thickness_mm > 0.0) {
            thickness += wxString::Format(_L(" / 建议 ≥ %.3f mm"), minimum_wall_thickness_mm);
        }
        append(thickness);
    }
    if (region.sample_count > 0) {
        append(wxString::Format(_L("%llu 个采样"),
                                static_cast<unsigned long long>(region.sample_count)));
    }
    if (std::isfinite(region.sampled_area_mm2) && region.sampled_area_mm2 > 0.0)
        append(wxString::Format(_L("%.3f mm²"), region.sampled_area_mm2));
    return metrics;
}

wxString thin_local_region_status(
    size_t region_index,
    size_t region_count,
    const AIModelGenerationClient::ModelQuality::ThinLocalRegion& region,
    bool threshold_available,
    double minimum_wall_thickness_mm)
{
    wxString status = wxString::Format(
        _L("第 %llu/%llu 处薄壁"),
        static_cast<unsigned long long>(region_index + 1),
        static_cast<unsigned long long>(region_count));
    const wxString metrics = thin_local_region_metrics(region, threshold_available, minimum_wall_thickness_mm);
    if (!metrics.empty())
        status += _L(" · ") + metrics;
    status += _L("；仅供复核。");
    return status;
}

wxString palette_role_label(const std::string& role)
{
    if (role == "primary") return _L("主体");
    if (role == "structure") return _L("结构");
    if (role == "light") return _L("浅色");
    if (role == "accent") return _L("强调");
    if (role == "secondary") return _L("辅助");
    if (role == "detail") return _L("细节");
    return {};
}

bool is_transient_sidecar_poll_error(const std::string& error)
{
    return error.find("AI sidecar is not reachable") != std::string::npos ||
           error.find("AI sidecar request timed out") != std::string::npos ||
           error.find("AI sidecar request failed") != std::string::npos;
}

std::string new_request_id()
{
    return boost::uuids::to_string(boost::uuids::random_generator()());
}

bool is_supported_image(const boost::filesystem::path& path)
{
    if (!boost::filesystem::is_regular_file(path))
        return false;
    const auto size = boost::filesystem::file_size(path);
    if (size == 0 || size > MAX_IMAGE_SIZE)
        return false;
    boost::filesystem::ifstream stream(path, std::ios::binary);
    unsigned char magic[8] {};
    stream.read(reinterpret_cast<char*>(magic), sizeof(magic));
    const auto count = stream.gcount();
    const bool png = count >= 8 && magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G' &&
                     magic[4] == 0x0d && magic[5] == 0x0a && magic[6] == 0x1a && magic[7] == 0x0a;
    const bool jpeg = count >= 3 && magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff;
    if (!png && !jpeg)
        return false;
    wxImage image(path.wstring());
    return image.IsOk() && image.GetWidth() >= MIN_SOURCE_IMAGE_EDGE &&
           image.GetHeight() >= MIN_SOURCE_IMAGE_EDGE;
}

bool is_nonempty_obj(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(path, ec) || boost::filesystem::file_size(path, ec) == 0)
        return false;
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension == ".obj";
}

boost::filesystem::path generated_models_root()
{
    const char* configured = std::getenv("ORCASLICER_AI_OUTPUT_DIR");
    return configured != nullptr && configured[0] != '\0'
        ? boost::filesystem::path(configured)
        : boost::filesystem::current_path() / "generated_models";
}

boost::filesystem::path temp_path(const std::string& job_id, const std::string& extension)
{
    const boost::filesystem::path root = generated_models_root();
    const boost::filesystem::path downloads = root / "downloads";
    boost::system::error_code ec;
    boost::filesystem::create_directories(downloads, ec);
    return downloads / (std::string(GENERATED_MODEL_PREFIX) + job_id + "." + extension);
}

boost::filesystem::path library_metadata_path(const std::string& job_id)
{
    return temp_path(job_id, "json");
}

std::string download_job_id(const boost::filesystem::path& path)
{
    const std::string stem = path.stem().string();
    if (stem.rfind(GENERATED_MODEL_PREFIX, 0) != 0)
        return {};
    return stem.substr(std::char_traits<char>::length(GENERATED_MODEL_PREFIX));
}

bool valid_provider_task_id(const std::string& value)
{
    return !value.empty() && value.size() <= MAX_PROVIDER_TASK_ID_SIZE &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isalnum(character) || character == '-' || character == '_';
           });
}

nlohmann::json read_json(const boost::filesystem::path& path)
{
    boost::filesystem::ifstream stream(path);
    if (!stream)
        return {};
    return nlohmann::json::parse(stream, nullptr, false);
}

bool write_json(const boost::filesystem::path& path, const nlohmann::json& value)
{
    boost::filesystem::ofstream stream(path);
    if (!stream)
        return false;
    stream << value.dump(2);
    stream.close();
    return stream.good();
}

bool path_is_inside(const boost::filesystem::path& root, const boost::filesystem::path& candidate)
{
    boost::system::error_code root_ec;
    boost::system::error_code candidate_ec;
    const boost::filesystem::path canonical_root = boost::filesystem::canonical(root, root_ec);
    const boost::filesystem::path canonical_candidate = boost::filesystem::canonical(candidate, candidate_ec);
    if (root_ec || candidate_ec)
        return false;

    auto root_part = canonical_root.begin();
    auto candidate_part = canonical_candidate.begin();
    for (; root_part != canonical_root.end() && candidate_part != canonical_candidate.end();
         ++root_part, ++candidate_part) {
        if (*root_part != *candidate_part)
            return false;
    }
    return root_part == canonical_root.end() && candidate_part != canonical_candidate.end();
}

boost::filesystem::path archive_library_image(const boost::filesystem::path& source,
                                              const std::string& job_id,
                                              const std::string& role)
{
    if (source.empty() || job_id.empty() || !is_supported_image(source))
        return {};
    const boost::filesystem::path root = generated_models_root();
    if (path_is_inside(root, source))
        return source;

    std::string extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg")
        extension = ".png";
    const boost::filesystem::path destination = temp_path(job_id + "-" + role, extension.substr(1));
    boost::system::error_code ec;
    boost::filesystem::copy_file(source, destination, boost::filesystem::copy_options::overwrite_existing, ec);
    if (ec || !is_supported_image(destination)) {
        BOOST_LOG_TRIVIAL(warning) << "Unable to archive generated-model " << role
                                   << " image: " << ec.message();
        return {};
    }
    return destination;
}

bool is_archived_library_image(const boost::filesystem::path& path, const std::string& job_id)
{
    const std::string prefix = std::string(GENERATED_MODEL_PREFIX) + job_id + "-";
    return !path.empty() && path.filename().string().rfind(prefix, 0) == 0;
}

boost::filesystem::path library_image_path(const nlohmann::json& metadata,
                                           const char* key,
                                           const boost::filesystem::path& root)
{
    if (!metadata.contains(key) || !metadata[key].is_string())
        return {};
    const boost::filesystem::path candidate = root / metadata[key].get<std::string>();
    return path_is_inside(root, candidate) && is_supported_image(candidate) ? candidate
                                                                            : boost::filesystem::path();
}

wxString model_load_summary(size_t triangle_count, double load_seconds)
{
    wxString summary = wxString::Format(_L("本机实测加载 %.2f 秒"), std::max(0.0, load_seconds));
    if (triangle_count >= 800000)
        summary += _L(" · 超高面数，旋转、选区和切片可能明显变慢");
    else if (triangle_count >= 300000)
        summary += _L(" · 高面数，旋转、选区和切片可能变慢");
    else
        summary += _L(" · 当前复杂度未触发高面数提醒");
    return summary;
}

wxString style_label(const std::string& style)
{
    if (style == "realistic" || style == "enamel_inlay")
        return _L("多色写实");
    if (style == "portrait_sketch")
        return _L("肖像速写");
    if (style == "cartoon" || style == "q_cartoon" || style == "cel_shaded")
        return _L("手办");
    if (style == "sculpture")
        return _L("单色写实");
    if (style == "low_poly")
        return _L("低多边形");
    if (style == "relief")
        return _L("浮雕");
    if (style == "ink_relief")
        return _L("水墨版画浮雕");
    if (style == "diorama")
        return _L("微缩场景");
    if (style == "custom")
        return _L("自定义风格");
    return _L("单色写实");
}

wxString style_recommendation_reason(const std::string& reason)
{
    if (reason == "portrait") return _L("肖像速写用少量体块概括明暗，同时把脸型和五官辨识度放在首位。");
    if (reason == "animal") return _L("宠物适合简化毛发为稳固体块，同时保留轮廓。");
    if (reason == "flat_graphic") return _L("平面图形适合转成留白清楚、线条可建模的水墨版画浮雕。");
    if (reason == "effects") return _L("透明和光影不能直接打印，水墨版画浮雕可把它们收敛为实体层次。");
    if (reason == "architecture") return _L("建筑轮廓清晰，适合保留比例和结构细节。");
    if (reason == "hard_surface" || reason == "structured_subject")
        return _L("硬表面结构清楚，写实微缩更能保留部件。");
    if (reason == "organic") return _L("植物和食物适度简化后，更容易形成稳固体块。");
    if (reason == "scene" || reason == "multiple_subjects")
        return _L("多个主体用微缩场景更容易保留空间关系。");
    if (reason == "limited_reference") return _L("原图立体信息有限，低多边形更稳、更易打印。");
    return _L("主体类别不明确，先用兼容性较好的手办风格。");
}

wxStaticText* section_label(wxWindow* parent, const wxString& text)
{
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    wxFont font = label->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    label->SetFont(font);
    label->SetForegroundColour(wxColour(40, 55, 58));
    return label;
}

wxString model_quality_code_label(const std::string& code)
{
    if (code == "tiny_detached_components") return _L("检测到微小脱离部件，请旋转模型确认是否需要保留。");
    if (code == "unwelded_structural_components") return _L("模型由多个未焊接的主要部件组成，请确认接触处不会在打印后分离。");
    if (code == "floating_disconnected_components") return _L("检测到未接触热床或主体的悬空分离部件，请检查是否可打印。");
    if (code == "thin_structural_components") return _L("检测到整体厚度较薄的连通部件，请检查是否需要加厚。");
    if (code == "thin_local_wall_regions") return _L("检测到附着在主体上的局部薄壁或细连接，请检查是否需要加厚。");
    if (code == "tiny_printable_color_regions") return _L("检测到过小的耗材色块，打印时可能产生碎片化换色。");
    if (code == "too_few_meaningful_target_palette_colors") return _L("最终模型中显著目标色不足，请检查已确认的颜色角色是否在建模后丢失。");
    if (code == "colors_outside_target_palette") return _L("最终模型包含目标调色板之外的颜色，请重新检查颜色量化结果。");
    if (code == "weak_bed_contact") return _L("模型与热床接触面积较小，请检查底座稳定性。");
    if (code == "extreme_aspect_ratio") return _L("模型比例较极端，请检查缩放和摆放方向。");
    if (code == "high_downward_surface_ratio") return _L("向下表面较多，打印时可能需要更多支撑。");
    if (code == "localized_overhang_regions") return _L("检测到局部悬垂面，请旋转模型检查是否需要支撑。");
    if (code == "dense_micro_triangles") return _L("局部三角面非常密集，请检查细小结构。");
    if (code == "repairable_boundary_edges") return _L("存在少量开放边，将在导入时交给 Orca 修复。");
    if (code == "repairable_non_manifold_edges") return _L("存在少量非流形边，将在导入时交给 Orca 修复。");
    if (code == "repairable_inconsistent_winding_edges") return _L("存在少量面绕序异常，将在导入时交给 Orca 修复。");
    if (code == "boundary_edges") return _L("模型包含开放边，当前不能安全导入切片。");
    if (code == "non_manifold_edges") return _L("模型包含非流形边，当前不能安全导入切片。");
    if (code == "inconsistent_winding_edges") return _L("模型面绕序不一致，当前不能安全导入切片。");
    if (code == "degenerate_faces") return _L("模型包含退化三角面，当前不能安全导入切片。");
    if (code == "flat_or_empty_axis") return _L("模型至少一个方向没有有效尺寸。");
    if (code == "too_many_faces") return _L("模型面数超过当前允许上限。");
    if (code == "missing_geometry") return _L("模型没有可用几何数据。");
    return _L("检测到需要复核的模型结构问题：") + from_u8(code);
}

wxString visual_quality_code_label(const std::string& code)
{
    if (code == "visual_subject_incomplete") return _L("主体可能缺失或截断，请对照原图确认。");
    if (code == "visual_semantic_incoherence") return _L("局部形体或部件关系可能不自然。");
    if (code == "visual_base_relationship") return _L("主体与底座的连接关系需要确认。");
    if (code == "visual_detached_artifacts") return _L("多视角中疑似存在意外漂浮物。");
    if (code == "visual_silhouette_unclear") return _L("部分视角轮廓不够清晰。");
    if (code == "visual_color_regions_unclear") return _L("顶点色色块可能过碎或不易辨认。");
    if (code == "visual_identity_mismatch") return _L("主体身份或人脸与原图差异较大，请重点对照五官和脸型。");
    if (code == "visual_material_color_mixing") return _L("肤色、衣物、头发或底座可能存在明显串色。");
    if (code == "visual_review_unavailable") return _L("AI 视觉服务暂不可用，可稍后重试。");
    return _L("检测到需要人工确认的外观问题：") + from_u8(code);
}

} // namespace Slic3r::GUI::ModelGenerationPresentation
