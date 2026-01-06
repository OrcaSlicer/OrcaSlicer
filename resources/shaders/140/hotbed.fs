#version 140

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
const vec3 WHITE = vec3(1.0, 1.0, 1.0);
struct PrintVolumeDetection
{
	// 0 = rectangle, 1 = circle, 2 = custom, 3 = invalid
	int type;
    // type = 0 (rectangle):
    // x = min.x, y = min.y, z = max.x, w = max.y
    // type = 1 (circle):
    // x = center.x, y = center.y, z = radius
	vec4 xy_data;
    // x = min z, y = max z
	vec2 z_data;
};

uniform vec4 uniform_color;
uniform float emission_factor;
uniform PrintVolumeDetection print_volume;
// x = diffuse, y = specular;
in vec2 intensity;
in vec4 world_pos;

out vec4 out_color;

void main()
{
    vec3  color = uniform_color.rgb;
    float alpha = uniform_color.a;
	// if the fragment is outside the print volume -> use darker color
    vec3 pv_check_min = ZERO;
    vec3 pv_check_max = ZERO;
    if (print_volume.type == 0) {// rectangle
        pv_check_min = world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x);
        pv_check_max = world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y);
    }
    else if (print_volume.type == 1) {// circle
        float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
        pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x);
        pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y);
    }
    color = (any(lessThan(pv_check_min, ZERO)) || any(greaterThan(pv_check_max, ZERO))) ? mix(color, WHITE, 0.3333) : color;
	//out_color = vec4(vec3(intensity.y) + color * intensity.x, alpha);
	out_color = vec4(vec3(intensity.y) + color * (intensity.x + emission_factor), alpha);
}