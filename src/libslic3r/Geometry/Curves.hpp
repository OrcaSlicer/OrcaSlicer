#ifndef SRC_LIBSLIC3R_GEOMETRY_CURVES_HPP_
#define SRC_LIBSLIC3R_GEOMETRY_CURVES_HPP_

#include "libslic3r/Point.hpp"
#include "Bicubic.hpp"

#include <iostream>

//#define LSQR_DEBUG

namespace Slic3r {
namespace Geometry {

template<int Dimension, typename NumberType>
struct PolynomialCurve {
    Eigen::MatrixXf coefficients;

    Vec<Dimension, NumberType> get_fitted_value(const NumberType& value) const {
        Vec<Dimension, NumberType> result = Vec<Dimension, NumberType>::Zero();
        size_t order = this->coefficients.rows() - 1;
        auto x = NumberType(1.);
        for (size_t index = 0; index < order + 1; ++index, x *= value)
            result += x * this->coefficients.col(index);
        return result;
    }
};

//https://towardsdatascience.com/least-square-polynomial-CURVES-using-c-eigen-package-c0673728bd01
template<int Dimension, typename NumberType>
PolynomialCurve<Dimension, NumberType> fit_polynomial(const std::vector<Vec<Dimension, NumberType>> &observations,
        const std::vector<NumberType> &observation_points,
        const std::vector<NumberType> &weights, size_t order) {
    // check to make sure inputs are correct
    size_t cols = order + 1;
    assert(observation_points.size() >= cols);
    assert(observation_points.size() == weights.size());
    assert(observations.size() == weights.size());

    Eigen::MatrixXf data_points(Dimension, observations.size());
    Eigen::MatrixXf T(observations.size(), cols);
    for (size_t i = 0; i < weights.size(); ++i) {
        auto squared_weight = sqrt(weights[i]);
        data_points.col(i) = observations[i] * squared_weight;
        // Populate the matrix
        auto x = squared_weight;
        auto c = observation_points[i];
        for (size_t j = 0; j < cols; ++j, x *= c)
            T(i, j) = x;
    }

    const auto QR = T.householderQr();
    Eigen::MatrixXf coefficients(Dimension, cols);
    // Solve for linear least square fit
    for (size_t dim = 0; dim < Dimension; ++dim) {
        coefficients.row(dim) = QR.solve(data_points.row(dim).transpose());
    }

    return {std::move(coefficients)};
}

template<size_t Dimension, typename NumberType, typename KernelType>
struct PiecewiseFittedCurve {
    using Kernel = KernelType;

    Eigen::MatrixXf coefficients;
    NumberType start;
    NumberType segment_size;
    size_t endpoints_level_of_freedom;

