#version 140

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
const float EPSILON = 0.0001;

#define INTENSITY_CORRECTION 0.6
#define PHONG_BRIGHTNESS     1.0

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  128.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)
#define LIGHT_FRONT_SPECULAR (0.28 * INTENSITY_CORRECTION)
#define LIGHT_FRONT_SHININESS 64.0

#define INTENSITY_AMBIENT    0.22
#define WINDOW_REFLECTION_INTENSITY 0.55

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

uniform float z_far;
uniform float z_near;
uniform bool enable_ssao;

in vec3 clipping_planes_dots;
in float color_clip_plane_dot;

in vec4 world_pos;
in float world_normal_z;
in vec3 eye_normal;
in vec3 eye_position;

out vec4 out_color;

vec3 getBackfaceColor(vec3 fill) {
    float brightness = 0.2126 * fill.r + 0.7152 * fill.g + 0.0722 * fill.b;
    return (brightness > 0.75) ? vec3(0.11, 0.165, 0.208) : vec3(0.988, 0.988, 0.988);
}

// Silhouette edge detection & rendering algorithm by leoneruggiero
// https://www.shadertoy.com/view/DslXz2
#define INFLATE 1

float GetTolerance(float d, float k)
{
    float A=-   (z_far+z_near)/(z_far-z_near);
    float B=-2.0*z_far*z_near /(z_far-z_near);

    d = d*2.0-1.0;

    return -k*(d+A)*(d+A)/B;
}

float DetectSilho(vec2 fragCoord, vec2 dir)
{
    float x0 = abs(texture(depth_tex, (fragCoord + dir*-2.0) / screen_size).r);
    float x1 = abs(texture(depth_tex, (fragCoord + dir*-1.0) / screen_size).r);
    float x2 = abs(texture(depth_tex, (fragCoord + dir* 0.0) / screen_size).r);
    float x3 = abs(texture(depth_tex, (fragCoord + dir* 1.0) / screen_size).r);

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
        DetectSilho(fragCoord, vec2(1,0)),
        DetectSilho(fragCoord, vec2(0,1))
        );
}

float compute_ssao_factor(vec3 normal, vec3 view_dir, vec3 eye_pos)
{
    vec3 normal_dx = dFdx(normal);
    vec3 normal_dy = dFdy(normal);
    float normal_variation = clamp(length(normal_dx) + length(normal_dy), 0.0, 1.0);

    float depth_gradient = clamp(length(vec2(dFdx(eye_pos.z), dFdy(eye_pos.z))) * 0.8, 0.0, 1.0);

    float cavity = clamp(normal_variation * 0.70 + depth_gradient * 0.60, 0.0, 1.0);
    float cavity_mask = smoothstep(0.25, 0.75, cavity);
    float ao_strength = pow(cavity, 1.15) * cavity_mask;
    return clamp(1.0 - ao_strength * 0.90, 0.25, 1.0);
}

float soft_circle(vec2 p, vec2 center, float radius, float blur)
{
    float dist = distance(p, center);
    return 1.0 - smoothstep(radius - blur, radius, dist);
}

vec3 compute_window_reflection(vec3 normal, vec3 view_dir)
{
    const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
    
    vec3 light_dir = normalize(LIGHT_TOP_DIR);
    vec3 reflect_light = normalize(reflect(-light_dir, normal));
    
    // UV coordinates for the reflection
    vec2 uv = (reflect_light.xy / (1.0 + max(reflect_light.z, 0.3))) * 2.2;
    
    vec2 grad = fwidth(uv) * 0.8;
    float blur = 0.12 + grad.x * 1.5;
    
    // === CIRCULAR WINDOW (porthole style) ===
    // Single round window, no bars
    vec2 window_center = vec2(0.0, 0.0);
    float window_radius = 0.5;  // Radius of the circular window
    
    float window_light = soft_circle(uv, window_center, window_radius, blur);
    
    // No bars - just pure circular glass
    float bars = 1.0;
    
    // Fresnel effect for edge glow
    float fresnel = pow(1.0 - max(dot(normal, view_dir), 0.0), 1.0);
    float facing = smoothstep(-0.4, 0.6, reflect_light.z);
    
    float intensity = window_light * bars * (0.25 + 0.25 * fresnel) * facing;
    intensity = clamp(intensity, 0.0, 0.45);
    
    return vec3(intensity);
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

    vec3 pv_check_min = ZERO;
    vec3 pv_check_max = ZERO;
    if (print_volume.type == 0) {
        pv_check_min = world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x);
        pv_check_max = world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y);
    }
    else if (print_volume.type == 1) {
        float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
        pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x);
        pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y);
    }
    color.rgb = (any(lessThan(pv_check_min, ZERO)) || any(greaterThan(pv_check_max, ZERO))) ? mix(color.rgb, ZERO, 0.3333) : color.rgb;

    vec3 normal = normalize(eye_normal);
    vec3 view_dir = normalize(-eye_position);

    float NdotL_top = max(dot(normal, LIGHT_TOP_DIR), 0.0);
    float diffuse = INTENSITY_AMBIENT + NdotL_top * LIGHT_TOP_DIFFUSE;
    vec3 half_top = normalize(LIGHT_TOP_DIR + view_dir);
    float specular = LIGHT_TOP_SPECULAR * pow(max(dot(normal, half_top), 0.0), LIGHT_TOP_SHININESS);

    float NdotL_front = max(dot(normal, LIGHT_FRONT_DIR), 0.0);
    diffuse += NdotL_front * LIGHT_FRONT_DIFFUSE;
    vec3 half_front = normalize(LIGHT_FRONT_DIR + view_dir);
    specular += LIGHT_FRONT_SPECULAR * pow(max(dot(normal, half_front), 0.0), LIGHT_FRONT_SHININESS);
    vec3 window_reflection = compute_window_reflection(normal, view_dir);

    // SSAO is applied in post-process pass. Keep base lighting unchanged here.

    if (is_outline) {
        vec3 shaded_rgb = (vec3(specular) + window_reflection + color.rgb * diffuse) * PHONG_BRIGHTNESS;
        vec4 shaded_color = vec4(clamp(shaded_rgb, vec3(0.0), vec3(1.0)), color.a);
        vec2 fragCoord = gl_FragCoord.xy;
        float s = DetectSilho(fragCoord);
        for(int i=1;i<=INFLATE; i++)
        {
           s = max(s, DetectSilho(fragCoord.xy + vec2(i, 0)));
           s = max(s, DetectSilho(fragCoord.xy + vec2(0, i)));
        }
        out_color = vec4(mix(shaded_color.rgb, getBackfaceColor(shaded_color.rgb), s), shaded_color.a);
    }
#ifdef ENABLE_ENVIRONMENT_MAP
    else if (use_environment_tex)
        out_color = vec4(clamp((0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + window_reflection + 0.8 * color.rgb * diffuse) * PHONG_BRIGHTNESS, vec3(0.0), vec3(1.0)), color.a);
#endif
    else
        out_color = vec4(clamp((vec3(specular) + window_reflection + color.rgb * diffuse) * PHONG_BRIGHTNESS, vec3(0.0), vec3(1.0)), color.a);
}