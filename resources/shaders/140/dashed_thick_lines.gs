#version 150

// see as reference: https://github.com/mhalber/Lines/blob/master/geometry_shader_lines.h
//                   https://stackoverflow.com/questions/52928678/dashed-line-in-opengl3

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

const float aa_radius = 0.5;

uniform vec2 viewport_size;
uniform float width;

in float coord_s[];

out float line_width;
// x = v tex coord, y = s coord
out vec2 seg_params;

void main()
{
	vec2 ndc_0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w;
	vec2 ndc_1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w;

	vec2 dir = normalize((ndc_1 - ndc_0) * viewport_size);
	vec2 normal_dir = vec2(-dir.y, dir.x);

	line_width  = max(1.0, width) + 2.0 * aa_radius;
	float half_line_width  = 0.5 * line_width;
	
	vec2 normal = vec2(line_width / viewport_size[0], line_width / viewport_size[1]) * normal_dir;

	seg_params = vec2(-half_line_width, coord_s[0]);
	gl_Position = vec4((ndc_0 + normal) * gl_in[0].gl_Position.w, gl_in[0].gl_Position.zw);
	EmitVertex();

	seg_params = vec2(-half_line_width, coord_s[0]);
	gl_Position = vec4((ndc_0 - normal) * gl_in[0].gl_Position.w, gl_in[0].gl_Position.zw);
	EmitVertex();

	seg_params = vec2(half_line_width, coord_s[1]);
	gl_Position = vec4((ndc_1 + normal) * gl_in[1].gl_Position.w, gl_in[1].gl_Position.zw);
	EmitVertex();

	seg_params = vec2(half_line_width, coord_s[1]);
	gl_Position = vec4((ndc_1 - normal) * gl_in[1].gl_Position.w, gl_in[1].gl_Position.zw);
	EmitVertex();

	EndPrimitive();
}
