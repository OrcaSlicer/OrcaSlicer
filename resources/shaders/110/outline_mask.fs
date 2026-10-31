#version 110

const vec3 ZERO = vec3(0.0, 0.0, 0.0);

// 1 to 255, told apart between neighboring volumes.
uniform float part_id;

varying vec3 clipping_planes_dots;
varying vec3 eye_normal;
varying vec3 to_camera;

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

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    // Back faces show through cuts, so every surface is compared by its side facing the camera.
    vec3 n = normalize(eye_normal);
    if (dot(n, to_camera) < 0.0)
        n = -n;
    vec2 e = oct_encode(n);
    // Without gl_VertexID there are no barycentrics to tell where the triangle ends.
    gl_FragColor = vec4(pack2(e.x, e.y), pack2(part_id, 0.0), 0.0, 0.0);
}
