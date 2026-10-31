#version 140

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
// Farthest triangle edge distance stored, in pixels.
const float EXIT_REACH = 4.0;

// 1 to 255, told apart between neighboring volumes.
uniform float part_id;

in vec3 clipping_planes_dots;
in vec3 eye_normal;
in vec3 to_camera;
noperspective in vec3 barycentric;

out vec4 out_color;

// Two bytes in one channel of the 16 bit target, offset so that a truncating conversion keeps them.
float pack2(float hi, float lo)
{
    return (hi * 256.0 + lo + 0.25) / 65535.0;
}

vec2 oct_encode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return floor((e * 0.5 + 0.5) * 255.0 + 0.5);
}

// Pixels to the edge of the triangle along the screen direction over which the barycentrics change by d.
float exit_distance(vec3 d)
{
    vec3 t = vec3(EXIT_REACH);
    if (d.x < 0.0) t.x = barycentric.x / -d.x;
    if (d.y < 0.0) t.y = barycentric.y / -d.y;
    if (d.z < 0.0) t.z = barycentric.z / -d.z;
    return floor(clamp(min(t.x, min(t.y, t.z)) / EXIT_REACH, 0.0, 1.0) * 255.0 + 0.5);
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    // Back faces show through cuts, so every surface is compared by its side facing the camera.
    vec3 n = normalize(eye_normal);
    if (dot(n, to_camera) < 0.0)
        n = -n;
    vec2 e = oct_encode(n);
    vec3 bx = dFdx(barycentric);
    vec3 by = dFdy(barycentric);
    out_color = vec4(pack2(e.x, e.y), pack2(part_id, 0.0),
                     pack2(exit_distance(bx), exit_distance(-bx)), pack2(exit_distance(by), exit_distance(-by)));
}
