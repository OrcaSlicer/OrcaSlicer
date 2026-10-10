// Modify the flow of extrusion lines inversely proportional to the length of
// the extrusion line. When infill lines get shorter the flow rate will auto-
// matically be reduced to mitigate the effect of small infill areas being
// over-extruded.

// Based on original work by Alexander Þór licensed under the GPLv3:
// https://github.com/Alexander-T-Moss/Small-Area-Flow-Comp

#include "SmallAreaInfillFlowCompensator.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <boost/log/trivial.hpp>

#include "../PrintConfig.hpp"
#include "libslic3r/CurveModel.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"

#include "SmallAreaInfillFlowCompensationModel.hpp"
#include "libslic3r/Config.hpp"

namespace Slic3r {

SmallAreaInfillFlowCompensator::SmallAreaInfillFlowCompensator(const Slic3r::GCodeConfig& config)
{
    try {
        // Use the editor's complete, locale-independent parser as well as its constraints.
        int error_row;
        const SmallAreaInfillFlowCompensationModel model;
        if (const char* error = model.read_points(CurveModel::decode_rows(config.small_area_infill_flow_compensation_model.values),
                                                 eLengths, flowComps, error_row))
            throw Slic3r::InvalidArgument(std::string("Small Area Flow Compensation: ") + error);

        // GCode skips empty settings; constructing a consumer directly still requires points.
        if (eLengths.empty())
            throw std::invalid_argument("Input vectors must have the same size and contain at least two points.");
        // Use the same factory as the graph, including any feature-specific override.
        flowModel = model.make_interpolator(eLengths, flowComps);

    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Error parsing small area infill compensation model: " << e.what();
        throw;
    }
}

SmallAreaInfillFlowCompensator::~SmallAreaInfillFlowCompensator() = default;

double SmallAreaInfillFlowCompensator::flow_comp_model(const double line_length)
{
    if (!flowModel)
        return 1.0;

    if (line_length == 0 || line_length > max_modified_length()) {
        return 1.0;
    }

    return flowModel(line_length);
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
