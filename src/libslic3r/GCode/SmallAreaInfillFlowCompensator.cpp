// Modify the flow of extrusion lines inversely proportional to the length of
// the extrusion line. When infill lines get shorter the flow rate will auto-
// matically be reduced to mitigate the effect of small infill areas being
// over-extruded.

// Based on original work by Alexander Þór licensed under the GPLv3:
// https://github.com/Alexander-T-Moss/Small-Area-Flow-Comp

#include <cmath>
#include <limits>
#include <exception>
#include <math.h>
#include <cstring>
#include <cfloat>
#include <ostream>
#include <memory>
#include <regex>

#include "../libslic3r.h"
#include "../PrintConfig.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/PchipInterpolatorHelper.hpp"
#include "libslic3r/ExtrusionEntity.hpp"

#include "SmallAreaInfillFlowCompensator.hpp"
#include "SmallAreaInfillFlowCompensationModel.hpp"
#include <boost/log/trivial.hpp>
#include <sstream>
#include <string>

namespace Slic3r {

SmallAreaInfillFlowCompensator::SmallAreaInfillFlowCompensator(const Slic3r::GCodeConfig& config)
{
    try {
        for (auto& line : config.small_area_infill_flow_compensation_model.values) {
            std::istringstream iss(line);
            std::string        value_str;
            double             eLength = 0.0;

            if (std::getline(iss, value_str, ',')) {
                try {
                    // Trim leading and trailing whitespace
                    value_str = std::regex_replace(value_str, std::regex("^\\s+|\\s+$"), "");
                    if (value_str.empty()) {
                        continue;
                    }
                    eLength = std::stod(value_str);
                    if (std::getline(iss, value_str, ',')) {
                        eLengths.push_back(eLength);
                        flowComps.push_back(std::stod(value_str));
                    }
                } catch (...) {
                    std::stringstream ss;
                    ss << "Small Area Flow Compensation: Error parsing data point in small area infill compensation model:" << line << std::endl;

                    throw Slic3r::InvalidArgument(ss.str());
                }
            }
        }

        // The editor and the G-code consumer must accept the same model constraints.
        int error_row;
        const SmallAreaInfillFlowCompensationModel model;
        if (const char* error = model.validate_points(eLengths, flowComps, error_row))
            throw Slic3r::InvalidArgument(std::string("Small Area Flow Compensation: ") + error);

        flowModel = std::make_unique<PchipInterpolatorHelper>(eLengths, flowComps);

    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Error parsing small area infill compensation model: " << e.what();
        throw;
    }
}

SmallAreaInfillFlowCompensator::~SmallAreaInfillFlowCompensator() = default;

double SmallAreaInfillFlowCompensator::flow_comp_model(const double line_length)
{
    if(flowModel == nullptr)
        return 1.0;

    if (line_length == 0 || line_length > max_modified_length()) {
        return 1.0;
    }

    return flowModel->interpolate(line_length);
}

double SmallAreaInfillFlowCompensator::modify_flow(const double line_length, const double dE, const ExtrusionRole role)
{
    if (flowModel &&
        (role == ExtrusionRole::erSolidInfill || role == ExtrusionRole::erTopSolidInfill || role == ExtrusionRole::erBottomSurface)) {
        return dE * flow_comp_model(line_length);
    }

    return dE;
}

} // namespace Slic3r
