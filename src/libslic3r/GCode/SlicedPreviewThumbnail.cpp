#include "SlicedPreviewThumbnail.hpp"
#include "GCodeProcessor.hpp"
#include "Thumbnails.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/libslic3r.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <boost/log/trivial.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/beast/core/detail/base64.hpp>

namespace Slic3r {
namespace SlicedPreviewThumbnails {

static inline void set_pixel(std::vector<unsigned char>& pixels, unsigned int width, unsigned int height,
                           int x, int y, const std::array<unsigned char, 4>& rgba, bool transparent) {
    if (x >= 0 && x < static_cast<int>(width) && y >= 0 && y < static_cast<int>(height)) {
        size_t idx = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4;
        if (idx + 3 < pixels.size()) {
            pixels[idx] = rgba[0];
            pixels[idx + 1] = rgba[1];
            pixels[idx + 2] = rgba[2];
            pixels[idx + 3] = transparent ? rgba[3] : 255;
        }
    }
}

static void draw_point(ThumbnailData& data, unsigned int width, unsigned int height,
                      int cx, int cy, const std::array<unsigned char, 4>& rgba, bool transparent, int radius) {
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx*dx + dy*dy <= radius*radius) {
                set_pixel(data.pixels, width, height, cx + dx, cy + dy, rgba, transparent);
            }
        }
    }
}

