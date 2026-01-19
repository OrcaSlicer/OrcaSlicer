#version 140

in vec3 v_position;
in vec2 v_tex_coord;

out vec2 tex_coord;

void main()
{
	tex_coord = v_tex_coord;
    gl_Position = vec4(v_position, 1.0);
}
