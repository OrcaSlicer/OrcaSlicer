#pragma once

#include <vector>
#include "libslic3r/CurveModel.hpp"

namespace Slic3r {

class SmallAreaInfillFlowCompensationModel : public CurveModel
{
public:
    using CurveModel::CurveModel;

    const char* validate_points(const std::vector<double>& x, const std::vector<double>& y, int& row) const override;
    CurveView fitted_view(const std::vector<double>& x, const std::vector<double>& y) const override;
    CurveViewLimits view_limits() const override;
    CurveView drag_bounds(int row, const std::vector<double>& x, const std::vector<double>& y,
                          const CurveView& view) const override;
    Rows default_rows() const override;
    Rows seed_rows() const override;
    const char* empty_message() const override;
};

} // namespace Slic3r