static void draw_line_bresenham(ThumbnailData& data, unsigned int width, unsigned int height,
                               int x0, int y0, int x1, int y1,
                               const std::array<unsigned char, 4>& rgba, bool transparent, int radius) {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    
    while (true) {
        draw_point(data, width, height, x0, y0, rgba, transparent, radius);
        
        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void draw_line_with_shading(ThumbnailData& data, unsigned int width, unsigned int height,
                                  int x0, int y0, const std::array<unsigned char, 4>& rgba0,
                                  int x1, int y1, const std::array<unsigned char, 4>& rgba1,
                                  bool transparent, int radius) {
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int steps = std::max(dx, dy);
    if (steps == 0) steps = 1;
    
    // Interpolate color from rgba0 to rgba1 as we move along the line
    for (int i = 0; i <= steps; i++) {
        double t = static_cast<double>(i) / static_cast<double>(steps);
        
        std::array<unsigned char, 4> rgba = {
            static_cast<unsigned char>((rgba0[0] * (1.0 - t) + rgba1[0] * t)),
            static_cast<unsigned char>((rgba0[1] * (1.0 - t) + rgba1[1] * t)),
            static_cast<unsigned char>((rgba0[2] * (1.0 - t) + rgba1[2] * t)),
            rgba0[3]
        };
        draw_point(data, width, height, x0, y0, rgba, transparent, radius);
        
        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        if (e2 >= -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static std::array<unsigned char, 4> hex_to_rgb(const std::string& hex) {
    std::array<unsigned char, 4> rgba = {200, 200, 200, 255};
    if (hex.length() >= 7 && hex[0] == '#') {
        try {
            int r = std::stoi(hex.substr(1, 2), nullptr, 16);
            int g = std::stoi(hex.substr(3, 2), nullptr, 16);
            int b = std::stoi(hex.substr(5, 2), nullptr, 16);
            rgba = {static_cast<unsigned char>(r), static_cast<unsigned char>(g), static_cast<unsigned char>(b), 255};
        } catch (...) {
        }
    }
    return rgba;
}

static std::vector<std::string> build_color_print_colors(
    const std::vector<std::string>& extruder_colors,
    const std::vector<Slic3r::CustomGCode::Item>& custom_gcode_per_print_z)
{
    std::vector<std::string> colors = extruder_colors;
    
    for (const auto& code : custom_gcode_per_print_z) {
        if (code.type == Slic3r::CustomGCode::ColorChange) {
            colors.emplace_back(code.color);
        }
    }
    
    if (colors.empty()) {
        colors = {"#26A69A"};
    }
    
    return colors;
}

static void resize_thumbnail(ThumbnailData& dest, const ThumbnailData& source, unsigned int factor) {
    if (!source.is_valid() || factor <= 1) {
        return;
    }
    
    unsigned int src_w = source.width;
    unsigned int src_h = source.height;
    unsigned int dst_w = dest.width;
    unsigned int dst_h = dest.height;
    
    if (src_w / factor != dst_w || src_h / factor != dst_h) {
        return;
    }
    
    const unsigned char* src_pixels = source.pixels.data();
    unsigned char* dst_pixels = dest.pixels.data();
    
    for (unsigned int dy = 0; dy < dst_h; dy++) {
        for (unsigned int dx = 0; dx < dst_w; dx++) {
            unsigned int src_x = dx * factor;
            unsigned int src_y = dy * factor;
            
            unsigned int r = 0, g = 0, b = 0, a = 0;
            unsigned int count = 0;
            
            for (unsigned int fy = 0; fy < factor; fy++) {
                for (unsigned int fx = 0; fx < factor; fx++) {
                    unsigned int sx = src_x + fx;
                    unsigned int sy = src_y + fy;
                    if (sx < src_w && sy < src_h) {
                        size_t idx = (sy * src_w + sx) * 4;
                        r += src_pixels[idx];
                        g += src_pixels[idx + 1];
                        b += src_pixels[idx + 2];
                        a += src_pixels[idx + 3];
                        count++;
                    }
                }
            }
            
            if (count > 0) {
                size_t dst_idx = (dy * dst_w + dx) * 4;
                dst_pixels[dst_idx] = static_cast<unsigned char>(r / count);
                dst_pixels[dst_idx + 1] = static_cast<unsigned char>(g / count);
                dst_pixels[dst_idx + 2] = static_cast<unsigned char>(b / count);
                dst_pixels[dst_idx + 3] = static_cast<unsigned char>(a / count);
            }
        }
    }
}

ThumbnailsList generate_sliced_preview_thumbnails(
    const ThumbnailsParams& params,
    const GCodeProcessorResult* slice_result)
{
    ThumbnailsList thumbnails;
    
    if (!slice_result || slice_result->moves.empty()) {
        return thumbnails;
    }
    
    for (const Vec2d& size : params.sizes) {
        unsigned int target_w = static_cast<unsigned int>(size.x());
        unsigned int target_h = static_cast<unsigned int>(size.y());
        
        unsigned int render_w = target_w;
        unsigned int render_h = target_h;
        unsigned int downscale_factor = 1;
        
        if (target_w <= 200 && target_h <= 200) {
            unsigned int max_dim = std::max(target_w, target_h);
            downscale_factor = std::max(1u, static_cast<unsigned int>(1000 / max_dim));
            render_w = target_w * downscale_factor;
            render_h = target_h * downscale_factor;
            
            if (render_w > 1000) render_w = 1000;
            if (render_h > 1000) render_h = 1000;
            
            downscale_factor = std::max(1u, std::min(render_w / target_w, render_h / target_h));
            render_w = target_w * downscale_factor;
            render_h = target_h * downscale_factor;
        }
        
        ThumbnailData thumbnail_data;
        thumbnail_data.set(render_w, render_h);
        
        if (params.transparent_background) {
            std::fill(thumbnail_data.pixels.begin(), thumbnail_data.pixels.end(), 0);
        }
        
        render_sliced_preview_thumbnail(thumbnail_data, slice_result, params);
        
        if (downscale_factor > 1 && thumbnail_data.is_valid()) {
            ThumbnailData scaled;
            scaled.set(target_w, target_h);
            resize_thumbnail(scaled, thumbnail_data, downscale_factor);
            thumbnail_data = std::move(scaled);
        }
        
        if (thumbnail_data.is_valid()) {
            thumbnails.push_back(std::move(thumbnail_data));
        }
    }
    
    return thumbnails;
}

void render_sliced_preview_thumbnail(
    ThumbnailData& thumbnail_data,
    const GCodeProcessorResult* slice_result,
    const ThumbnailsParams& params)
{
    if (!slice_result || slice_result->moves.empty() || thumbnail_data.pixels.empty()) {
        return;
    }
    
    unsigned int width = thumbnail_data.width;
    unsigned int height = thumbnail_data.height;
    
    std::vector<std::string> color_print_colors = build_color_print_colors(
        slice_result->extruder_colors,
        slice_result->custom_gcode_per_print_z);
    
    if (color_print_colors.empty()) {
        if (slice_result->filaments_count > 0) {
            color_print_colors.resize(slice_result->filaments_count, "#26A69A");
        } else {
            color_print_colors = {"#26A69A"};
        }
    }
    
    struct BoundingBox3d {
        double min_x, max_x, min_y, max_y, min_z, max_z;
        bool initialized = false;
        void update(double x, double y, double z) {
            if (!initialized) {
                min_x = max_x = x;
                min_y = max_y = y;
                min_z = max_z = z;
                initialized = true;
            } else {
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
                min_z = std::min(min_z, z);
                max_z = std::max(max_z, z);
            }
        }
        double size_x() const { return max_x - min_x; }
        double size_y() const { return max_y - min_y; }
        double size_z() const { return max_z - min_z; }
    };
    
    BoundingBox3d moves_bbox;
    bool has_moves = false;
    bool show_support = params.show_support;
    
    for (const auto& move : slice_result->moves) {
        if (move.type != EMoveType::Extrude) {
            continue;
        }
        
        if (move.extrusion_role == erWipeTower || move.extrusion_role == erCustom) {
            continue;
        }

        if (!show_support && (move.extrusion_role == erSupportMaterial || move.extrusion_role == erSupportMaterialInterface)) {
            continue;
        }

        // Skip internal bridge, internal solid infill and sparse infill
        if (move.extrusion_role == erInternalBridgeInfill ||
            move.extrusion_role == erSolidInfill ||
            move.extrusion_role == erInternalInfill) {
            continue;
        }

        moves_bbox.update(move.position.x(), move.position.y(), move.position.z());
        has_moves = true;
    }
    
    if (!has_moves) {
        return;
    }
    
    double padding_factor = 0.02;
    double pad_x = moves_bbox.size_x() * padding_factor;
    double pad_y = moves_bbox.size_y() * padding_factor;
    double pad_z = moves_bbox.size_z() * padding_factor;
    
    moves_bbox.min_x -= pad_x;
    moves_bbox.max_x += pad_x;
    moves_bbox.min_y -= pad_y;
    moves_bbox.max_y += pad_y;
    moves_bbox.min_z -= pad_z;
    moves_bbox.max_z += pad_z;
    
    // Match 3D camera default orientation (Iso view): 45° zenit, 45° phi
    // Camera at (-X, -Y, +Z) looking toward origin
    const double zenit_deg = 45.0;
    const double phi_deg = 45.0;
    const double zenit_rad = zenit_deg * M_PI / 180.0;
    const double phi_rad = phi_deg * M_PI / 180.0;
    
    // Camera position from set_default_orientation:
    // theta = -45 degrees, phi = 45 degrees
    // camera_pos = (sin(-45)*sin(45), sin(-45)*cos(45), cos(-45)) = (-0.5, -0.5, 0.707)
    const double sin_zenit = std::sin(-zenit_rad);
    Vec3d camera_pos(sin_zenit * std::sin(phi_rad),
                     sin_zenit * std::cos(phi_rad),
                     std::cos(-zenit_rad));
    
    // Camera forward (from camera toward center = negative of camera position direction)
    Vec3d forward = -camera_pos;
    forward.normalize();
    
    // Camera right vector (horizontal, perpendicular to forward and up)
    // In world coordinates, right is along +X when viewed from +X,+Y,+Z
    Vec3d right = forward.cross(Vec3d::UnitZ());
    right.normalize();
    
    // Camera up vector (perpendicular to forward and right)
    Vec3d up = right.cross(forward);
    up.normalize();
    
    // Bounding box corners in world space
    std::vector<Vec3d> vertices = {
        {moves_bbox.min_x, moves_bbox.min_y, moves_bbox.min_z},
        {moves_bbox.max_x, moves_bbox.min_y, moves_bbox.min_z},
        {moves_bbox.max_x, moves_bbox.max_y, moves_bbox.min_z},
        {moves_bbox.min_x, moves_bbox.max_y, moves_bbox.min_z},
        {moves_bbox.min_x, moves_bbox.min_y, moves_bbox.max_z},
        {moves_bbox.max_x, moves_bbox.min_y, moves_bbox.max_z},
        {moves_bbox.max_x, moves_bbox.max_y, moves_bbox.max_z},
        {moves_bbox.min_x, moves_bbox.max_y, moves_bbox.max_z}
    };
    
    double bbox_center_x = (moves_bbox.min_x + moves_bbox.max_x) / 2.0;
    double bbox_center_y = (moves_bbox.min_y + moves_bbox.max_y) / 2.0;
    double bbox_center_z = (moves_bbox.min_z + moves_bbox.max_z) / 2.0;
    Vec3d bb_center(bbox_center_x, bbox_center_y, bbox_center_z);
    
    // Project vertices onto camera plane and find screen-space extents
    double min_x = DBL_MAX, min_y = DBL_MAX, max_x = -DBL_MAX, max_y = -DBL_MAX;
    
    for (const Vec3d& v : vertices) {
        Vec3d pos = v - bb_center;
        Vec3d proj_on_plane = pos - pos.dot(forward) * forward;
        double x_on_plane = proj_on_plane.dot(right);
        double y_on_plane = proj_on_plane.dot(up);
        min_x = std::min(min_x, x_on_plane);
        min_y = std::min(min_y, y_on_plane);
        max_x = std::max(max_x, x_on_plane);
        max_y = std::max(max_y, y_on_plane);
    }
    
    double dx = max_x - min_x;
    double dy = max_y - min_y;
    
    if (dx <= 0.0 || dy <= 0.0) {
        return;
    }
    
    // Use same margin factor as 3D camera (1.025 = 2.5% margin)
    // Adjusted to match 3D model thumbnail scale
    const double margin_factor = 0.973;
    double scale = std::min(width / dx, height / dy) * margin_factor;
    
    double bbox_z_range = moves_bbox.max_z - moves_bbox.min_z;
    
    // Use the same camera vectors for projection to match 3D rendering exactly
    auto to_iso = [&](double x, double y, double z, int& ix, int& iy) {
        Vec3d pos(x - bbox_center_x, y - bbox_center_y, z - bbox_center_z);
        Vec3d proj_on_plane = pos - pos.dot(forward) * forward;
        double x_on_plane = proj_on_plane.dot(right);
        double y_on_plane = proj_on_plane.dot(up);
        
        // Scale and center in viewport
        ix = static_cast<int>(x_on_plane * scale + width / 2.0);
        iy = static_cast<int>(y_on_plane * scale + height / 2.0);
    };

    // Light direction in screen space (from upper-left)
    static const double light_dir_x = -0.7071;
    static const double light_dir_y = 0.7071;
    
    auto compute_direction_shade = [&](int curr_x, int curr_y, int prev_x, int prev_y, double z) -> double {
        double dx = static_cast<double>(curr_x - prev_x);
        double dy = static_cast<double>(curr_y - prev_y);
        
        double len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.5) {
            return 0.7;
        }
        
        double dir_x = dx / len;
        double dir_y = dy / len;
        
        // Normal perpendicular to direction
        double normal_x = -dir_y;
        double normal_y = dir_x;
        
        // Choose normal that faces the light
        double dot = normal_x * light_dir_x + normal_y * light_dir_y;
        if (dot < 0) {
            normal_x = -normal_x;
            normal_y = -normal_y;
            dot = -dot;
        }
        
        // Map dot from [0, 1] to [0.2, 1.0] for higher contrast on ridges
        double normal_shade = 0.2 + 0.8 * dot;
        
        // Z-based shading: darker at bottom, lighter at top
        double z_t = (z - moves_bbox.min_z) / (bbox_z_range > 0 ? bbox_z_range : 1.0);
        z_t = std::max(0.0, std::min(1.0, z_t));
        double z_shade = 0.4 + 0.6 * z_t;
        
        // Combined shading
        double shade = normal_shade * z_shade;
        return std::max(0.15, std::min(1.0, shade));
    };
    
    // Variant that takes pre-computed screen-space direction (for line continuations)
    auto compute_direction_shade_from_direction = [&](int x, int y, int dx, int dy, double z) -> double {
        double len = std::sqrt(static_cast<double>(dx * dx + dy * dy));
        if (len < 0.5) {
            return 0.7;
        }
        
        double dir_x = static_cast<double>(dx) / len;
        double dir_y = static_cast<double>(dy) / len;
        
        // Normal perpendicular to direction
        double normal_x = -dir_y;
        double normal_y = dir_x;
        
        // Choose normal that faces the light
        double dot = normal_x * light_dir_x + normal_y * light_dir_y;
        if (dot < 0) {
            normal_x = -normal_x;
            normal_y = -normal_y;
            dot = -dot;
        }
        
        // Map dot from [0, 1] to [0.2, 1.0] for higher contrast on ridges
        double normal_shade = 0.2 + 0.8 * dot;
        
        // Z-based shading: darker at bottom, lighter at top
        double z_t = (z - moves_bbox.min_z) / (bbox_z_range > 0 ? bbox_z_range : 1.0);
        z_t = std::max(0.0, std::min(1.0, z_t));
        double z_shade = 0.4 + 0.6 * z_t;
        
        // Combined shading
        double shade = normal_shade * z_shade;
        return std::max(0.15, std::min(1.0, shade));
    };
    
    auto compute_hybrid_shade = [&](int px, int py, double z) -> double {
        // Z-based shading: darker at bottom, lighter at top
        double z_t = (z - moves_bbox.min_z) / (bbox_z_range > 0 ? bbox_z_range : 1.0);
        z_t = std::max(0.0, std::min(1.0, z_t));
        double z_shade = 0.4 + 0.6 * z_t;
        
        // Corner-based shading: light from upper-left
        double corner_x = static_cast<double>(px) / static_cast<double>(width);
        double corner_y = static_cast<double>(py) / static_cast<double>(height);
        double pos_x = corner_x - 0.5;
        double pos_y = 0.5 - corner_y;
        double corner_dot = -pos_x + pos_y;
        double corner_shade = 0.25 + 0.75 * (corner_dot + 1.0) * 0.5;
        corner_shade = std::max(0.15, std::min(1.0, corner_shade));
        
        // Hybrid: 70% normal + 30% Z-gradient for depth perception
        // Combined shading
        double shade = corner_shade * z_shade;
        return std::max(0.15, std::min(1.0, shade));
    };
    
    auto shade_color = [&](const std::array<unsigned char, 4>& rgba, double shade) {
        return std::array<unsigned char, 4>{
            static_cast<unsigned char>(std::min(255, int(rgba[0] * shade))),
            static_cast<unsigned char>(std::min(255, int(rgba[1] * shade))),
            static_cast<unsigned char>(std::min(255, int(rgba[2] * shade))),
            rgba[3]
        };
    };
    
    int prev_x = 0, prev_y = 0;
    double prev_z = 0;
    int prev_screen_dx = 0, prev_screen_dy = 0;
    bool has_prev = false;
    bool first_move_processed = false; // Track if we've processed at least one valid move
    unsigned char prev_cp_color_id = 0;
    bool is_transparent = params.transparent_background;
    int shading_mode = params.thumbnail_shading_mode;
    
    for (const auto& move : slice_result->moves) {
        if (move.type != EMoveType::Extrude) {
            has_prev = false;
            continue;
        }
        
        if (move.extrusion_role == erWipeTower || move.extrusion_role == erCustom) {
            has_prev = false;
            continue;
        }
        
        if (!show_support && (move.extrusion_role == erSupportMaterial || move.extrusion_role == erSupportMaterialInterface)) {
            has_prev = false;
            continue;
        }

        // Skip internal bridge, internal solid infill and sparse infill
        if (move.extrusion_role == erInternalBridgeInfill ||
            move.extrusion_role == erSolidInfill ||
            move.extrusion_role == erInternalInfill) {
            has_prev = false;
            continue;
        }
        
        size_t color_idx = static_cast<size_t>(move.cp_color_id) % color_print_colors.size();
        std::array<unsigned char, 4> rgba = hex_to_rgb(color_print_colors[color_idx]);
        
        int curr_x, curr_y;
        double curr_z = move.position.z();
        to_iso(move.position.x(), move.position.y(), curr_z, curr_x, curr_y);
        
        // Compute shading based on mode
        double curr_shade;
        if (shading_mode == 1) {
            // Hybrid mode: corner + Z-gradient
            curr_shade = compute_hybrid_shade(curr_x, curr_y, curr_z);
        } else {
            // Direction mode (default): direction-based normal shading from current move direction
            curr_shade = compute_direction_shade(curr_x, curr_y, prev_x, prev_y, curr_z);
        }
        
        std::array<unsigned char, 4> shaded_rgba = shade_color(rgba, curr_shade);
        
        if (has_prev && move.cp_color_id == prev_cp_color_id) {
            double prev_shade;
            if (shading_mode == 1) {
                prev_shade = compute_hybrid_shade(prev_x, prev_y, prev_z);
            } else {
                // Use stored direction from previous segment instead of zero-length vector
                prev_shade = compute_direction_shade_from_direction(prev_x, prev_y, prev_screen_dx, prev_screen_dy, prev_z);
            }
            std::array<unsigned char, 4> prev_rgba = shade_color(rgba, prev_shade);
            draw_line_with_shading(thumbnail_data, width, height,
                                  prev_x, prev_y, prev_rgba,
                                  curr_x, curr_y, shaded_rgba,
                                  is_transparent, 2);
        } else {
            draw_point(thumbnail_data, width, height, 
                      curr_x, curr_y, shaded_rgba, is_transparent, 3);
        }
        
        // Store direction for next segment's shading (only if we had a previous segment)
        if (has_prev) {
            prev_screen_dx = curr_x - prev_x;
            prev_screen_dy = curr_y - prev_y;
        }
        
        prev_x = curr_x;
        prev_y = curr_y;
        prev_z = curr_z;
        has_prev = true;
        first_move_processed = true;
        prev_cp_color_id = move.cp_color_id;
    }
}

void append_sliced_preview_thumbnails_to_file(
    const std::string& gcode_path,
    const ThumbnailsList& thumbnails,
    const std::vector<std::pair<GCodeThumbnailsFormat, Vec2d>>& thumbnail_defs)
{
    if (thumbnails.empty() || thumbnail_defs.empty()) {
        return;
    }
    
    // Read the existing file content
    std::ifstream infile(gcode_path, std::ios::binary);
    if (!infile) {
        BOOST_LOG_TRIVIAL(error) << "Failed to open " << gcode_path << " for reading";
        return;
    }
    
    std::string file_content((std::istreambuf_iterator<char>(infile)),
                             std::istreambuf_iterator<char>());
    infile.close();
    
    // Build thumbnail block
    std::string thumbnail_block;
    thumbnail_block = "\n; SLICED_PREVIEW_THUMBNAIL_BLOCK_START\n";
    
    for (size_t i = 0; i < std::min(thumbnails.size(), thumbnail_defs.size()); ++i) {
        const ThumbnailData& data = thumbnails[i];
        GCodeThumbnailsFormat format = thumbnail_defs[i].first;
        
        if (!data.is_valid()) {
            continue;
        }
        
        auto compressed = GCodeThumbnails::compress_thumbnail(data, format);
        if (compressed->data && compressed->size > 0) {
            std::string encoded;
            encoded.resize(boost::beast::detail::base64::encoded_size(compressed->size));
            encoded.resize(boost::beast::detail::base64::encode((void*)encoded.data(), compressed->data, compressed->size));
            
            thumbnail_block += "; THUMBNAIL_BLOCK_START\n";
            thumbnail_block += (boost::format(";\n; %s begin %dx%d %d\n") % compressed->tag() % data.width % data.height % encoded.size()).str();
            
            static constexpr size_t max_row_length = 78;
            while (encoded.size() > max_row_length) {
                thumbnail_block += (boost::format("; %s\n") % encoded.substr(0, max_row_length)).str();
                encoded = encoded.substr(max_row_length);
            }
            if (!encoded.empty()) {
                thumbnail_block += (boost::format("; %s\n") % encoded).str();
            }
            thumbnail_block += (boost::format("; %s end\n") % compressed->tag()).str();
            thumbnail_block += "; THUMBNAIL_BLOCK_END\n\n";
            
            BOOST_LOG_TRIVIAL(info) << "Prepared sliced preview thumbnail " << data.width << "x" << data.height;
        }
    }
    
    thumbnail_block += "; SLICED_PREVIEW_THUMBNAIL_BLOCK_END\n\n";
    
    // Find insertion point: after "; HEADER_BLOCK_END" if present, otherwise after first line
    size_t insert_pos = 0;
    size_t header_end = file_content.find("HEADER_BLOCK_END");
    if (header_end != std::string::npos) {
        insert_pos = header_end + strlen("HEADER_BLOCK_END");
        // Find end of that line
        size_t line_end = file_content.find('\n', insert_pos);
        if (line_end != std::string::npos) {
            insert_pos = line_end + 1;
        }
    } else {
        // Insert after first line
        size_t first_newline = file_content.find('\n');
        if (first_newline != std::string::npos) {
            insert_pos = first_newline + 1;
        }
    }
    
    // Insert thumbnails at the found position
    file_content.insert(insert_pos, thumbnail_block);
    
    // Write back to file
    std::ofstream outfile(gcode_path, std::ios::binary | std::ios::trunc);
    if (!outfile) {
        BOOST_LOG_TRIVIAL(error) << "Failed to open " << gcode_path << " for writing";
        return;
    }
    
    outfile << file_content;
    outfile.close();
    
    BOOST_LOG_TRIVIAL(info) << "Inserted sliced preview thumbnails into header of " << gcode_path;
}

} // namespace SlicedPreviewThumbnails

namespace SlicedPreviewThumbnails {

void append_thumbnails_to_file(
    const std::string& gcode_path,
    const ThumbnailsList& thumbnails,
    const std::vector<std::pair<GCodeThumbnailsFormat, Vec2d>>& thumbnail_defs,
    const std::string& block_start_marker,
    const std::string& block_end_marker)
{
    if (thumbnails.empty() || thumbnail_defs.empty()) {
        return;
    }
    
    // Read the existing file content
    std::ifstream infile(gcode_path, std::ios::binary);
    if (!infile) {
        BOOST_LOG_TRIVIAL(error) << "Failed to open " << gcode_path << " for reading";
        return;
    }
    
    std::string file_content((std::istreambuf_iterator<char>(infile)),
                             std::istreambuf_iterator<char>());
    infile.close();
    
    // Build thumbnail block
    std::string thumbnail_block;
    thumbnail_block = "\n" + block_start_marker + "\n";
    
    for (size_t i = 0; i < std::min(thumbnails.size(), thumbnail_defs.size()); ++i) {
        const ThumbnailData& data = thumbnails[i];
        GCodeThumbnailsFormat format = thumbnail_defs[i].first;
        
        if (!data.is_valid()) {
            continue;
        }
        
        auto compressed = GCodeThumbnails::compress_thumbnail(data, format);
        if (compressed->data && compressed->size > 0) {
            std::string encoded;
            encoded.resize(boost::beast::detail::base64::encoded_size(compressed->size));
            encoded.resize(boost::beast::detail::base64::encode((void*)encoded.data(), compressed->data, compressed->size));
            
            thumbnail_block += "; THUMBNAIL_BLOCK_START\n";
            thumbnail_block += (boost::format(";\n; %s begin %dx%d %d\n") % compressed->tag() % data.width % data.height % encoded.size()).str();
            
            static constexpr size_t max_row_length = 78;
            while (encoded.size() > max_row_length) {
                thumbnail_block += (boost::format("; %s\n") % encoded.substr(0, max_row_length)).str();
                encoded = encoded.substr(max_row_length);
            }
            if (!encoded.empty()) {
                thumbnail_block += (boost::format("; %s\n") % encoded).str();
            }
            thumbnail_block += (boost::format("; %s end\n") % compressed->tag()).str();
            thumbnail_block += "; THUMBNAIL_BLOCK_END\n\n";
            
            BOOST_LOG_TRIVIAL(info) << "Prepared thumbnail " << data.width << "x" << data.height << " format " << int(format);
        }
    }
    
    thumbnail_block += block_end_marker + "\n\n";
    
    // Find insertion point: after "; HEADER_BLOCK_END" if present, otherwise after first line
    size_t insert_pos = 0;
    size_t header_end = file_content.find("HEADER_BLOCK_END");
    if (header_end != std::string::npos) {
        insert_pos = header_end + strlen("HEADER_BLOCK_END");
        // Find end of that line
        size_t line_end = file_content.find('\n', insert_pos);
        if (line_end != std::string::npos) {
            insert_pos = line_end + 1;
        }
    } else {
        // Insert after first line
        size_t first_newline = file_content.find('\n');
        if (first_newline != std::string::npos) {
            insert_pos = first_newline + 1;
        }
    }
    
    // Insert thumbnails at the found position
    file_content.insert(insert_pos, thumbnail_block);
    
    // Write back to file
    std::ofstream outfile(gcode_path, std::ios::binary | std::ios::trunc);
    if (!outfile) {
        BOOST_LOG_TRIVIAL(error) << "Failed to open " << gcode_path << " for writing";
        return;
    }
    
    outfile << file_content;
    outfile.close();
    
    BOOST_LOG_TRIVIAL(info) << "Inserted thumbnails into header of " << gcode_path;
}

} // namespace SlicedPreviewThumbnails
} // namespace Slic3r
