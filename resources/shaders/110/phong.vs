#version 110

const vec3 ZERO = vec3(0.0, 0.0, 0.0);

struct SlopeDetection
{
    bool actived;
    float normal_z;
    mat3 volume_world_normal_matrix;
};

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;
uniform mat4 volume_world_matrix;
uniform SlopeDetection slope;
uniform bool is_outline;
uniform vec2 screen_size;

// Clipping plane, x = min z, y = max z. Used by the FFF and SLA previews to clip with a top / bottom plane.
uniform vec2 z_range;
// Clipping plane - general orientation. Used by the SLA gizmo.
uniform vec4 clipping_plane;
// Color clip plane - general orientation. Used by the cut gizmo.
uniform vec4 color_clip_plane;

attribute vec3 v_position;
attribute vec3 v_normal;

varying vec3 clipping_planes_dots;
varying float color_clip_plane_dot;

varying vec4 world_pos;
varying float world_normal_z;
varying vec3 eye_normal;
varying vec3 eye_position;

void main()
{
    // First transform the normal into camera space and normalize the result.
    eye_normal = normalize(view_normal_matrix * v_normal);

    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    eye_position = position.xyz;

    // Point in homogenous coordinates.
    world_pos = volume_world_matrix * vec4(v_position, 1.0);

    // z component of normal vector in world coordinate used for slope shading
    world_normal_z = slope.actived ? (normalize(slope.volume_world_normal_matrix * v_normal)).z : 0.0;

    gl_Position = projection_matrix * position;
    if (is_outline) {
        vec3 n = normalize((view_normal_matrix * v_normal).xyz);
        vec2 dir = normalize(n.xy);
        if (dot(dir, dir) > 0.0) {
            //set outline thickness
            float px = 3.0;
            gl_Position.xy += dir * (px * 2.0 / screen_size) * gl_Position.w;
        }
    }
    // Fill in the scalars for fragment shader clipping. Fragments with any of these components lower than zero are discarded.
    clipping_planes_dots = vec3(dot(world_pos, clipping_plane), world_pos.z - z_range.x, z_range.y - world_pos.z);
    color_clip_plane_dot = dot(world_pos, color_clip_plane);
}
