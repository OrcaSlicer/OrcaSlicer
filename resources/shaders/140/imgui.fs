#version 140

uniform sampler2D s_texture;

in vec2 Frag_UV;
in vec4 Frag_Color;

out vec4 out_color;

void main()
{
    out_color = Frag_Color * texture(s_texture, Frag_UV.st);
}