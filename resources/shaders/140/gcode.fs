#version 140

#define INTENSITY_CORRECTION 0.6

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)

#define INTENSITY_AMBIENT    0.3

const vec3 KTravel_Colors[3] = vec3[3](
    vec3(0.505, 0.064, 0.028),  
    vec3(0.219, 0.282, 0.609), 
    vec3(0.112, 0.422, 0.103) 
);

uniform vec2 u_isTopLayer_hasCustomOptins;
uniform mat3 normal_matrix;
uniform float emission_factor;
uniform vec3 u_rangeType_isUnlit_topLayerIndex;
uniform vec4 u_isRangeView_isRangeVaild_topLayerOnly_viewType;
uniform vec2 u_pathDataRange;
uniform sampler2D s_color_range_texture;
uniform samplerBuffer s_per_move_data_texture;

in float moveId;
in vec3 frag_normal;
in vec3 frag_pos;

out vec4 frag_color;

bool is_top_layer()
{
    return u_isTopLayer_hasCustomOptins.x > 0.5;
}

vec4 extrusion_color(vec3 type_rangeData_deltaExtruder)
{
    vec4 final_color = vec4(0.25, 0.25, 0.25, 1.0); // Neutral_Color
    float path_data = type_rangeData_deltaExtruder.y;
    if (u_isRangeView_isRangeVaild_topLayerOnly_viewType.x > 0.5)
    {
        float view_type = u_isRangeView_isRangeVaild_topLayerOnly_viewType.w;
        if (view_type < 0.5 || (view_type > 8.5 && view_type < 9.5)) // Summer or ColorPrint
        {
            if (int(path_data + 0.5) > int(u_pathDataRange.y + 0.5))
            {
                final_color = vec4(0.5, 0.5, 0.5, 1.0);
                return final_color;
            }
        }
        else if (view_type > 9.5 && view_type < 10.5) // FilamentId
        {
            float path_data2 = type_rangeData_deltaExtruder.z;
            float id = path_data / (u_pathDataRange.y - u_pathDataRange.x);
            float role = path_data2 / (u_pathDataRange.y - u_pathDataRange.x);
            final_color = vec4(id, role, id, 1.0);
            return final_color;
        }
        // helio
        else if (view_type > 11.5 && view_type < 14.5)
        {
            if (path_data < u_pathDataRange.x - 0.01)
            {
                final_color = vec4(0.5, 0.5, 0.5, 1.0);
                return final_color;
            }
        }
        // end helio
        vec2 uv = vec2(0.0, 0.5);
        if (u_isRangeView_isRangeVaild_topLayerOnly_viewType.y > 0.5)
        {
            if (u_rangeType_isUnlit_topLayerIndex.x < 0.5)
            {
                uv.x = (path_data - u_pathDataRange.x) / (u_pathDataRange.y - u_pathDataRange.x);
            }
            else
            {
                uv.x = (log(path_data) - u_pathDataRange.x) / (u_pathDataRange.y - u_pathDataRange.x);
            }
            uv.x = clamp(uv.x, 0.0, 1.0);
        }
        final_color = texture(s_color_range_texture, uv);
    }
    return final_color;
}

vec4 get_base_color(vec3 type_rangeData_deltaExtruder)
{
    vec4 final_color = vec4(0.0, 0.0, 0.0, 1.0);
    float path_type = type_rangeData_deltaExtruder.x;

    if (path_type > 7.5 && path_type < 8.5) //EMoveType::Travel
    {
        bool is_top_layer_only = u_isRangeView_isRangeVaild_topLayerOnly_viewType.z > 0.5;
        if (!is_top_layer_only || is_top_layer())
        {
            float view_type = u_isRangeView_isRangeVaild_topLayerOnly_viewType.w;
            if ((view_type > 3.5 && view_type < 4.5) || (view_type > 7.5 && view_type < 8.5))
            {
                final_color = extrusion_color(type_rangeData_deltaExtruder);
            }
            else
            {
                float flag = sign(type_rangeData_deltaExtruder.z);
                final_color.rgb = KTravel_Colors[int(flag) + 1];
            }
        }
        else
        {
           final_color = vec4(0.25, 0.25, 0.25, 1.0); // Neutral_Color
        }
    }
    else if (path_type > 8.5 && path_type < 9.5)
    {
        final_color = vec4(1.0, 1.0, 0.0, 1.0); // Wipe_Color
    }
    else if (path_type > 9.5 && path_type < 10.5)
    {
        bool is_top_layer_only = u_isRangeView_isRangeVaild_topLayerOnly_viewType.z > 0.5;
        if (!is_top_layer_only || is_top_layer())
        {
            final_color = extrusion_color(type_rangeData_deltaExtruder);
        }
        else
        {
            final_color = vec4(0.25, 0.25, 0.25, 1.0); // Neutral_Color
        }
    }
    return final_color;
}

void main()
{
    vec4 custom_option_color = vec4(0.5, 0.5, 0.5, 1.0);

    vec3 type_rangeData_deltaExtruder = texelFetch(s_per_move_data_texture, int(moveId)).rgb;
    vec4 base_color = get_base_color(type_rangeData_deltaExtruder);

    vec4 mixed_color = mix(custom_option_color, base_color, u_isTopLayer_hasCustomOptins.y > 0.5 ? 0.0 : 1.0);
    // x = tainted, y = specular;
    vec2 intensity = vec2(0.0, 0.0);
    float t_emission_factor = 1.0;
    if (u_rangeType_isUnlit_topLayerIndex.y < 0.5)
    {
        vec3 norm = normal_matrix * normalize(frag_normal);
        norm = normalize(norm);
        // Compute the cos of the angle between the normal and lights direction. The light is directional so the direction is constant for every vertex.
        // Since these two are normalized the cosine is the dot product. We also need to clamp the result to the [0,1] range.
        float NdotL = max(dot(norm, LIGHT_TOP_DIR), 0.0);

        intensity.x = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;

        intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(frag_pos.xyz), reflect(-LIGHT_TOP_DIR, norm)), 0.0), LIGHT_TOP_SHININESS);

        // Perform the same lighting calculation for the 2nd light source (no specular applied).
        NdotL = max(dot(norm, LIGHT_FRONT_DIR), 0.0);
        intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;

        t_emission_factor = emission_factor;
    }

    frag_color = vec4(vec3(intensity.y) + mixed_color.rgb * (intensity.x + t_emission_factor), mixed_color.a);
}
