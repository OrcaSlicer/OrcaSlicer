#version 140

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;
uniform mat4 volume_world_matrix;

// Clipping, as in gouraud.vs: the preview's top / bottom plane and the SLA gizmo's free one.
uniform vec2 z_range;
uniform vec4 clipping_plane;

in vec3 v_position;
in vec3 v_normal;

out vec3 eye_normal;
out vec3 eye_position;
out vec3 clipping_planes_dots;

void main()
{
    eye_normal = view_normal_matrix * v_normal;

    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    eye_position = position.xyz;

    // A component below zero means the fragment is clipped away.
    vec4 world_pos = volume_world_matrix * vec4(v_position, 1.0);
    clipping_planes_dots = vec3(dot(world_pos, clipping_plane), world_pos.z - z_range.x, z_range.y - world_pos.z);

    gl_Position = projection_matrix * position;
}
