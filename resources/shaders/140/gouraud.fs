#version 140

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
//BBS: add grey and orange
//const vec3 GREY = vec3(0.9, 0.9, 0.9);
const vec3 ORANGE = vec3(0.8, 0.4, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
const float EPSILON = 0.0001;

struct PrintVolumeDetection
{
	// 0 = rectangle, 1 = circle, 2 = custom, 3 = invalid
	int type;
    // type = 0 (rectangle):
    // x = min.x, y = min.y, z = max.x, w = max.y
    // type = 1 (circle):
    // x = center.x, y = center.y, z = radius
	vec4 xy_data;
    // x = min z, y = max z
	vec2 z_data;
};

struct SlopeDetection
{
    bool actived;
	float normal_z;
    mat3 volume_world_normal_matrix;
    vec3 up_direction;
};

uniform vec4 uniform_color;
uniform bool use_color_clip_plane;
uniform vec4 uniform_color_clip_plane_1;
uniform vec4 uniform_color_clip_plane_2;
uniform SlopeDetection slope;

// ORCA: a part of a translucent filament, drawn as its print would show in premultiplied color, see
// GLVolumeCollection::render(). It catches a highlight only with translucent_gloss, in realistic view.
uniform bool translucent_filament;
uniform bool translucent_gloss;

#ifdef ENABLE_ENVIRONMENT_MAP
    uniform sampler2D environment_tex;
    uniform bool use_environment_tex;
#endif // ENABLE_ENVIRONMENT_MAP

uniform PrintVolumeDetection print_volume;
// BBS H2D/H2C per-extruder printable height (3DScene.cpp): .x = flag (>=1 active), .y/.z = the two
// extruders' Z limits. Inert unless the CPU sets .x >= 1.0 (multi-extruder printers only), so the
// shared object shader stays pixel-identical for single-extruder printers. See 3DScene.cpp.
uniform vec3 extruder_printable_heights;
const float ONE_OVER_EPSILON = 1e4;


// Depth-based shadow map (object-on-object and self shadows). shadow_intensity == 0 disables it.
uniform sampler2D shadow_map;
uniform mat4 shadow_light_vp;
uniform float shadow_intensity;
uniform float shadow_map_texel;

// LIGHT_TOP_DIR in eye space (matches the diffuse light used for shading in gouraud.vs).
const vec3 SHADOW_LIGHT_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
// ORCA: realistic view - static shadows also light the scene from their fixed light.
uniform bool use_static_light;
uniform vec3 static_light_dir;
vec3 top_light_dir() { return use_static_light ? static_light_dir : SHADOW_LIGHT_DIR; }

in vec3 clipping_planes_dots;
in float color_clip_plane_dot;

// x = diffuse, y = specular;
in vec2 intensity;

in vec4 world_pos;
in float world_normal_z;
in vec3 eye_normal;

// Returns a lighting multiplier in [1 - shadow_intensity, 1]: < 1 where the fragment is
// occluded from the light in the shadow map. 3x3 PCF softens the edges.
float shadow_shade()
{
    if (shadow_intensity <= 0.0)
        return 1.0;

    vec4 lp = shadow_light_vp * world_pos;
    vec3 proj = lp.xyz / lp.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0)
        return 1.0;

    // Slope-scaled depth bias: larger where the surface grazes / faces away from the light. This
    // suppresses self-shadow acne without discarding real shadows cast by other objects onto
    // back-facing surfaces (e.g. the shaded back/tip of a cone sitting inside a larger shadow).
    float NdotL = dot(normalize(eye_normal), top_light_dir());
    float bias = mix(0.0004, 0.004, clamp(1.0 - NdotL, 0.0, 1.0));
    // 5x5 PCF: softens shadow edges into a smooth penumbra and blurs residual facet acne.
    float sum = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            float closest = texture(shadow_map, proj.xy + vec2(float(x), float(y)) * shadow_map_texel).r;
            sum += (proj.z - bias > closest) ? 1.0 : 0.0;
        }
    }
    return 1.0 - shadow_intensity * (sum / 25.0);
}

// ORCA: the view ray crosses two shells of the part, the front and the back one, of two 0.45 mm walls each,
// longer at grazing angles, and the opacity of the filament is that of 0.4 mm of it, as in the G-code preview.
vec4 translucent_filament_color(vec3 diffuse, float alpha, vec3 highlight, float cos_view)
{
    float a = 1.0 - pow(1.0 - alpha, 2.0 * 0.9 / 0.4 / max(cos_view, 0.25));
    return vec4(diffuse * a + highlight, a);
}

out vec4 out_color;

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    vec4 color;
	if (use_color_clip_plane) {
		color.rgb = (color_clip_plane_dot < 0.0) ? uniform_color_clip_plane_1.rgb : uniform_color_clip_plane_2.rgb;
		color.a = uniform_color.a;
    }
    else
	    color = uniform_color;

    if (slope.actived) {
         if(world_pos.z<0.1&&world_pos.z>-0.1)
         {
                color.rgb = LightBlue;
                color.a = 0.8;
         }
         else if( world_normal_z < slope.normal_z - EPSILON)
         {
                color.rgb = color.rgb * 0.5 + LightRed * 0.5;
                color.a = 0.8;
         }
    }
    // if the fragment is outside the print volume -> use darker color
	vec3 pv_check_min = ZERO;
	vec3 pv_check_max = ZERO;
    if (print_volume.type == 0) {
		// rectangle
		pv_check_min = world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x);
		pv_check_max = world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y);
	}
	else if (print_volume.type == 1) {
		// circle
		float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
		pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x);
		pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y);
	}
	color.rgb = (any(lessThan(pv_check_min, ZERO)) || any(greaterThan(pv_check_max, ZERO))) ? mix(color.rgb, ZERO, 0.3333) : color.rgb;

    // BBS per-extruder printable-height shading (H2D/H2C). Gated on the flag so it is inert for
    // single-extruder printers. Darkens the band between the two extruders' Z limits inside the bed
    // rect (the zone only the taller extruder can reach). Math kept byte-identical to BBS gouraud.fs.
    if (extruder_printable_heights.x >= 1.0) {
        vec3 eph_check_min = (world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, extruder_printable_heights.y)) * ONE_OVER_EPSILON;
        vec3 eph_check_max = (world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, extruder_printable_heights.z)) * ONE_OVER_EPSILON;
        bool is_out_printable_height = (all(greaterThan(eph_check_min, vec3(1.0))) && all(lessThan(eph_check_max, vec3(1.0))));
        color.rgb = is_out_printable_height ? mix(color.rgb, ZERO, 0.7) : color.rgb;
    }
    float shade = shadow_shade();

#ifdef ENABLE_ENVIRONMENT_MAP
    if (use_environment_tex)
        out_color = vec4((0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + 0.8 * color.rgb * intensity.x) * shade, color.a);
    else
#endif
    if (translucent_filament)
        out_color = translucent_filament_color(color.rgb * intensity.x * shade, color.a,
                                               translucent_gloss ? vec3(intensity.y * shade) : ZERO, abs(normalize(eye_normal).z));
    else
        out_color = vec4((vec3(intensity.y) + color.rgb * intensity.x) * shade, color.a);
}