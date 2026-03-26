#ifndef slic3r_SlicedPreviewThumbnail_hpp_
#define slic3r_SlicedPreviewThumbnail_hpp_

#include <vector>
#include <string>
#include "ThumbnailData.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {

struct ThumbnailsParams;
class GCodeProcessorResult;

namespace SlicedPreviewThumbnails {

ThumbnailsList generate_sliced_preview_thumbnails(
    const ThumbnailsParams& params,
    const GCodeProcessorResult* slice_result);

void render_sliced_preview_thumbnail(
    ThumbnailData& thumbnail_data,
    const GCodeProcessorResult* slice_result,
    const ThumbnailsParams& params);

void append_sliced_preview_thumbnails_to_file(
    const std::string& gcode_path,
    const ThumbnailsList& thumbnails,
    const std::vector<std::pair<GCodeThumbnailsFormat, Vec2d>>& thumbnail_defs);

void append_thumbnails_to_file(
    const std::string& gcode_path,
    const ThumbnailsList& thumbnails,
    const std::vector<std::pair<GCodeThumbnailsFormat, Vec2d>>& thumbnail_defs,
    const std::string& block_start_marker,
    const std::string& block_end_marker);

} // namespace SlicedPreviewThumbnails
} // namespace Slic3r

#endif // slic3r_SlicedPreviewThumbnail_hpp_
