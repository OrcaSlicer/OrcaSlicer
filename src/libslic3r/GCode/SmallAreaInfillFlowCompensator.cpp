// Modify the flow of extrusion lines inversely proportional to the length of
// the extrusion line. When infill lines get shorter the flow rate will auto-
// matically be reduced to mitigate the effect of small infill areas being
// over-extruded.

// Based on original work by Alexander Þór licensed under the GPLv3:
// https://github.com/Alexander-T-Moss/Small-Area-Flow-Comp

#include <math.h>
#include <cstring>
#include <cfloat>
#include <regex>

#include "../libslic3r.h"
#include "../PrintConfig.hpp"

#include "SmallAreaInfillFlowCompensator.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r {

bool nearly_equal(double a, double b)
{
    return std::nextafter(a, std::numeric_limits<double>::lowest()) <= b && std::nextafter(a, std::numeric_limits<double>::max()) >= b;
}

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

        for (size_t i = 0; i < eLengths.size(); i++) {
            if (i == 0) {
                if (!nearly_equal(eLengths[i], 0.0)) {
                    throw Slic3r::InvalidArgument("Small Area Flow Compensation: First extrusion length for small area infill compensation model must be 0");
                }
            } else {
                if (nearly_equal(eLengths[i], 0.0)) {
                    throw Slic3r::InvalidArgument("Small Area Flow Compensation: Only the first extrusion length for small area infill compensation model can be 0");
                }
                if (eLengths[i] <= eLengths[i - 1]) {
                    throw Slic3r::InvalidArgument("Small Area Flow Compensation: Extrusion lengths for subsequent points must be increasing");
                }
            }
        }

        for (size_t i = 1; i < flowComps.size(); ++i) {
            if (flowComps[i] <= flowComps[i - 1]) {
                throw Slic3r::InvalidArgument("Small Area Flow Compensation: Flow compensation factors must strictly increase with extrusion length");
            }
        }

        if (!flowComps.empty() && !nearly_equal(flowComps.back(), 1.0)) {
            throw Slic3r::InvalidArgument("Small Area Flow Compensation: Final compensation factor for small area infill flow compensation model must be 1.0");
        }

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
