#version 140

uniform sampler2D uniform_texture;

in vec2 tex_coord;

void main()
{
    gl_FragColor = texture(uniform_texture, tex_coord);
}
