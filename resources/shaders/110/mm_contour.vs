#version 110

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform float offset;

attribute vec3 v_position;

void main()
{
    // Add small epsilon to z to solve z-fighting between painted triangles and contour lines.
	vec4 clip_position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
	clip_position.z -= offset * abs(clip_position.w);
    gl_Position = clip_position;
}
