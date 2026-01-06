#version 140

uniform sampler2D Texture;

in vec2 Frag_UV;
in vec4 Frag_Color;

void main()
{
	gl_FragColor = Frag_Color * texture(Texture, Frag_UV.st);
}