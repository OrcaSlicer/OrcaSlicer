#version 110

uniform vec4 uniform_color;
uniform float emission_factor;

// x = tainted, y = specular;
varying vec2 intensity;

void main()
{
    gl_FragColor = vec4(vec3(intensity.y) + uniform_color.rgb * (intensity.x + emission_factor), uniform_color.a);
}
