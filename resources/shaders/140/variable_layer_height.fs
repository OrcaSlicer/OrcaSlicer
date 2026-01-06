#version 140

#define M_PI 3.1415926535897932384626433832795

// 2D texture (1D texture split by the rows) of color along the object Z axis.
uniform sampler2D z_texture;
// Scaling from the Z texture rows coordinate to the normalized texture row coordinate.
uniform float z_to_texture_row;
uniform float z_texture_row_to_normalized;
uniform float z_cursor;
uniform float z_cursor_band_width;

// x = tainted, y = specular;
in vec2 intensity;

in float object_z;

out vec4 out_color;

void main()
{
    float object_z_row = z_to_texture_row * object_z;
    // Index of the row in the texture.
    float z_texture_row = floor(object_z_row);
    // Normalized coordinate from 0. to 1.
    float z_texture_col = object_z_row - z_texture_row;
    float z_blend = 0.25 * cos(min(M_PI, abs(M_PI * (object_z - z_cursor) * 1.8 / z_cursor_band_width))) + 0.25;
    // Calculate level of detail from the object Z coordinate.
    // This makes the slowly sloping surfaces to be shown with high detail (with stripes),
    // and the vertical surfaces to be shown with low detail (no stripes)
    float z_in_cells = object_z_row * 190.;
    // Gradient of Z projected on the screen.
    float dx_vtc = dFdx(z_in_cells);
    float dy_vtc = dFdy(z_in_cells);
    float lod = clamp(0.5 * log2(max(dx_vtc * dx_vtc, dy_vtc * dy_vtc)), 0., 1.);
    // Sample the Z texture. Texture coordinates are normalized to <0, 1>.
    vec4 color = vec4(0.25, 0.25, 0.25, 1.0);
    if (z_texture_row >= 0.0)
        color = mix(texture(z_texture, vec2(z_texture_col, z_texture_row_to_normalized * (z_texture_row + 0.5    )), -10000.),
                    texture(z_texture, vec2(z_texture_col, z_texture_row_to_normalized * (z_texture_row * 2. + 1.)),  10000.), lod);            
    // Mix the final color.
    out_color = vec4(vec3(intensity.y), 1.0) +  intensity.x * mix(color, vec4(1.0, 1.0, 0.0, 1.0), z_blend);
}
