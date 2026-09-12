#version 140

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

uniform mat4 volume_world_matrix;
// Clipping plane, x = min z, y = max z. Used by the FFF and SLA previews to clip with a top / bottom plane.
uniform vec2 z_range;
// Clipping plane - general orientation. Used by the SLA gizmo.
uniform vec4 clipping_plane;

in vec3 v_position;
// GLModel's P3N3T2 layout (position + normal + texcoord), reused so this mesh builds and renders
// like any other GLModel rather than needing a bespoke vertex buffer. The two spare channels carry
// what the bump preview actually needs per vertex:
//   v_normal.x   -- the active layer's paint weight, 0 (untouched) or 1 (painted).
//   v_normal.y   -- 1 for a vertex of the island currently being dragged in the UV editor, else 0.
//                   The fragment shader applies island_delta to those vertices' uv, so a UV drag is a
//                   single uniform update rather than a whole-mesh rebuild (like Adjust placement).
//   v_tex_coord  -- the precomputed texture uv for this vertex, valid only when use_vertex_uv is set
//                   (i.e. the LSCM projection, where uv can't be reconstructed in the shader). The
//                   triplanar path ignores it and projects in the fragment shader instead.
in vec3 v_normal;
in vec2 v_tex_coord;

out vec3  clipping_planes_dots;
out vec4  model_pos;
out vec4  world_pos;
out float weight;
out float island_active;
out vec2  vertex_uv;

void main()
{
    model_pos = vec4(v_position, 1.0);
    world_pos = volume_world_matrix * model_pos;

    gl_Position = projection_matrix * view_model_matrix * model_pos;
    clipping_planes_dots = vec3(dot(world_pos, clipping_plane), world_pos.z - z_range.x, z_range.y - world_pos.z);

    weight        = v_normal.x;
    island_active = v_normal.y;
    vertex_uv     = v_tex_coord;
}
