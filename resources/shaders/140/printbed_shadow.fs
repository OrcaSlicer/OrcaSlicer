#version 140

// Draws the build-plate as a receiver of the same depth shadow map used for object/self shadows,
// so the plate, objects, and self-shadows all come from one unified technique.
uniform sampler2D shadow_map;
uniform mat4 shadow_light_vp;
uniform float shadow_intensity;
uniform float shadow_map_texel;

in vec4 world_pos;

out vec4 out_color;

// Fraction of the 5x5 PCF kernel occluded from the light. Matches the object shadow shader.
float shadow_occlusion()
{
    vec4 lp = shadow_light_vp * world_pos;
    vec3 proj = lp.xyz / lp.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0)
        return 0.0;

    // The plate is a pure receiver (never rendered into the shadow map), so a tiny constant
    // bias for numerical safety is enough here.
    float bias = 0.0004;
    float sum = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            float closest = texture(shadow_map, proj.xy + vec2(float(x), float(y)) * shadow_map_texel).r;
            sum += (proj.z - bias > closest) ? 1.0 : 0.0;
        }
    }
    return sum / 25.0;
}

void main()
{
    float occ = shadow_occlusion();
    if (occ <= 0.0)
        discard;
    out_color = vec4(0.0, 0.0, 0.0, shadow_intensity * occ);
}
