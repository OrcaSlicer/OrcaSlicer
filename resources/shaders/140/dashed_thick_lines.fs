#version 150

// see as reference: https://github.com/mhalber/Lines/blob/master/geometry_shader_lines.h
//                   https://stackoverflow.com/questions/52928678/dashed-line-in-opengl3

const float aa_radius = 0.5;

uniform float dash_size;
uniform float gap_size;
uniform vec4 uniform_color;

in float line_width;
// x = v tex coord, y = s coord
in vec2 seg_params;

out vec4 out_color;

void main()
{
	float inv_stride = 1.0 / (dash_size + gap_size);
    if (gap_size > 0.0 && fract(seg_params.y * inv_stride) > dash_size * inv_stride)
        discard;
		
	// We render a quad that is fattened by r, giving total width of the line to be w+r. We want smoothing to happen
	// around w, so that the edge is properly smoothed out. As such, in the smoothstep function we have:
	// Far edge   : 1.0                                          = (w+r) / (w+r)
	// Close edge : 1.0 - (2r / (w+r)) = (w+r)/(w+r) - 2r/(w+r)) = (w-r) / (w+r)
	// This way the smoothing is centered around 'w'.

	out_color = uniform_color;
	float inv_line_width = 1.0 / line_width;
	float aa = 1.0 - smoothstep(1.0 - (2.0 * aa_radius * inv_line_width),  1.0, abs(seg_params.x * inv_line_width));
	out_color.a *= aa;
}
