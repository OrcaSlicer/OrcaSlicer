#version 140

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat4 volume_world_matrix;

// Clipping plane, x = min z, y = max z. Used by the FFF and SLA previews to clip with a top / bottom plane.
uniform vec2 z_range;
// Clipping plane - general orientation. Used by the SLA gizmo.
uniform vec4 clipping_plane;

in vec3 v_position;

out vec3 clipping_planes_dots;

void main()
{
    // Fill in the scalars for fragment shader clipping. Fragments with any of these components lower than zero are discarded.
    vec4 world_pos = volume_world_matrix * vec4(v_position, 1.0);
    clipping_planes_dots = vec3(dot(world_pos, clipping_plane), world_pos.z - z_range.x, z_range.y - world_pos.z);

    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
