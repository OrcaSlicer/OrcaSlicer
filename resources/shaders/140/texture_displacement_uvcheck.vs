#version 140

// Vertex stage for the UV-check overlay (checker / distortion heatmap) drawn over the painted patch
// by GLGizmoTextureDisplacement. Reuses GLModel's P3N3T2 layout so it needs no bespoke buffer:
//   v_normal.x  - per-vertex UV distortion (uv-area / surface-area ratio, remapped so 0.5 = ideal);
//                  only the distortion mode reads it.
//   v_tex_coord - precomputed texture uv, valid only when use_vertex_uv is set (LSCM); the checker
//                  mode reconstructs uv in the fragment shader otherwise.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat4 volume_world_matrix;
uniform vec2 z_range;
uniform vec4 clipping_plane;

in vec3 v_position;
in vec3 v_normal;
in vec2 v_tex_coord;

out vec3  clipping_planes_dots;
out vec4  model_pos;
out vec4  world_pos;
out float distortion;
out vec2  vertex_uv;

void main()
{
    model_pos = vec4(v_position, 1.0);
    world_pos = volume_world_matrix * model_pos;

    gl_Position = projection_matrix * view_model_matrix * model_pos;
    clipping_planes_dots = vec3(dot(world_pos, clipping_plane), world_pos.z - z_range.x, z_range.y - world_pos.z);

    distortion = v_normal.x;
    vertex_uv  = v_tex_coord;
}
