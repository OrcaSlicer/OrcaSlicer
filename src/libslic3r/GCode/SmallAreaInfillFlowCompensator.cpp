// Modify the flow of extrusion lines inversely proportional to the length of
// the extrusion line. When infill lines get shorter the flow rate will auto-
// matically be reduced to mitigate the effect of small infill areas being
// over-extruded.

// Based on original work by Alexander Þór licensed under the GPLv3:
// https://github.com/Alexander-T-Moss/Small-Area-Flow-Comp

#include "SmallAreaInfillFlowCompensator.hpp"

#include <exception>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <boost/log/trivial.hpp>

#include "../PrintConfig.hpp"
#include "libslic3r/CurveModel.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/format.hpp"

#include "SmallAreaInfillFlowCompensationModel.hpp"
#include "libslic3r/Config.hpp"

namespace Slic3r {

SmallAreaInfillFlowCompensator::SmallAreaInfillFlowCompensator(const Slic3r::GCodeConfig& config)
{
    try {
        // Use the editor's complete, locale-independent parser as well as its constraints.
        int error_row = -1;
        const SmallAreaInfillFlowCompensationModel model;
        const auto rows = CurveModel::decode_rows(config.small_area_infill_flow_compensation_model.values);
        if (const char* error = model.read_points(rows, eLengths, flowComps, error_row)) {
            std::string message = _u8L(error);
            if (error_row >= 0 && size_t(error_row) < rows.size())
                message = Slic3r::format(_u8L("Table row %d: %s"), error_row + 1, message) +
                    " [" + rows[error_row].first + "," + rows[error_row].second + "]";
            // TRN %s is the reason the small area flow compensation model is invalid.
            throw Slic3r::InvalidArgument(Slic3r::format(_u8L("Flow Compensation Model: %s"), message));
        }

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
