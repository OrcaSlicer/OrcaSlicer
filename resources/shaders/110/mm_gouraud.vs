#version 110

const vec3 ZERO = vec3(0.0, 0.0, 0.0);

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

uniform mat4 volume_world_matrix;
// Clipping plane, x = min z, y = max z. Used by the FFF and SLA previews to clip with a top / bottom plane.
uniform vec2 z_range;
// Clipping plane - general orientation. Used by the SLA gizmo.
uniform vec4 clipping_plane;

attribute vec3 v_position;
attribute vec3 v_barycentric;

varying vec3 clipping_planes_dots;
varying vec4 model_pos;
varying vec4 world_pos;
varying vec3 barycentric_coordinates;

struct SlopeDetection
{
    bool actived;
	float normal_z;
    mat3 volume_world_normal_matrix;
};
uniform SlopeDetection slope;
void main()
{
    model_pos = vec4(v_position, 1.0);
    // Point in homogenous coordinates.
    world_pos = volume_world_matrix * model_pos;

    gl_Position = projection_matrix * view_model_matrix * model_pos;
    // Fill in the scalars for fragment shader clipping. Fragments with any of these components lower than zero are discarded.
    clipping_planes_dots = vec3(dot(world_pos, clipping_plane), world_pos.z - z_range.x, z_range.y - world_pos.z);

    //compute the Barycentric Coordinates
    barycentric_coordinates = v_barycentric;
}
