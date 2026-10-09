#pragma once

#include <string>
#include <vector>
#include "libslic3r/CurveModel.hpp"

namespace Slic3r {

class SmallAreaInfillFlowCompensationModel : public CurveModel
{
public:
    explicit SmallAreaInfillFlowCompensationModel(std::vector<std::string> parameters = {}, double bed_diagonal = 0.0);

    const char* validate_points(const std::vector<double>& x, const std::vector<double>& y, int& row) const override;
    CurveView fitted_view(const std::vector<double>& x, const std::vector<double>& y) const override;
    CurveViewLimits view_limits() const override;
    bool expand_view_limits(const std::vector<double>& x) override;
    const char* x_view_limit_message() const override;
    CurveView drag_bounds(int row, const std::vector<double>& x, const std::vector<double>& y,
                          const CurveView& view) const override;
    Rows default_rows() const override;
    Rows seed_rows() const override;
    const char* empty_message() const override;

private:
    double m_maximum_x;
};

} // namespace Slic3r
