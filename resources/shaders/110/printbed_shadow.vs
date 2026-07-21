#version 110

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

attribute vec3 v_position;

// The plate mask quad is authored directly in world coordinates (z = 0 plane),
// so v_position is already the world position of the fragment.
varying vec4 world_pos;

void main()
{
    world_pos = vec4(v_position, 1.0);
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
