#pragma once

#include "slic3r/AI/Contracts/ColorIntent.hpp"
#include "slic3r/GUI/AIModelGenerationClient.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json_fwd.hpp>
#include <wx/defs.h>
#include <wx/string.h>

#include <array>
#include <cstddef>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

class wxStaticText;
class wxWindow;

namespace Slic3r::GUI::ModelGenerationPresentation {

inline constexpr size_t MAX_MODEL_INPUT_BYTES = 2000;
inline constexpr double MIN_PREVIEW_ZOOM = 0.5;
inline constexpr double MAX_PREVIEW_ZOOM = 4.0;
inline constexpr int MAX_PREVIEW_BITMAP_DIMENSION = 4096;
inline constexpr size_t MAX_PROVIDER_TASK_ID_SIZE = 256;
inline constexpr const char* INTERNAL_DEFAULT_IMAGE_INSTRUCTION =
    "Stylize only the content already visible in the reference image. Preserve the exact crop, framing, visible regions, "
    "occlusions, subjects, objects, and background; do not add, remove, reveal, reconstruct, or extend anything.";
inline constexpr const auto& PALETTE_ROLE_IDS = Slic3r::AI::kPaletteRoleIds;

wxString thin_local_region_metrics(
    const AIModelGenerationClient::ModelQuality::ThinLocalRegion& region,
    bool threshold_available,
    double minimum_wall_thickness_mm);
wxString thin_local_region_status(
    size_t region_index,
    size_t region_count,
    const AIModelGenerationClient::ModelQuality::ThinLocalRegion& region,
    bool threshold_available,
    double minimum_wall_thickness_mm);
AIModelGenerationClient::PaletteRoles automatic_palette_roles(const std::vector<std::string>& palette);
void synchronize_palette_roles(const std::vector<std::string>& current_palette,
                               AIModelGenerationClient::PaletteRoles& current_roles,
                               const std::vector<std::string>& submitted_palette,
                               const AIModelGenerationClient::PaletteRoles& submitted_roles,
                               const std::vector<std::string>& returned_palette,
                               const AIModelGenerationClient::PaletteRoles& returned_roles);
bool same_palette_color(const std::string& left, const std::string& right);
wxString palette_role_label(const std::string& role);
double minimum_palette_distance(const std::vector<std::string>& palette);
int remap_progress(int value, int input_start, int input_end, int output_start, int output_end);
int display_progress(const AIModelGenerationClient::JobStatus& status);
bool is_transient_sidecar_poll_error(const std::string& error);
std::string new_request_id();
bool is_supported_image(const boost::filesystem::path& path);
bool is_nonempty_model(const boost::filesystem::path& path);
bool is_nonempty_obj(const boost::filesystem::path& path);
boost::filesystem::path generated_models_root();
boost::filesystem::path temp_path(const std::string& job_id, const std::string& extension);
boost::filesystem::path library_metadata_path(const std::string& job_id);
std::string download_job_id(const boost::filesystem::path& path);
bool valid_provider_task_id(const std::string& value);
nlohmann::json read_json(const boost::filesystem::path& path);
bool write_json(const boost::filesystem::path& path, const nlohmann::json& value);
bool path_is_inside(const boost::filesystem::path& root, const boost::filesystem::path& candidate);
struct DesignHistoryEntry
{
    std::string job_id, state, source, prompt;
    boost::filesystem::path input_path, preview_path, raw_preview_path;
    std::time_t generated_at { 0 };
};
std::optional<DesignHistoryEntry> read_design_history_entry(
    const boost::filesystem::path& root, const std::string& job_id);
bool has_persisted_generation_assets(const boost::filesystem::path& root, const std::string& job_id);
boost::filesystem::path archive_library_image(const boost::filesystem::path& source,
                                              const std::string& job_id,
                                              const std::string& role);
bool is_archived_library_image(const boost::filesystem::path& path, const std::string& job_id);
boost::filesystem::path library_image_path(const nlohmann::json& metadata,
                                           const char* key,
                                           const boost::filesystem::path& root);
wxString model_load_summary(size_t triangle_count, double load_seconds);
wxString style_label(const std::string& style);
int style_selection(const std::string& style);
int stylized_style_selection(const std::string& style);
std::string selected_style(int family, int stylized);
bool style_uses_printable_colors(const std::string& style);
wxString style_recommendation_reason(const std::string& reason);
wxStaticText* section_label(wxWindow* parent, const wxString& text);
wxString model_quality_code_label(const std::string& code);
wxString visual_quality_code_label(const std::string& code);

} // namespace Slic3r::GUI::ModelGenerationPresentation
