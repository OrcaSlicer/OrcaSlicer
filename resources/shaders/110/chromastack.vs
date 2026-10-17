#version 110

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;
uniform mat4 volume_world_matrix;
// The volume's normal matrix in world space, so the fragment shader can tell a wall from a terrace -
// which is what decides whether layer lines show at all.
uniform mat3 world_normal_matrix;
// Clipping plane, x = min z, y = max z (the FFF/SLA preview's top/bottom cut).
uniform vec2 z_range;
// Clipping plane, general orientation (the section view).
uniform vec4 clipping_plane;

// GLModel's P3N3 layout, which is what init_from(indexed_triangle_set) produces - this shader needs
// nothing per vertex that the mesh does not already carry, because the colour comes from the
// fragment's own height rather than from anything baked into the vertices.
attribute vec3 v_position;
attribute vec3 v_normal;

varying vec3  clipping_planes_dots;
varying vec3  eye_normal;
varying vec3  eye_position;
varying vec3  world_normal;
varying float world_z;

void main()
{
    vec4 model_pos = vec4(v_position, 1.0);
    vec4 world_pos = volume_world_matrix * model_pos;
    world_z        = world_pos.z;
    world_normal   = normalize(world_normal_matrix * v_normal);
    eye_normal     = normalize(view_normal_matrix * v_normal);

    vec4 eye_pos = view_model_matrix * model_pos;
    eye_position = eye_pos.xyz;
    gl_Position  = projection_matrix * eye_pos;

    clipping_planes_dots = vec3(dot(world_pos, clipping_plane), world_pos.z - z_range.x, z_range.y - world_pos.z);
}
