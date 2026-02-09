#include "GCodeChecker.h"
#include <fstream>
#include <math.h>
#include <map>
#include <string.h>
namespace BambuStudio {

//BBS: only check wodth when dE is longer than this value
const double CHECK_WIDTH_E_THRESHOLD = 0.0025;
const double WIDTH_THRESHOLD = 0.05;
const double RADIUS_THRESHOLD = 0.005;
const double MULTI_NOZZLE_TEMP_THRESHOLD = 100;

const double filament_diameter = 1.75;
const double Pi = 3.14159265358979323846;

const std::string Extrusion_Role_Tag = " FEATURE: ";
const std::string Width_Tag          = " LINE_WIDTH: ";
const std::string Wipe_Start_Tag     = " WIPE_START";
const std::string Wipe_End_Tag       = " WIPE_END";
const std::string Wipe_Tower_Start_Tag = " WIPE_TOWER_START";
const std::string Wipe_Tower_End_Tag = " WIPE_TOWER_END";
const std::string Layer_Change_Tag   = " CHANGE_LAYER";
const std::string Height_Tag         = " LAYER_HEIGHT: ";
const std::string filament_flow_ratio_tag = " filament_flow_ratio";
const std::string has_scarf_joint_seam_tag = " has_scarf_joint_seam";
const std::string nozzle_temperature_Tag   = " nozzle_temperature =";
const std::string nozzle_temperature_initial_layer_Tag  = " nozzle_temperature_initial_layer";
const std::string Z_HEIGHT_TAG                         = " Z_HEIGHT: ";
const std::string Initial_Layer_Ptint_Height_Tag        = " initial_layer_print_height =";
const std::string Line_Width_Tag = " line_width =";
const std::string Filament_Map_Tag = " filament_map =";
const std::string Physical_Extruder_Map_Tag = " physical_extruder_map =";

GCodeCheckResult GCodeChecker::parse_file(const std::string& path)
{
    std::ifstream file(path);
    if (file.fail()) {
        std::cout << "Failed to open file " << path << std::endl;
        return GCodeCheckResult::ParseFailed;
    }
    std::string line_raw;
    std::string line;
    int line_number = 0; 


    while (std::getline(file, line_raw)) {
        line_number++;

        const char *c = line_raw.c_str();
        c = skip_whitespaces(c);
        if (std::toupper(*c) == 'N')
            c = skip_word(c);
        c = skip_whitespaces(c);
        line = c;
        if (parse_line(line) != GCodeCheckResult::Success) {
            std::cerr << "Failed to parse line " << line_number
                << ": " << line_raw << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
    }

    if (m_layer_num == 0) {
        std::cout << "Invalid gcode file without layer change comment" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }

    return GCodeCheckResult::Success;
}

bool GCodeChecker::include_chinese(const char* str)
{
   char c;
   while(1)
   {
       c=*str++;
       if (is_end_of_line(c))
           break;
       if ((c & 0x80) && (*str & 0x80))
           return true;
   }
   return false;
}

GCodeCheckResult GCodeChecker::parse_line(const std::string& line)
{
    // update start position
    m_start_position = m_end_position;

    GCodeCheckResult ret;
    const char *c = skip_whitespaces(line.c_str());
    if (include_chinese(c)) {
        //chinese is forbidden
        return GCodeCheckResult::ParseFailed;
    } if (is_end_of_line(*c)) {
        //BBS: skip empty line
        return GCodeCheckResult::Success;
    } else if (is_comment_line(*c)) {
        GCodeLine gcode_line;
        gcode_line.m_raw = c;
        ret = parse_comment(gcode_line);
        if (ret != GCodeCheckResult::Success)
            return ret;
    } else {
        GCodeLine gcode_line;
        gcode_line.m_raw = c;
        ret = parse_command(gcode_line);
        if (ret != GCodeCheckResult::Success)
            return ret;
        ret = check_line_width(gcode_line);
        if (ret != GCodeCheckResult::Success)
            return ret;
    }

    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::parse_comment(GCodeLine& line)
{
    const char *c = line.m_raw.c_str();
    c++;
    std::string comment = c;
    // extrusion role tag
    if (starts_with(comment, Extrusion_Role_Tag)) {
        m_role = string_to_role(comment.substr(Extrusion_Role_Tag.length()));
        check_gap_infill_width = false;

        double check_nozzle_temp = 0.0f;
        if (is_multi_nozzle == true) {
            check_nozzle_temp = multi_nozzle_temp[current_nozzle_id];
        }
        else
        {
            check_nozzle_temp = nozzle_temp;
        }

        if (m_role == erExternalPerimeter) {
            if (z_height != initial_layer_height) {
                if (is_multi_nozzle) {
                    double expected_temp = nozzle_temperature[filament_id];
                    if (std::abs(check_nozzle_temp - expected_temp) > MULTI_NOZZLE_TEMP_THRESHOLD) {
                        // for two nozzle gcode, remove temp check temply
                        //std::cout << "Multi-nozzle: Invalid filament nozzle temperature! Expected: "
                        //    << expected_temp
                        //    << ", but got: " << check_nozzle_temp
                        //    << " (Threshold: ±100)." << std::endl;
                        return GCodeCheckResult::Success;
                    }
                }
                else {
                    if (check_nozzle_temp != nozzle_temperature[filament_id]) {
                        std::cout << "Invalid filament nozzle temperature! Expected: "
                            << nozzle_temperature[filament_id]
                            << ", but got: " << check_nozzle_temp << "." << std::endl;
                        return GCodeCheckResult::ParseFailed;
                    }
                }
            }
        }
        else if (m_role == erGapFill) {
            check_gap_infill_width = true;
        }

    }else if (starts_with(comment, Wipe_Start_Tag)) {
        m_wiping = true;
    } else if (starts_with(comment, Wipe_End_Tag)) {
        m_wiping = false;
    }
    else if (starts_with(comment, Wipe_Tower_Start_Tag)) {
        is_wipe_tower = true;
    }
    else if (starts_with(comment, Wipe_Tower_End_Tag)) {
        is_wipe_tower = false;
    } else if (starts_with(comment, Height_Tag)) {
        std::string str = comment.substr(Height_Tag.size());
        if (!parse_double_from_str(str, m_height)) {
            std::cout << "invalid height comment with invalid value!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
    } else if (starts_with(comment, Width_Tag)) {
        std::string str = comment.substr(Width_Tag.size());
        if (!parse_double_from_str(str, m_width)) {
            std::cout << "invalid width comment with invalid value!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }

        //check gap infill line width
        if( check_gap_infill_width ) {
            if (m_width > max_gap_infill_width) {
                std::cout << "gap infill width has invalid value!" << std::endl;
                std::cout << "allowed max gap width: " << max_gap_infill_width << std::endl;
                return GCodeCheckResult::ParseFailed;
            }
        }
    } else if (starts_with(comment, Layer_Change_Tag)) {
        m_layer_num++;
    } else if (starts_with(comment, filament_flow_ratio_tag))
    {
        std::string str = comment.substr(filament_flow_ratio_tag.size()+3);
        if (!parse_double_from_str(str, filament_flow_ratio))
        {
            std::cout << "invalid filament flow ratio comment with invalid value!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
    }
    else if (starts_with(comment, has_scarf_joint_seam_tag))
    {
        std::string str = comment.substr(has_scarf_joint_seam_tag.size() + 3);
        has_scarf_joint_seam = (str == "1");
    }
    else if (starts_with(comment, nozzle_temperature_Tag)) {
        std::string str = comment.substr(nozzle_temperature_Tag.size() + 1);
        if (!parse_double_from_str(str, nozzle_temperature)) {
            std::cout << "invalid nozzle temperature comment with invalid value!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
    }
    else if (starts_with(comment, nozzle_temperature_initial_layer_Tag)) {
        std::string str = comment.substr(nozzle_temperature_initial_layer_Tag.size() + 3);
        if (!parse_double_from_str(str, nozzle_temperature_initial_layer)) {
            std::cout << "invalid nozzle temperature initial layer comment with invalid value!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
    }
    else if (starts_with(comment, Filament_Map_Tag)) {
        std::string str = comment.substr(Filament_Map_Tag.size() + 1);
        if (!parse_double_from_str(str, filament_map)) {
            std::cout << "invalid filament map comment with invalid value!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
        else {
            for (size_t i = 0; i < filament_map.size(); ++i) {
                filament_map[i] -= 1;
            }
            is_multi_nozzle = true;
        }
    }
    else if (starts_with(comment, Physical_Extruder_Map_Tag)) {
        std::string str = comment.substr(Physical_Extruder_Map_Tag.size() + 1);
        std::vector<double>tmp;

        if (!parse_double_from_str(str, tmp)){
            std::cout << "invalid physical extruder map comment with invalid value!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }

        for (size_t idx = 0; idx < tmp.size(); ++idx) {
            physical_to_logic_extruder_map[(int)(tmp[idx])]= idx;
            logic_to_physical_extruder_map[idx] = (int)(tmp[idx]);
        }
        
    }
    else if (starts_with(comment, Z_HEIGHT_TAG)) {
        std::string str = comment.substr(Z_HEIGHT_TAG.size());
        if (!parse_double_from_str(str, z_height)) {
            std::cout << "invalid z height comment with invalid value!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
    } else if (starts_with(comment, Initial_Layer_Ptint_Height_Tag)) {
        std::string str = comment.substr(Initial_Layer_Ptint_Height_Tag.size());
        if (!parse_double_from_str(str, initial_layer_height)) {
            std::cout << "invalid initial layer height comment with invalid value!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
    } else if (starts_with(comment, Line_Width_Tag)) {
        std::string str = comment.substr(Line_Width_Tag.size());
        double default_line_width = 0.0f;
        if (!parse_double_from_str(str, default_line_width)) {
            std::cout << "invalid initial layer height comment with invalid value!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
        max_gap_infill_width = 3 * default_line_width;
    }
    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::parse_command(GCodeLine& gcode_line)
{
    const std::string cmd = gcode_line.cmd();
    GCodeCheckResult ret = GCodeCheckResult::Success;
    switch (::toupper(cmd[0])) {
        case 'G':
        {
            switch (::atoi(&cmd[1]))
            {
                case 0:
                case 1:  { ret = parse_G0_G1(gcode_line); break; }  // Move
                case 2:
                case 3:  { ret = parse_G2_G3(gcode_line); break; }  // Move
                case 90: { ret = parse_G90(gcode_line); break; }    // Set to Absolute Positioning
                case 91: { ret = parse_G91(gcode_line); break; }    // Set to Relative Positioning
                case 92: { ret = parse_G92(gcode_line); break; }    // Set Position
                default: { break; }
            }
            break;
        }
        case 'M':{
            switch (::atoi(&cmd[1]))
            {
                case 82: { ret = parse_M82(gcode_line); break; }    // Set to Absolute extrusion
                case 83: { ret = parse_M83(gcode_line); break; }    // Set to Relative extrusion
                case 104: {
                    ret = parse_M104_M109(gcode_line);
                    break;
                } // Set to nozzle temperature
                case 109: {
                    ret = parse_M104_M109(gcode_line);
                    break;
                } // Set to nozzle temperature
                case 1020: {
                    ret = parse_M1020(gcode_line);
                    break;
                }
                default: { break; }
            }
            break;
        }
        case 'T':{

            int pt = ::atoi(&cmd[1]);
            if (pt == 1000 || pt == 1100 || pt == 255 || pt == 1001 || pt == 65535 || pt == 65279 || pt == -1) {
                break;
            }

            if (pt < 0 || pt > 254 || pt >= filament_flow_ratio.size()) {
                std::cout << "Invalid T command"<<std::endl;
                ret = GCodeCheckResult::ParseFailed;
                break;
            }
            filament_id = pt;

            
            if (is_multi_nozzle == true) {
                set_current_nozzle(pt);
            }
           
            flow_ratio = filament_flow_ratio[pt];
            break;
        }
        case 'S': {
            if (cmd.compare(0, 4, "SYNC") == 0) {
                // Valid SYNC command
            }
            else {
                // Invalid SYNC command
                ret = GCodeCheckResult::ParseFailed;
            }
            break;
        }
        default: {
            //BBS: other g command? impossible! must be invalid
            ret = GCodeCheckResult::ParseFailed;
            break;
        }
    }

    return ret;
}

GCodeCheckResult GCodeChecker::parse_axis(GCodeLine& gcode_line)
{
    const std::string cmd = gcode_line.m_raw;
    const char* c = cmd.c_str();
    c = skip_word(c);
    while (! is_end_of_gcode_line(*c)) {
        c = skip_whitespaces(c);
        if (is_end_of_gcode_line(*c))
            break;

        Axis axis = UNKNOWN_AXIS;
        switch (*c) {
        case 'X': axis = X; break;
        case 'Y': axis = Y; break;
        case 'Z': axis = Z; break;
        case 'E': axis = E; break;
        case 'F': axis = F; break;
        case 'I': axis = I; break;
        case 'J': axis = J; break;
        case 'P': axis = P; break;
        default:
            //BBS: invalid command which has invalid axis
            std::cout << "Invalid gcode because of invalid axis!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }

        char   *pend = nullptr;
        double v = strtod(++c, &pend);
        if (pend != nullptr && is_end_of_word(*pend) && !isnan(v) && !isinf(v)) {
	        gcode_line.m_axis[int(axis)] = v;
            if (gcode_line.m_mask & (1 << int(axis))) {
                //BBS: invalid command which has duplicated axis
                std::cout << "Invalid gcode because of duplicated axis!" << std::endl;
                return GCodeCheckResult::ParseFailed;
            } else {
                gcode_line.m_mask |= 1 << int(axis);
            }
            if (c == pend) {
                //BBS: invalid command which has invalid axis value
                std::cout << "Invalid gcode because of invalid axis value!" << std::endl;
                return GCodeCheckResult::ParseFailed;
            }
            c = pend;
        } else {
            //BBS: invalid command for invalid axis value
            std::cout << "Invalid gcode because of invalid axis value!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
    }

    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::parse_G0_G1(GCodeLine& gcode_line)
{
    if (parse_axis(gcode_line) != GCodeCheckResult::Success)
        return GCodeCheckResult::ParseFailed;

    //BBS: invalid G1 command which has no axis or invalid axis
    if ((!gcode_line.m_mask) ||
        gcode_line.has(I) ||
        gcode_line.has(J)) {
        std::cout << "Invalid G0_G1 gcode because of no axis or invalid axis!" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }

    //BBS: invalid G1 command which has zero speed
    if (gcode_line.has(F) && gcode_line.get(F) == 0.0) {
        std::cout << "Invalid G0_G1 gcode because has F axis but 0 speed!" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }

    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::parse_G2_G3(GCodeLine& gcode_line)
{
    if (parse_axis(gcode_line) != GCodeCheckResult::Success)
        return GCodeCheckResult::ParseFailed;

    //BBS: invalid G2_G3 command which has no axis or Z axis
    if (!gcode_line.m_mask) {
        std::cout << "Invalid G2_G3 gcode because of no axis or has Z axis!" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }
    //BBS: invalid G2_G3 command which has zero speed
    if (gcode_line.has(F) && gcode_line.get(F) == 0.0) {
        std::cout << "Invalid G2_G3 gcode because has F axis but 0 speed!" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }
    //BBS: invalid G2_G3 command which has no I and J axis
    if (!gcode_line.has(I) &&
        !gcode_line.has(J)) {
        std::cout << "Invalid G2_G3 gcode because of no I and J axis at same time!" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }
    //BBS: invalid G2_G3 command which has no X and Y axis at same time
    if (!gcode_line.has(X) && !gcode_line.has(Y) && !gcode_line.has(I) && !gcode_line.has(J)) {
        if (!gcode_line.has(X) || !gcode_line.has(P) || (int)gcode_line.get(P) != 1) {
            std::cout << "Invalid G2_G3 gcode because of no X and Y axis at same time!" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
    }

    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::parse_G90(const GCodeLine& gcode_line)
{
    const char* c = gcode_line.m_raw.c_str();
    //BBS: G90 is single command with no argument
    if (!is_single_gcode_word(c)) {
        std::cout << "Invalid G90 gcode with invalid end!" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }
    m_global_positioning_type = EPositioningType::Absolute;
    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::parse_G91(const GCodeLine& gcode_line)
{
    const char* c = gcode_line.m_raw.c_str();
    //BBS: G91 is single command with no argument
    if (!is_single_gcode_word(c)) {
        std::cout << "Invalid G91 gcode with invalid end!" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }
    m_global_positioning_type = EPositioningType::Relative;
    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::parse_G92(GCodeLine& gcode_line)
{
    if (parse_axis(gcode_line) != GCodeCheckResult::Success)
        return GCodeCheckResult::ParseFailed;

    //BBS: invalid G92 command which has no axis or invalid axis
    if (!gcode_line.m_mask ||
        gcode_line.has(F) ||
        gcode_line.has(I) ||
        gcode_line.has(J)) {
        std::cout << "Invalid G2_G3 gcode because of no axis or invalid axis!" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }

    bool any_found = false;
    if (gcode_line.has(X)){
        m_origin[X] = m_end_position[X] - gcode_line.get(X);
        any_found = true;
    }

    if (gcode_line.has(Y)){
        m_origin[Y] = m_end_position[Y] - gcode_line.get(Y);
        any_found = true;
    }

    if (gcode_line.has(Z)){
        m_origin[Z] = m_end_position[Z] - gcode_line.get(Z);
        any_found = true;
    }

    if (gcode_line.has(E)){
        m_end_position[E] = gcode_line.get(E);
        any_found = true;
    }

    if (!any_found) {
        for (unsigned char a = X; a <= E; ++a) {
            m_origin[a] = m_end_position[a];
        }
    }

    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::parse_M82(const GCodeLine& gcode_line)
{
    const char* c = gcode_line.m_raw.c_str();
    //BBS: M82 is single command with no argument
    if (!is_single_gcode_word(c)) {
        std::cout << "Invalid M82 gcode with invalid end!" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }
    m_e_local_positioning_type = EPositioningType::Absolute;
    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::parse_M83(const GCodeLine& gcode_line)
{
    const char* c = gcode_line.m_raw.c_str();
    //BBS: M83 is single command with no argument
    if (!is_single_gcode_word(c)) {
        std::cout << "Invalid M83 gcode with invalid end!" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }
    m_e_local_positioning_type = EPositioningType::Relative;
    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::parse_M104_M109(const GCodeLine& gcode_line)
{
    const char* c = gcode_line.m_raw.c_str();
    const char* rs = strchr(c, 'S');
    if (!rs) {
        std::cout << "No S parameter found in M104/M109 command!" << std::endl;
        return GCodeCheckResult::Success;
    }

    std::string strS = rs + 1;
    size_t pos = strS.find_first_of(" ;");
    if (pos != std::string::npos)
        strS = strS.substr(0, pos);

    double temp_nozzle_temp;
    if (!parse_double_from_str(strS, temp_nozzle_temp)) {
        std::cout << "Invalid nozzle temperature: " << strS << std::endl;
        return GCodeCheckResult::ParseFailed;
    }

    if (is_multi_nozzle == true) {
        const char* rt = strchr(c, 'T');
        if (rt) {
            std::string strT = rt + 1;
            size_t posT = strT.find_first_of(" ;");
            if (posT != std::string::npos)
                strT = strT.substr(0, posT);
            int logic_nozzle_id = physical_to_logic_extruder_map[std::stoi(strT)];
            multi_nozzle_temp[logic_nozzle_id] = temp_nozzle_temp;
        }
        else {
            multi_nozzle_temp[current_nozzle_id] = temp_nozzle_temp;
        }
    }
    else {
        nozzle_temp = temp_nozzle_temp;
    }

    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::parse_M1020(const GCodeLine& gcode_line)
{
    const char* c = gcode_line.m_raw.c_str();
    const char* rs = strchr(c, 'S');

    if (rs != nullptr) {
        std::string str = rs;
        str = str.substr(1);
        for (int i = 0; i < str.size(); i++) {
            if (str[i] == ' ')
                str = str.substr(0, i);
        }

        try {
            int value = std::stoi(str);
            if (value >= 0 && value <= filament_flow_ratio.size() - 1) {
                filament_id = value;
                flow_ratio = filament_flow_ratio[value];
                return GCodeCheckResult::Success;
            }
            else {
                return GCodeCheckResult::ParseFailed;
            }
        }
        catch (std::invalid_argument&) {
            std::cout << "Invalid argument: not a valid integer" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
        catch (std::out_of_range&) {
            std::cout << "Out of range: number is too large" << std::endl;
            return GCodeCheckResult::ParseFailed;
        }
    }
    else
    {
        std::cout << "Missing 'S' character in the G-code line!" << std::endl;
        return GCodeCheckResult::ParseFailed;
    }
}

double GCodeChecker::calculate_G1_width(const std::array<double, 3>& source,
                                       const std::array<double, 3>& target,
                                       double e, double height, bool is_bridge) const
{
    double volume = (e / flow_ratio) * Pi * (filament_diameter / 2.0f) * (filament_diameter / 2.0f);
    std::array<double, 3> delta = { target[0] - source[0],
                                   target[1] - source[1],
                                   target[2] - source[2] };
    double length = sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    double mm3_per_mm = volume / length;
    return is_bridge? 2 * sqrt(mm3_per_mm/Pi) :
           (mm3_per_mm / height) + height * (1 - 0.25 * Pi);
}

double GCodeChecker::calculate_G2_G3_width(const std::array<double, 2>& source,
                                          const std::array<double, 2>& target,
                                          const std::array<double, 2>& center,
                                          bool is_ccw, double e, double height,
                                          bool is_bridge) const
{
    std::array<double, 2> v1 = { source[0] - center[0], source[1] - center[1] };
    std::array<double, 2> v2 = { target[0] - center[0], target[1] - center[1] };

    double dot = v1[0] * v2[0] + v1[1] * v2[1];
    double cross = v1[0] * v2[1] - v1[1] * v2[0];
    double radian = atan2(cross, dot);
    radian = is_ccw ?
        (radian < 0 ? 2 * Pi + radian : radian) :
        (radian < 0 ? -radian : 2 * Pi - radian);
    double radius = sqrt(v1[0] * v1[0] + v1[1] * v1[1]);
    double length = radius * radian;
    double volume = (e / flow_ratio) * Pi * (filament_diameter / 2) * (filament_diameter / 2);
    double mm3_per_mm = volume / length;

    return is_bridge? 2 * sqrt(mm3_per_mm/Pi) :
           (mm3_per_mm / height) + height * (1 - 0.25 * Pi);
}

GCodeCheckResult GCodeChecker::check_line_width(const GCodeLine& gcode_line)
{
    //BBS: don't need to check extrusion before first layer
    if (m_layer_num <= 0) {
        return GCodeCheckResult::Success;
    }

    GCodeCheckResult ret = GCodeCheckResult::Success;
    //BBS: only need to handle G0 G1 G2 G3
    const std::string cmd = gcode_line.cmd();
    int cmd_id = ::atoi(&cmd[1]);
    if (::toupper(cmd[0]) == 'G')
        switch (::atoi(&cmd[1]))
        {
            case 0:
            case 1:  { ret = check_G0_G1_width(gcode_line); break; }
            case 2:
            case 3:  { ret = check_G2_G3_width(gcode_line); break; }
            default: { break; }
        }

    return ret;
}

GCodeCheckResult GCodeChecker::check_G0_G1_width(const GCodeLine& line)
{
    auto absolute_position = [this](Axis axis, const GCodeLine& lineG1) {
        bool is_relative = (m_global_positioning_type == EPositioningType::Relative);
        if (axis == E)
            is_relative |= (m_e_local_positioning_type == EPositioningType::Relative);

        if (lineG1.has(Axis(axis))) {
            double ret = lineG1.get(Axis(axis));
            return is_relative ? m_start_position[axis] + ret : m_origin[axis] + ret;
        } else
            return m_start_position[axis];
    };

    auto move_type = [this](const std::array<double, 4>& delta_pos) {
        EMoveType type = EMoveType::Noop;

        if (m_wiping)
            type = EMoveType::Wipe;
        else if (delta_pos[E] < 0.0f)
            type = (delta_pos[X] != 0.0f || delta_pos[Y] != 0.0f || delta_pos[Z] != 0.0f) ? EMoveType::Travel : EMoveType::Retract;
        else if (delta_pos[E] > 0.0f) {
            if (delta_pos[X] == 0.0f && delta_pos[Y] == 0.0f)
                type = (delta_pos[Z] == 0.0f) ? EMoveType::Unretract : EMoveType::Travel;
            else if (delta_pos[X] != 0.0f || delta_pos[Y] != 0.0f)
                type = EMoveType::Extrude;
        } 
        else if (delta_pos[X] != 0.0f || delta_pos[Y] != 0.0f || delta_pos[Z] != 0.0f)
            type = EMoveType::Travel;

        return type;
    };
    
    for (unsigned char a = X; a <= E; ++a) {
        m_end_position[a] = absolute_position((Axis)a, line);
    }

    // calculates movement deltas
    std::array<double, 4> delta_pos;
    for (unsigned char a = X; a <= E; ++a)
        delta_pos[a] = m_end_position[a] - m_start_position[a];

    // Todo: currently, for precision, there alwasy has possible to generate
    // such gcode because of decimal truncation
    /*if (line.has(Axis(E)) && delta_pos[E] == 0.0 && !m_wiping) {
        std::cout << "Invalid GCode because has E axis but 0 extrusion" << std::endl;
        return GCodeCheckResult::CheckFailed;
    }*/

    EMoveType type = move_type(delta_pos);
    if (type == EMoveType::Extrude && m_end_position[Z] == 0.0f)
        type = EMoveType::Travel;

    //BBS: calculate line width and compare.
    //Don't need to check gap fill which has verious width
    if (type == EMoveType::Extrude &&
        m_role != erGapFill &&
        delta_pos[E] > CHECK_WIDTH_E_THRESHOLD) {
        std::array<double, 3> source = { m_start_position[X], m_start_position[Y], m_start_position[Z] };
        std::array<double, 3> target = { m_end_position[X], m_end_position[Y], m_end_position[Z] };

        bool is_bridge = m_role == erOverhangPerimeter || m_role == erBridgeInfill;
        if (!is_bridge && !is_wipe_tower) {
            double real_height = m_height;
            if (line.has(Z) && has_scarf_joint_seam && line.get(Z) != 0)
            {
                if (line.get(Z) == z_height)
                {
                    return GCodeCheckResult::Success;
                }
                if (line.get(Z) && line.get(E))
                {
                    return GCodeCheckResult::Success;
                }
                real_height = line.get(Z) - (z_height - m_height);
            }
            double width_real = calculate_G1_width(source, target, delta_pos[E], real_height, is_bridge);
            if (fabs(width_real - m_width) > WIDTH_THRESHOLD) {
                std::cout << "Invalid G0_G1 because has abnormal line width." << std::endl;
                std::cout << "Width: " << m_width << " Width_real: " << width_real << std::endl;
                return GCodeCheckResult::CheckFailed;
            }
        }

    }

    return GCodeCheckResult::Success;
}

GCodeCheckResult GCodeChecker::check_G2_G3_width(const GCodeLine& line)
{
    auto absolute_position = [this](Axis axis, const GCodeLine& lineG2_3) {
        bool is_relative = (m_global_positioning_type == EPositioningType::Relative);
        if (axis == E)
            is_relative |= (m_e_local_positioning_type == EPositioningType::Relative);

        if (lineG2_3.has(Axis(axis))) {
            double ret = lineG2_3.get(Axis(axis));
            if (axis == I)
                return m_start_position[X] + ret;
            else if (axis == J)
                return m_start_position[Y] + ret;
            else
                return is_relative ? m_start_position[axis] + ret : m_origin[axis] + ret;
        } else {
            if (axis == I)
                return m_start_position[X];
            else if (axis == J)
                return m_start_position[Y];
            else
                return m_start_position[axis];
        }
    };

     auto move_type = [this](const double& delta_E) {
        EMoveType type = EMoveType::Noop;

        if (m_wiping)
            type = EMoveType::Wipe;
        else if (delta_E < 0.0f || delta_E == 0.0f)
            type = EMoveType::Travel;
        else
            type = EMoveType::Extrude;

        return type;
    };

    for (unsigned char a = X; a <= E; ++a) {
        m_end_position[a] = absolute_position((Axis)a, line);
    }
    std::array<double, 2> source = { m_start_position[X], m_start_position[Y] };
    std::array<double, 2> target = { m_end_position[X], m_end_position[Y] };
    std::array<double, 2> center = { absolute_position(I, line),absolute_position(J, line) };
    const std::string& cmd = line.cmd();
    bool is_ccw = (::atoi(&cmd[1]) == 2) ? false : true;
    double delta_e = m_end_position[E] - m_start_position[E];
    EMoveType type = move_type(delta_e);

    //BBS: judge whether is normal arc by radius
    double radius1 = sqrt(pow((source[0] - center[0]), 2) + pow((source[1] - center[1]), 2));
    double radius2 = sqrt(pow((target[0] - center[0]), 2) + pow((target[1] - center[1]), 2));
    if (fabs(radius2 - radius1) > RADIUS_THRESHOLD) {
        std::cout << "Invalid G2_G3 because of abnormal radius." << std::endl;
        std::cout << "radius1: " << radius1 << " radius2: " << radius2 <<  std::endl;
        return GCodeCheckResult::CheckFailed;
    }

    //BBS: calculate line width and compare
    //Don't need to check gap fill which has verious width
    if (type == EMoveType::Extrude &&
        m_role != erGapFill &&
        delta_e > CHECK_WIDTH_E_THRESHOLD) {
        bool is_bridge = m_role == erOverhangPerimeter || m_role == erBridgeInfill;

        if (!is_bridge) {
            double width_real = calculate_G2_G3_width(source, target, center, is_ccw, delta_e, m_height, is_bridge);
            if (fabs(width_real - m_width) > WIDTH_THRESHOLD) {
                std::cout << "Invalid G2_G3 because has abnormal line width." << std::endl;
                std::cout << "Width: " << m_width << " Width_real: " << width_real << std::endl;
                return GCodeCheckResult::CheckFailed;
            }
        }

    }

    return GCodeCheckResult::Success;
}

void GCodeChecker::set_current_nozzle(int filament_id) {    
    if (filament_id >= 0 && filament_id < static_cast<int>(filament_map.size())) {
        current_nozzle_id = filament_map[filament_id];
    }
    else {
        std::cerr << "Error: filament_id is out of range!" << std::endl;
        current_nozzle_id = 0;
    }
}

const std::map<std::string, ExtrusionRole> string_to_role_map = {
    { "Inner wall",                 erPerimeter },
    { "Outer wall",                 erExternalPerimeter },
    { "Overhang wall",              erOverhangPerimeter },
    { "Sparse infill",              erInternalInfill },
    { "Internal solid infill",      erSolidInfill },
    { "Top surface",                erTopSolidInfill },
    { "Bottom surface",             erBottomSurface },
    { "Ironing",                    erIroning },
    { "Bridge",                     erBridgeInfill },
    { "Gap infill",                 erGapFill },
    { "Skirt",                      erSkirt },
    { "Brim",                       erBrim },
    { "Support",                    erSupportMaterial },
    { "Support interface",          erSupportMaterialInterface },
    { "Support transition",         erSupportTransition },
    { "Prime tower",                erWipeTower },
    { "Custom",                     erCustom },
    { "Mixed",                      erMixed }
};

ExtrusionRole GCodeChecker::string_to_role(const std::string &role)
{
    for (auto it = string_to_role_map.begin(); it != string_to_role_map.end(); it++) {
        if (role == it->first)
            return it->second;
    }
    return erNone;
}

}


