#version 140

// X-Ray view: each surface is a thin translucent layer of its own color, covering more the more
// edge-on it is seen. The caller draws it with depth writes off, so every surface a view ray crosses
// composites and overlapping geometry reads as denser. Source-alpha rather than additive blending:
// additive would saturate to white on the light theme's near-white background.
#define EDGE_FALLOFF 2.0
// Coverage head-on and edge-on.
#define FACE_DENSITY 0.12
#define EDGE_DENSITY 0.65

uniform vec4 uniform_color;

in vec3 eye_normal;
in vec3 eye_position;
in vec3 clipping_planes_dots;

out vec4 out_color;

void main()
{
    if (any(lessThan(clipping_planes_dots, vec3(0.0))))
        discard;

    // abs(): back faces are drawn too, and there only the angle matters.
    float facing = abs(dot(normalize(eye_normal), normalize(-eye_position)));
    float density = mix(EDGE_DENSITY, FACE_DENSITY, pow(facing, EDGE_FALLOFF));

    out_color = vec4(uniform_color.rgb, density * uniform_color.a);
}
