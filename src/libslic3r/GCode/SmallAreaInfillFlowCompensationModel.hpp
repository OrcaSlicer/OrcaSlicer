#pragma once

#include "libslic3r/CurveModel.hpp"

namespace Slic3r {

class SmallAreaInfillFlowCompensationModel : public CurveModel
{
public:
    using CurveModel::CurveModel;

    const char* validate_points(const std::vector<double>& x, const std::vector<double>& y, int& row) const override;
    Interpolator make_interpolator(const std::vector<double>& x, const std::vector<double>& y) const override;
    CurveView fitted_view(const std::vector<double>& x, const std::vector<double>& y) const override;
    CurveView drag_bounds(int row, const std::vector<double>& x, const std::vector<double>& y,
                          const CurveView& view) const override;
    Rows default_rows() const override;
    Rows seed_rows() const override;
    const char* empty_message() const override;
    const char* validate_view(const CurveView& view) const override;
};

} // namespace Slic3r
