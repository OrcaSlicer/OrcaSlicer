#version 110

// The solid model of the G-code preview drawn depth-only into the shadow map, cut as the gouraud
// pass cuts it; see cut_caster.fs.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat4 volume_world_matrix;

// Clipping plane, x = min z, y = max z.
uniform vec2 z_range;
// Clipping plane - general orientation, in the keep-where-the-dot-is-positive convention.
uniform vec4 clipping_plane;

attribute vec3 v_position;

varying vec3 clipping_planes_dots;
varying vec3 world_position;

void main()
{
    vec4 world_pos = volume_world_matrix * vec4(v_position, 1.0);
    world_position = world_pos.xyz;
    // Fill in the scalars for fragment shader clipping. Fragments with any of these components lower than zero are discarded.
    clipping_planes_dots = vec3(dot(world_pos, clipping_plane), world_pos.z - z_range.x, z_range.y - world_pos.z);

    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
