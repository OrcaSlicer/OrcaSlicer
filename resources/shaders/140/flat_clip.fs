#version 140

const vec3 ZERO = vec3(0.0, 0.0, 0.0);

uniform vec4 uniform_color;

in vec3 clipping_planes_dots;

out vec4 out_color;

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    out_color = uniform_color;
}
