// AdaptivePAProcessor.hpp
// OrcaSlicer
//
// Header file for the AdaptivePAProcessor class, responsible for processing G-code layers for the purposes of applying adaptive pressure advance.

#ifndef ADAPTIVEPAPROCESSOR_H
#define ADAPTIVEPAPROCESSOR_H

#include <string>
#include <sstream>
#include <regex>
#include <memory>
#include <map>
#include <vector>
#include "AdaptivePAInterpolator.hpp"

namespace Slic3r {

// Forward declaration of GCode class
class GCode;

/**
 * @brief Class for processing G-code layers with adaptive pressure advance.
 */
class AdaptivePAProcessor {
public:
    /**
     * @brief Constructor for AdaptivePAProcessor.
     *
     * This constructor initializes the AdaptivePAProcessor with a reference to a GCode object.
     * It also initializes the configuration reference, pressure advance interpolation object,
     * and regular expression patterns used for processing the G-code.
     *
     * @param gcodegen A reference to the GCode object that generates the G-code.
     */
    AdaptivePAProcessor(GCode &gcodegen, const std::vector<unsigned int> &tools_used);
    
    /**
     * @brief Processes a layer of G-code and applies adaptive pressure advance.
     *
     * This method processes the G-code for a single layer, identifying the appropriate
     * pressure advance settings and applying them based on the current state and configurations.
     *
     * @param gcode A string containing the G-code for the layer.
     * @return A string containing the processed G-code with adaptive pressure advance applied.
     */
    std::string process_layer(std::string &&gcode);
    
    /**
     * @brief Manually sets adaptive PA internal value.
     *
     * This method manually sets the adaptive PA internally held value.
     * Call this when changing tools or in any other case where the internally assumed last PA value may be incorrect
     */
    void resetPreviousPA(double PA){ m_last_predicted_pa = PA; };
    
private:
    GCode &m_gcodegen; ///< Reference to the GCode object.
    std::unordered_map<unsigned int, std::unique_ptr<AdaptivePAInterpolator>> m_AdaptivePAInterpolators; ///< Map between Interpolator objects and tool ID's
    const PrintConfig &m_config; ///< Reference to the print configuration.
    double m_last_predicted_pa; ///< Last predicted pressure advance value.
    double m_max_next_feedrate; ///< Maximum feed rate (speed) for the upcomming island. If no speed is found, the previous island speed is used.
    double m_next_feedrate; ///< First feed rate (speed) for the upcomming island.
    double m_current_feedrate; ///< Current, latest feedrate.
    int m_last_extruder_id; ///< Last used extruder ID.

    std::regex m_pa_change_pattern; ///< Regular expression to detect PA_CHANGE pattern.
    std::regex m_g1_f_pattern; ///< Regular expression to detect G1 F pattern.
    std::smatch m_match; ///< Match results for regular expressions.

    /**
     * @brief Get the PA interpolator attached to the specified tool ID.
     *
     * This method manually sets the adaptive PA internally held value.
     * Call this when changing tools or in any other case where the internally assumed last PA value may be incorrect
     *
     * @param An integer with the tool ID for which the PA interpolation model is to be returned.
     * @return The Adaptive PA Interpolator object corresponding to that tool.
     */
    AdaptivePAInterpolator* getInterpolator(unsigned int tool_id);
};

} // namespace Slic3r

#endif // ADAPTIVEPAPROCESSOR_H
