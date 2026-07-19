#version 110

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
};

uniform vec4 uniform_color;
uniform bool use_color_clip_plane;
uniform vec4 uniform_color_clip_plane_1;
uniform vec4 uniform_color_clip_plane_2;
uniform SlopeDetection slope;

//BBS: add outline_color
uniform bool is_outline;
uniform sampler2D depth_tex;
uniform vec2 screen_size;


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

uniform float z_far;
uniform float z_near;

// Depth-based shadow map (object-on-object and self shadows). shadow_intensity == 0 disables it.
uniform sampler2D shadow_map;
uniform mat4 shadow_light_vp;
uniform float shadow_intensity;
uniform float shadow_map_texel;

// LIGHT_TOP_DIR in eye space (matches the diffuse light used for shading in gouraud.vs).
const vec3 SHADOW_LIGHT_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);

varying vec3 clipping_planes_dots;
varying float color_clip_plane_dot;

// x = diffuse, y = specular;
varying vec2 intensity;

varying vec4 world_pos;
varying float world_normal_z;
varying vec3 eye_normal;

vec3 getBackfaceColor(vec3 fill) {
    float brightness = 0.2126 * fill.r + 0.7152 * fill.g + 0.0722 * fill.b;
    return (brightness > 0.75) ? vec3(0.11, 0.165, 0.208) : vec3(0.988, 0.988, 0.988);
}

// Silhouette edge detection & rendering algorithem by leoneruggiero
// https://www.shadertoy.com/view/DslXz2
#define INFLATE 1

float GetTolerance(float d, float k)
{
    // -------------------------------------------
    // Find a tolerance for depth that is constant
    // in view space (k in view space).
    //
    // tol = k*ddx(ZtoDepth(z))
    // -------------------------------------------
    
    float A=-   (z_far+z_near)/(z_far-z_near);
    float B=-2.0*z_far*z_near /(z_far-z_near);
    
    d = d*2.0-1.0;
    
    return -k*(d+A)*(d+A)/B;   
}

float DetectSilho(vec2 fragCoord, vec2 dir)
{
    // -------------------------------------------
    //   x0 ___ x1----o 
    //          :\    : 
    //       r0 : \   : r1
    //          :  \  : 
    //          o---x2 ___ x3
    //
    // r0 and r1 are the differences between actual
    // and expected (as if x0..3 where on the same
    // plane) depth values.
    // -------------------------------------------
    
    float x0 = abs(texture2D(depth_tex, (fragCoord + dir*-2.0) / screen_size).r);
    float x1 = abs(texture2D(depth_tex, (fragCoord + dir*-1.0) / screen_size).r);
    float x2 = abs(texture2D(depth_tex, (fragCoord + dir* 0.0) / screen_size).r);
    float x3 = abs(texture2D(depth_tex, (fragCoord + dir* 1.0) / screen_size).r);
    
    float d0 = (x1-x0);
    float d1 = (x2-x3);
    
    float r0 = x1 + d0 - x2;
    float r1 = x2 + d1 - x1;
    
    float tol = GetTolerance(x2, 0.04);
    
    return smoothstep(0.0, tol*tol, max( - r0*r1, 0.0));

}

float DetectSilho(vec2 fragCoord)
{
    return max(
        DetectSilho(fragCoord, vec2(1,0)), // Horizontal
        DetectSilho(fragCoord, vec2(0,1))  // Vertical
        );
}

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
    float NdotL = dot(normalize(eye_normal), SHADOW_LIGHT_DIR);
    float bias = mix(0.0004, 0.004, clamp(1.0 - NdotL, 0.0, 1.0));
    // 5x5 PCF: softens shadow edges into a smooth penumbra and blurs residual facet acne.
    float sum = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            float closest = texture2D(shadow_map, proj.xy + vec2(float(x), float(y)) * shadow_map_texel).r;
            sum += (proj.z - bias > closest) ? 1.0 : 0.0;
        }
    }
    return 1.0 - shadow_intensity * (sum / 25.0);
}

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

    //BBS: add outline_color
    if (is_outline) {
        color = vec4((vec3(intensity.y) + color.rgb * intensity.x) * shade, color.a);
        vec2 fragCoord = gl_FragCoord.xy;
        float s = DetectSilho(fragCoord);
        // Makes silhouettes thicker.
        for(int i=1;i<=INFLATE; i++)
        {
           s = max(s, DetectSilho(fragCoord.xy + vec2(i, 0)));
           s = max(s, DetectSilho(fragCoord.xy + vec2(0, i)));
        }
        if (s < 0.01)
            discard;
        gl_FragColor = vec4(mix(color.rgb, getBackfaceColor(color.rgb), s), color.a);
    }
#ifdef ENABLE_ENVIRONMENT_MAP
    else if (use_environment_tex)
        gl_FragColor = vec4((0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + 0.8 * color.rgb * intensity.x) * shade, color.a);
#endif
    else
        gl_FragColor = vec4((vec3(intensity.y) + color.rgb * intensity.x) * shade, color.a);
}