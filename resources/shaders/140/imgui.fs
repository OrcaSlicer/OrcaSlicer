#version 140

uniform sampler2D Texture;

in vec2 Frag_UV;
in vec4 Frag_Color;

out vec4 out_color;

void main()
{
	out_color = Frag_Color * texture(Texture, Frag_UV.st);
}