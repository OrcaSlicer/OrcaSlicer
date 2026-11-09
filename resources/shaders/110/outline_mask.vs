#version 110

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;
uniform mat4 volume_world_matrix;

// Clipping plane, x = min z, y = max z. Used by the FFF and SLA previews to clip with a top / bottom plane.
uniform vec2 z_range;
// Clipping plane - general orientation. Used by the SLA gizmo.
uniform vec4 clipping_plane;

attribute vec3 v_position;
attribute vec3 v_normal;

varying vec3 clipping_planes_dots;
varying vec3 eye_normal;
varying vec3 to_camera;

void main()
{
    vec4 world_pos = volume_world_matrix * vec4(v_position, 1.0);
    clipping_planes_dots = vec3(dot(world_pos, clipping_plane), world_pos.z - z_range.x, z_range.y - world_pos.z);

    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    eye_normal = view_normal_matrix * v_normal;
    // projection_matrix[3][3] is 1 only for an orthographic camera, whose view rays all run along -z.
    to_camera = mix(-position.xyz, vec3(0.0, 0.0, 1.0), projection_matrix[3][3]);
    gl_Position = projection_matrix * position;
}
