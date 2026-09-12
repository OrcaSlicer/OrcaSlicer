#version 110

// See resources/shaders/140/texture_displacement_bump.vs for full documentation; this is the
// GLSL 1.10 compatibility variant.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat4 volume_world_matrix;
uniform vec2 z_range;
uniform vec4 clipping_plane;

attribute vec3 v_position;
attribute vec3 v_normal;    // .x = paint weight (0/1); .y = 1 for the dragged island's vertices
attribute vec2 v_tex_coord; // precomputed texture uv, used only when use_vertex_uv is set

varying vec3  clipping_planes_dots;
varying vec4  model_pos;
varying vec4  world_pos;
varying float weight;
varying float island_active;
varying vec2  vertex_uv;

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
