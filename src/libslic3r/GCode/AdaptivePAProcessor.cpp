// AdaptivePAProcessor.cpp
// OrcaSlicer
//
// Implementation of the AdaptivePAProcessor class, responsible for processing G-code layers with adaptive pressure advance.

#include "../GCode.hpp"
#include "AdaptivePAProcessor.hpp"
#include <sstream>
#include <iostream>
#include <cmath>

namespace Slic3r {

/**
 * @brief Constructor for AdaptivePAProcessor.
 *
 * This constructor initializes the AdaptivePAProcessor with a reference to a GCode object.
 * It also initializes the configuration reference, pressure advance interpolation object,
 * and regular expression patterns used for processing the G-code.
 *
 * @param gcodegen A reference to the GCode object that generates the G-code.
 */
AdaptivePAProcessor::AdaptivePAProcessor(GCode &gcodegen, const std::vector<unsigned int> &tools_used)
    : m_gcodegen(gcodegen),
      m_config(gcodegen.config()),
      m_last_predicted_pa(0.0),
      m_max_next_feedrate(0.0),
      m_next_feedrate(0.0),
      m_current_feedrate(0.0),
      m_last_extruder_id(-1),
      m_pa_change_pattern(R"(; PA_CHANGE:T(\d+) MM3MM:([0-9]*\.[0-9]+) ACCEL:(\d+) BR:(\d+) RC:(\d+) OV:(\d+))"),
      m_g1_f_pattern(R"(G1 F([0-9]+))")
{
    // Constructor body can be used for further initialization if necessary
    for (unsigned int tool : tools_used) {
        // Only enable model for the tool if both PA and adaptive PA options are enabled
        if(m_config.adaptive_pressure_advance.get_at(tool) && m_config.enable_pressure_advance.get_at(tool)){
            auto interpolator = std::make_unique<AdaptivePAInterpolator>();
            // Get calibration values from extruder
            std::string pa_calibration_values = m_config.adaptive_pressure_advance_model.get_at(tool);
            // Setup the model and store it in the tool-interpolation model map
            interpolator->parseAndSetData(pa_calibration_values);
            m_AdaptivePAInterpolators[tool] = std::move(interpolator);
        }
    }
}

// Method to get the interpolator for a specific tool ID
AdaptivePAInterpolator* AdaptivePAProcessor::getInterpolator(unsigned int tool_id) {
    auto it = m_AdaptivePAInterpolators.find(tool_id);
    if (it != m_AdaptivePAInterpolators.end()) {
        return it->second.get();
    }
    return nullptr;  // Handle the case where the tool_id was not found
}

/**
 * @brief Processes a layer of G-code and applies adaptive pressure advance.
 *
 * This method processes the G-code for a single layer, identifying the appropriate
 * pressure advance settings and applying them based on the current state and configurations.
 *
 * @param gcode A string containing the G-code for the layer.
 * @return A string containing the processed G-code with adaptive pressure advance applied.
 */
std::string AdaptivePAProcessor::process_layer(std::string &&gcode) {
    std::istringstream stream(gcode);
    std::string line;
    std::ostringstream output;
    double mm3mm_value = 0.0;
    unsigned int accel_value = 0;
    std::string pa_change_line;
    bool wipe_command = false;

    // Iterate through each line of the layer G-code
    while (std::getline(stream, line)) {
        
        // If a wipe start command is found, ignore all speed changes till the wipe end part is found
        if (line.find("WIPE_START") != std::string::npos) {
            wipe_command = true;
        }
                
        // Update current feed rate (this is preceding an extrude or wipe command only). Ignore any speed changes that are emitted during a wipe move.
        // Travel feedrate is output as part of a G1 X Y (Z) F command
        if ( (line.find("G1 F") == 0) && (!wipe_command) ) { // prune lines quickly before running pattern matching
            std::size_t pos = line.find('F');
            if (pos != std::string::npos){
                m_current_feedrate = std::stod(line.substr(pos + 1)) / 60.0; // Convert from mm/min to mm/s
            }
        }
        
        // Wipe end found, continue searching for current feed rate.
        if (line.find("WIPE_END") != std::string::npos) {
            wipe_command = false;
        }
        
        // Reset next feedrate to zero enable searching for the first encountered
        // feedrate change command after the PA change tag.
        m_next_feedrate = 0;
        
        // Check for PA_CHANGE pattern in the line
        // We will only find this pattern for extruders where adaptive PA is enabled.
        // If there is mixed extruders in the layer (i.e. with adaptive PA on and off
        // this will only update the extruders where the adaptive PA is enabled
        // as these are the only ones where the PA pattern is output
        // For a mixed extruder layer with both adaptive PA enabled and disabled when the new tool is selected
        // the PA for that material is set. As no tag below will be found for this extruder, the original PA is retained.
        if (line.find("; PA_CHANGE") == 0) { // prune lines quickly before running regex check as regex is more expensive to run
            if (std::regex_search(line, m_match, m_pa_change_pattern)) {
                int extruder_id = std::stoi(m_match[1].str());
                mm3mm_value = std::stod(m_match[2].str());
                accel_value = std::stod(m_match[3].str());
                int isBridge = std::stoi(m_match[4].str());
                int roleChange = std::stoi(m_match[5].str());
                int isOverhang = std::stoi(m_match[6].str());
                
                // Check if the extruder ID has changed
                bool extruder_changed = (extruder_id != m_last_extruder_id);
                m_last_extruder_id = extruder_id;
                
                // Save the PA_CHANGE line to output later after finding feedrate
                pa_change_line = line;
                
                // Look ahead for feedrate before any line containing both G and E commands
                std::streampos current_pos = stream.tellg();
                std::string next_line;
                double temp_feed_rate = 0;
                bool extrude_move_found = false;
                int line_counter = 0;
                
                // Carry on searching on the layer gcode lines to find the print speed
                // If a G1 Fxxxx pattern is found, the new speed is identified
                // Carry on searching for feedrates to find the maximum print speed
                // until a feature change pattern or a wipe command is detected
                while (std::getline(stream, next_line)) {
                    line_counter++;
                    // Found an extrude move, set extrude move found flag and move to the next line
                    if ((!extrude_move_found) && next_line.find("G1 ") == 0 &&
                        next_line.find('X') != std::string::npos &&
                        next_line.find('Y') != std::string::npos &&
                        next_line.find('E') != std::string::npos) {
                        // Pattern matched, break the loop
                        extrude_move_found = true;
                        continue;
                    }
                    
                    // Found a travel move after we've found at least one extrude move
                    // We now need to stop searching for speeds as we're done printing this island
                    if (next_line.find("G1 ") == 0 &&
                        next_line.find('X') != std::string::npos && // X is present
                        next_line.find('Y') != std::string::npos && // Y is present
                        next_line.find('E') == std::string::npos && // no "E" present
                        extrude_move_found) {                       // An extrude move has happened already
                        // First travel move after extrude move found. Stop searching
                        break;
                    }
                    
                    // Found a WIPE command
                    // If we have a wipe command, usually the wipe speed is different (larger) than the max print speed
                    // for that feature. So stop searching if a wipe command is found because we do not want to overwrite the
                    // speed used for PA calculation by the Wipe speed.
                    if (next_line.find("WIPE") != std::string::npos) {
                        break; // Stop searching if wipe command is found
                    }
                    
                    // Found another PA_CHANGE pattern
                    // If RC = 1, it means we have a role change, so stop trying to find the max speed for the feature.
                    // This is possibly redundant as a new feature would always have a travel move preceding it
                    // but check anyway. However check last so to not invoke it without reason...
                    if (next_line.find("; PA_CHANGE") == 0) { // prune lines quickly before running pattern matching
                        std::size_t rc_pos = next_line.rfind("RC:");
                        if (rc_pos != std::string::npos) {
                            int rc_value = std::stoi(next_line.substr(rc_pos + 3));
                            if (rc_value == 1) {
                                break; // Role change found, stop searching
                            }
                        }
                    }
                    
                    // Found a Feedrate change command
                    // If the new feedrate is greater than any feedrate encountered so far after the PA change command, use that to calculate the PA value
                    // Also if this is the first feedrate we encounter, store it as the next feedrate.
                    if (next_line.find("G1 F") == 0) { // prune lines quickly before running pattern matching
                        std::size_t pos = next_line.find('F');
                        if (pos != std::string::npos) {
                            double feedrate = std::stod(next_line.substr(pos + 1)) / 60.0; // Convert from mm/min to mm/s
                            if(line_counter==1){ // this is the first command after the PA change pattern, and hence before any extrusion has happened. Reset
                                                // the current speed to this one
                                m_current_feedrate = feedrate;
                            }
                            if (temp_feed_rate < feedrate) {
                                temp_feed_rate = feedrate;
                            }
                            if(m_next_feedrate < EPSILON){ // This the first feedrate found after the PA Change command
                                m_next_feedrate = feedrate;
                            }
                        }
                        continue;
                    }
                }
                
                // If we found a new maximum feedrate after the PA change command, use it
                if (temp_feed_rate > 0) {
                    m_max_next_feedrate = temp_feed_rate;
                } else // If we didnt find a new feedrate at all after the PA change command, use the current feedrate.
                    m_max_next_feedrate = m_current_feedrate;
                
                // Restore stream position
                stream.clear();
                stream.seekg(current_pos);
                
                // Calculate the predicted PA using the upcomming feature maximum feedrate
                // Get the interpolator for the active tool
                AdaptivePAInterpolator* interpolator = getInterpolator(m_last_extruder_id);
                
                double predicted_pa = 0;
                double adaptive_PA_speed = 0;
            
                if(!interpolator){ // Tool not found in the interpolator map
                    // Tool not found in the PA interpolator to tool map
                    predicted_pa = m_config.enable_pressure_advance.get_at(m_last_extruder_id) ? m_config.pressure_advance.get_at(m_last_extruder_id) : 0;
                    if(m_config.gcode_comments) output << "; APA: Tool doesnt have APA enabled\n";
                } else if (!interpolator->isInitialised() || (!m_config.adaptive_pressure_advance.get_at(m_last_extruder_id)) )
                    // Check if the model is not initialised by the constructor for the active extruder
                    // Also check that adaptive PA is enabled for that extruder. This should not be needed
                    // as the PA change flag should not be set upstream (in the GCode.cpp file) if adaptive PA is disabled
                    // however check for robustness sake.
                {
                    // Model failed or adaptive pressure advance not enabled - use default value from m_config
                    predicted_pa = m_config.enable_pressure_advance.get_at(m_last_extruder_id) ? m_config.pressure_advance.get_at(m_last_extruder_id) : 0;
                    if(m_config.gcode_comments) output << "; APA: Interpolator setup failed, using default pressure advance\n";
                } else { // Model setup succeeded
                    // Proceed to identify the print speed to use to calculate the adaptive PA value
                    if(isOverhang > 0){  // If we are in an overhang area, use the minimum between current print speed
                                        // and any speed immediately after
                                        // In most cases the current speed is the minimum one;
                                        // however if slowdown for layer cooling is enabled, the overhang
                                        // may be slowed down more than the current speed.
                        adaptive_PA_speed = (m_current_feedrate == 0 || m_next_feedrate == 0) ?
                                                std::max(m_current_feedrate, m_next_feedrate) :
                                                std::min(m_current_feedrate, m_next_feedrate);
                    }else{                // If this is not an overhang area, use the maximum speed from the current and
                                          // upcomming speeds for the island.
                        adaptive_PA_speed = std::max(m_max_next_feedrate,m_current_feedrate);
                    }
                    
                    // Calculate the adaptive PA value
                    predicted_pa = (*interpolator)(mm3mm_value * adaptive_PA_speed, accel_value);
                    
                    // This is a bridge, use the dedicated PA setting.
                    if(isBridge && m_config.adaptive_pressure_advance_bridges.get_at(m_last_extruder_id) > EPSILON)
                        predicted_pa = m_config.adaptive_pressure_advance_bridges.get_at(m_last_extruder_id);
                    
                    if (predicted_pa < 0) { // If extrapolation fails, fall back to the default PA for the extruder.
                        predicted_pa = m_config.enable_pressure_advance.get_at(m_last_extruder_id) ? m_config.pressure_advance.get_at(m_last_extruder_id) : 0;
                        if(m_config.gcode_comments) output << "; APA: Interpolation failed, using fallback pressure advance value\n";
                    }
                }
                if(m_config.gcode_comments) {
                    // Output debug GCode comments
                    output << pa_change_line << '\n'; // Output PA change command tag
                    if(isBridge && m_config.adaptive_pressure_advance_bridges.get_at(m_last_extruder_id) > EPSILON)
                        output << "; APA Model Override (bridge)\n";
                    output << "; APA Current Speed: " << std::to_string(m_current_feedrate) << "\n";
                    output << "; APA Next Speed: " << std::to_string(m_next_feedrate) << "\n";
                    output << "; APA Max Next Speed: " << std::to_string(m_max_next_feedrate) << "\n";
                    output << "; APA Speed Used: " << std::to_string(adaptive_PA_speed) << "\n";
                    output << "; APA Flow rate: " << std::to_string(mm3mm_value * m_max_next_feedrate) << "\n";
                    output << "; APA Prev PA: " << std::to_string(m_last_predicted_pa) << " New PA: " << std::to_string(predicted_pa) << "\n"; 
                }
                if (extruder_changed || std::fabs(predicted_pa - m_last_predicted_pa) > EPSILON) {
                    output << m_gcodegen.writer().set_pressure_advance(predicted_pa); // Use m_writer to set pressure advance
                    m_last_predicted_pa = predicted_pa; // Update the last predicted PA value
                }
            }
        }else {
            // Output the current line as this isn't a PA change tag
            output << line << '\n';
        }
    }

    return output.str();
}

} // namespace Slic3r
