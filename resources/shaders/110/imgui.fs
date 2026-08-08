#version 110

uniform sampler2D s_texture;

varying vec2 Frag_UV;
varying vec4 Frag_Color;

void main()
{
    gl_FragColor = Frag_Color * texture2D(s_texture, Frag_UV.st);
}