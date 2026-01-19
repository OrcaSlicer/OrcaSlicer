#version 110

const vec3 ZERO = vec3(0.0, 0.0, 0.0);

uniform vec4 uniform_color;

varying vec3 clipping_planes_dots;

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    gl_FragColor = uniform_color;
}