    Vec<Dimension, NumberType> get_fitted_value(const NumberType &observation_point) const {
        Vec<Dimension, NumberType> result = Vec<Dimension, NumberType>::Zero();

        //find corresponding segment index; expects kernels to be centered
        int middle_right_segment_index = floor((observation_point - start) / segment_size);
        //find index of first segment that is affected by the point i; this can be deduced from kernel_span
        int start_segment_idx = middle_right_segment_index - Kernel::kernel_span / 2 + 1;
        for (int segment_index = start_segment_idx; segment_index < int(start_segment_idx + Kernel::kernel_span);
                segment_index++) {
            NumberType segment_start = start + segment_index * segment_size;
            NumberType normalized_segment_distance = (segment_start - observation_point) / segment_size;

            int parameter_index = segment_index + endpoints_level_of_freedom;
            parameter_index = std::clamp(parameter_index, 0, int(coefficients.cols()) - 1);
            result += Kernel::kernel(normalized_segment_distance) * coefficients.col(parameter_index);
        }
        return result;
    }
};

// observations: data to be fitted by the curve
// observation points: growing sequence of points where the observations were made.
//      In other words, for function f(x) = y, observations are y0...yn, and observation points are x0...xn
// weights: how important the observation is
// segments_count: number of segments inside the valid length of the curve
// endpoints_level_of_freedom: number of additional parameters at each end; reasonable values depend on the kernel span
template<typename Kernel, int Dimension, typename NumberType>
PiecewiseFittedCurve<Dimension, NumberType, Kernel> fit_curve(
        const std::vector<Vec<Dimension, NumberType>> &observations,
        const std::vector<NumberType> &observation_points,
        const std::vector<NumberType> &weights,
        size_t segments_count,
        size_t endpoints_level_of_freedom) {

    // check to make sure inputs are correct
    assert(segments_count > 0);
    assert(observations.size() > 0);
    assert(observation_points.size() == observations.size());
    assert(observation_points.size() == weights.size());
    assert(segments_count <= observations.size());

    //prepare sqrt of weights, which will then be applied to both matrix T and observed data: https://en.wikipedia.org/wiki/Weighted_least_squares
    std::vector<NumberType> sqrt_weights(weights.size());
    for (size_t index = 0; index < weights.size(); ++index) {
        assert(weights[index] > 0);
        sqrt_weights[index] = sqrt(weights[index]);
    }

    // prepare result and compute metadata
    PiecewiseFittedCurve<Dimension, NumberType, Kernel> result { };

    NumberType valid_length = observation_points.back() - observation_points.front();
    NumberType segment_size = valid_length / NumberType(segments_count);
    result.start = observation_points.front();
    result.segment_size = segment_size;
    result.endpoints_level_of_freedom = endpoints_level_of_freedom;

    // prepare observed data
    // Eigen defaults to column major memory layout.
    Eigen::MatrixXf data_points(Dimension, observations.size());
    for (size_t index = 0; index < observations.size(); ++index) {
        data_points.col(index) = observations[index] * sqrt_weights[index];
    }
    // parameters count is always increased by one to make the parametric space of the curve symmetric.
    // without this fix, the end of the curve is less flexible than the beginning
    size_t parameters_count = segments_count + 1 + 2 * endpoints_level_of_freedom;
    //Create weight matrix T for each point and each segment;
    Eigen::MatrixXf T(observation_points.size(), parameters_count);
    T.setZero();
    //Fill the weight matrix
    for (size_t i = 0; i < observation_points.size(); ++i) {
        NumberType observation_point = observation_points[i];
        //find corresponding segment index; expects kernels to be centered
        int middle_right_segment_index = floor((observation_point - result.start) / result.segment_size);
        //find index of first segment that is affected by the point i; this can be deduced from kernel_span
        int start_segment_idx = middle_right_segment_index - int(Kernel::kernel_span / 2) + 1;
        for (int segment_index = start_segment_idx; segment_index < int(start_segment_idx + Kernel::kernel_span);
                segment_index++) {
            NumberType segment_start = result.start + segment_index * result.segment_size;
            NumberType normalized_segment_distance = (segment_start - observation_point) / result.segment_size;

            int parameter_index = segment_index + endpoints_level_of_freedom;
            parameter_index = std::clamp(parameter_index, 0, int(parameters_count) - 1);
            T(i, parameter_index) += Kernel::kernel(normalized_segment_distance) * sqrt_weights[i];
        }
    }

#ifdef LSQR_DEBUG
    std::cout << "weight matrix: " << std::endl;
    for (int obs = 0; obs < observation_points.size(); ++obs) {
        std::cout << std::endl;
        for (int segment = 0; segment < parameters_count; ++segment) {
            std::cout << T(obs, segment) << "  ";
        }
    }
    std::cout << std::endl;
#endif

    // Solve for linear least square fit
    result.coefficients.resize(Dimension, parameters_count);
    const auto QR = T.fullPivHouseholderQr();
    for (size_t dim = 0; dim < Dimension; ++dim) {
        result.coefficients.row(dim) = QR.solve(data_points.row(dim).transpose());
    }

    return result;
}


template<int Dimension, typename NumberType>
PiecewiseFittedCurve<Dimension, NumberType, LinearKernel<NumberType>>
fit_linear_spline(
        const std::vector<Vec<Dimension, NumberType>> &observations,
        std::vector<NumberType> observation_points,
        std::vector<NumberType> weights,
        size_t segments_count,
        size_t endpoints_level_of_freedom = 0) {
    return fit_curve<LinearKernel<NumberType>>(observations, observation_points, weights, segments_count,
            endpoints_level_of_freedom);
}

template<int Dimension, typename NumberType>
PiecewiseFittedCurve<Dimension, NumberType, CubicBSplineKernel<NumberType>>
fit_cubic_bspline(
        const std::vector<Vec<Dimension, NumberType>> &observations,
        std::vector<NumberType> observation_points,
        std::vector<NumberType> weights,
        size_t segments_count,
        size_t endpoints_level_of_freedom = 0) {
    return fit_curve<CubicBSplineKernel<NumberType>>(observations, observation_points, weights, segments_count,
            endpoints_level_of_freedom);
}

template<int Dimension, typename NumberType>
PiecewiseFittedCurve<Dimension, NumberType, CubicCatmulRomKernel<NumberType>>
fit_catmul_rom_spline(
        const std::vector<Vec<Dimension, NumberType>> &observations,
        std::vector<NumberType> observation_points,
        std::vector<NumberType> weights,
        size_t segments_count,
        size_t endpoints_level_of_freedom = 0) {
    return fit_curve<CubicCatmulRomKernel<NumberType>>(observations, observation_points, weights, segments_count,
            endpoints_level_of_freedom);
}

}
}

#endif /* SRC_LIBSLIC3R_GEOMETRY_CURVES_HPP_ */
