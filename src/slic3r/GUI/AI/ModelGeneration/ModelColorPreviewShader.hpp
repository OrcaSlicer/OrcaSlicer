#pragma once

#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Slic3r::GUI {

// RGB8 packs exactly into a 24-bit integer representable by float. Reusing the
// native P3N3T2 layout avoids changing GLModel or quantizing a model to a small
// palette. Colors are decoded per vertex, then interpolated across the face.
inline uint32_t preview_rgb8(float r, float g, float b)
{
    const auto channel = [](float value) { return uint32_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
    return (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

inline bool initialize_model_color_shader(GLShaderProgram& shader)
{
    const bool modern = OpenGLManager::get_gl_info().is_glsl_version_greater_or_equal_to(1, 40);
    const std::string version = modern ? "#version 140\n" : "#version 110\n";
    GLShaderProgram::ShaderSources sources;
    sources[size_t(GLShaderProgram::EShaderType::Vertex)] = version +
        (modern ? "in vec3 v_position; in vec3 v_normal; in vec2 v_tex_coord; out vec4 shaded_color; out vec3 source_rgb; out float light_intensity; out float local_color_lock;\n"
                : "attribute vec3 v_position; attribute vec3 v_normal; attribute vec2 v_tex_coord; varying vec4 shaded_color; varying vec3 source_rgb; varying float light_intensity; varying float local_color_lock;\n") + R"(
uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;
uniform vec4 uniform_color;
uniform bool use_uniform_color;
void main() {
    float encoded_rgb = floor(v_tex_coord.x);
    vec3 rgb = vec3(floor(encoded_rgb / 65536.0), mod(floor(encoded_rgb / 256.0), 256.0), mod(encoded_rgb, 256.0)) / 255.0;
    local_color_lock = (!use_uniform_color && v_tex_coord.y < 0.0) ? 1.0 : 0.0;
    vec4 color = use_uniform_color ? uniform_color : vec4(rgb, abs(v_tex_coord.y));
    vec3 normal = normalize(view_normal_matrix * v_normal);
    float intensity = 0.42 + 0.48 * max(dot(normal, vec3(-0.4574957, 0.4574957, 0.7624929)), 0.0)
        + 0.18 * max(dot(normal, vec3(0.6985074, 0.1397015, 0.6985074)), 0.0);
    shaded_color = vec4(color.rgb * intensity, color.a);
    source_rgb = color.rgb;
    light_intensity = intensity;
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
})";
    sources[size_t(GLShaderProgram::EShaderType::Fragment)] = version +
        (modern ? "in vec4 shaded_color; in vec3 source_rgb; in float light_intensity; in float local_color_lock; out vec4 out_color;\n"
                : "varying vec4 shaded_color; varying vec3 source_rgb; varying float light_intensity; varying float local_color_lock;\n") + R"(
uniform int preview_color_count;
uniform bool preview_lighting;
uniform float preview_lightness_weight;
uniform bool gray_view;
uniform vec3 preview_rgb[6];
uniform vec3 preview_lab[6];
vec3 to_oklab(vec3 rgb) {
    vec3 linear_rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), rgb));
    vec3 lms = vec3(dot(linear_rgb, vec3(0.4122214708,0.5363325363,0.0514459929)),
                    dot(linear_rgb, vec3(0.2119034982,0.6806995451,0.1073969566)),
                    dot(linear_rgb, vec3(0.0883024619,0.2817188376,0.6299787005)));
    lms = pow(max(lms, vec3(0.0)), vec3(1.0/3.0));
    return vec3(dot(lms, vec3(0.2104542553,0.793617785,-0.0040720468)),
                dot(lms, vec3(1.9779984951,-2.428592205,0.4505937099)),
                dot(lms, vec3(0.0259040371,0.7827717662,-0.808675766)));
}
void main() {
    vec4 result = shaded_color;
    if (gray_view) result = vec4(vec3(0.78) * light_intensity, shaded_color.a);
    if (preview_color_count > 0) {
        vec3 lab = to_oklab(source_rgb);
        vec3 selected = source_rgb;
        float best = 100.0;
        for (int i = 0; i < 6; ++i) {
            if (local_color_lock > 0.5) break;
            if (i >= preview_color_count) break;
            vec3 difference = lab - preview_lab[i];
            difference.x *= preview_lightness_weight;
            float d = dot(difference, difference);
            if (d < best) { best = d; selected = preview_rgb[i]; }
        }
        // Quantize after source interpolation. The unlit view outputs only
        // palette entries, never an interpolated or shaded target color.
        result = vec4(preview_lighting ? selected * light_intensity : selected, 1.0);
    }
)" + (modern ? "out_color = result; }" : "gl_FragColor = result; }");
    return shader.init_from_texts("ai_model_vertex_color", sources);
}
} // namespace Slic3r::GUI
