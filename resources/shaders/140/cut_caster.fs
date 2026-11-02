#version 140

// A cut body is open where it is cut, and from the light a ray through an opening would reach
// the body's far wall, leaving whatever stands in the opening, such as the toolpaths of the
// layers above, to throw its shadow through the hollow. The material the cut stands for would
// stop the ray at the cut, so a back face the ray reached through an opening is drawn at the
// depth of the cut plane it came through: the nearest of the cut planes on the way back toward
// the light. A ray that came in through a wall instead, which the front faces drawn on their
// own into front_depth tell, leaves the back face alone: that wall holds the depth already.

const vec3 ZERO = vec3(0.0, 0.0, 0.0);

uniform vec2 z_range;
uniform vec4 clipping_plane;
// world to the light's clip space
uniform mat4 light_view_projection;
// unit, from the scene toward the light
uniform vec3 to_light;
// the body's front faces alone, from the light, and the size of the map
uniform sampler2D front_depth;
uniform vec2 map_size;

in vec3 clipping_planes_dots;
in vec3 world_position;
out vec4 out_color;

// How far back toward the light the plane lies, negative where it does not; the plane in the
// keep-where-the-dot-is-positive convention of the clipping above.
float way_to_plane(vec4 plane)
{
    float approach = dot(to_light, plane.xyz);
    if (abs(approach) < 1e-6)
        return -1.0;
    return -dot(vec4(world_position, 1.0), plane) / approach;
}

float nearer(float way, float candidate)
{
    return (candidate > 0.0 && (way < 0.0 || candidate < way)) ? candidate : way;
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    float depth = gl_FragCoord.z;
    if (!gl_FrontFacing) {
        // a wall facing the light lies nearer: the ray came through it, not through a cut
        if (texture(front_depth, gl_FragCoord.xy / map_size).r < gl_FragCoord.z - 0.001)
            discard;
        float way = -1.0;
        way = nearer(way, way_to_plane(clipping_plane));
        way = nearer(way, way_to_plane(vec4(0.0, 0.0, 1.0, -z_range.x)));
        way = nearer(way, way_to_plane(vec4(0.0, 0.0, -1.0, z_range.y)));
        if (way > 0.0) {
            vec4 cut = light_view_projection * vec4(world_position + way * to_light, 1.0);
            depth = 0.5 * cut.z / cut.w + 0.5;
        }
    }
    gl_FragDepth = depth;
    out_color = vec4(0.0, 0.0, 0.0, 1.0);
}
