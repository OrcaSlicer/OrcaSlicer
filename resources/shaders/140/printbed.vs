#version 140

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

in vec3 v_position;
in vec2 v_tex_coord;

out vec2 tex_coord;

void main()
{
	tex_coord = v_tex_coord;
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
