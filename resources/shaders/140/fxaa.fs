#version 140

uniform sampler2D uniform_texture;
uniform vec2 inv_tex_size;

in vec2 tex_coord;

out vec4 out_color;

void main()
{
    vec3 rgbNW = texture(uniform_texture, tex_coord + vec2(-1.0, -1.0) * inv_tex_size).rgb;
    vec3 rgbNE = texture(uniform_texture, tex_coord + vec2(1.0, -1.0) * inv_tex_size).rgb;
    vec3 rgbSW = texture(uniform_texture, tex_coord + vec2(-1.0, 1.0) * inv_tex_size).rgb;
    vec3 rgbSE = texture(uniform_texture, tex_coord + vec2(1.0, 1.0) * inv_tex_size).rgb;
    vec3 rgbM  = texture(uniform_texture, tex_coord).rgb;

    vec3 luma_coeff = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma_coeff);
    float lumaNE = dot(rgbNE, luma_coeff);
    float lumaSW = dot(rgbSW, luma_coeff);
    float lumaSE = dot(rgbSE, luma_coeff);
    float lumaM  = dot(rgbM, luma_coeff);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    const float FXAA_REDUCE_MIN = 1.0 / 128.0;
    const float FXAA_REDUCE_MUL = 1.0 / 8.0;
    const float FXAA_SPAN_MAX = 8.0;

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(FXAA_SPAN_MAX), max(vec2(-FXAA_SPAN_MAX), dir * rcpDirMin)) * inv_tex_size;

    vec3 rgbA = 0.5 * (
        texture(uniform_texture, tex_coord + dir * (1.0 / 3.0 - 0.5)).rgb +
        texture(uniform_texture, tex_coord + dir * (2.0 / 3.0 - 0.5)).rgb
    );

    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(uniform_texture, tex_coord + dir * -0.5).rgb +
        texture(uniform_texture, tex_coord + dir * 0.5).rgb
    );

    float lumaB = dot(rgbB, luma_coeff);
    out_color = (lumaB < lumaMin || lumaB > lumaMax) ? vec4(rgbA, 1.0) : vec4(rgbB, 1.0);
}
