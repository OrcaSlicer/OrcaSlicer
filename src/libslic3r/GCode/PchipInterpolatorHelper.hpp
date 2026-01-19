// PchipInterpolatorHelper.hpp
// OrcaSlicer
//
// Header file for the PchipInterpolatorHelper class, responsible for performing Piecewise Cubic Hermite Interpolating Polynomial (PCHIP) interpolation on given data points.

#ifndef PCHIPINTERPOLATORHELPER_HPP
#define PCHIPINTERPOLATORHELPER_HPP

#include <vector>

/**
 * @class PchipInterpolatorHelper
 * @brief A helper class to perform Piecewise Cubic Hermite Interpolating Polynomial (PCHIP) interpolation.
 */
class PchipInterpolatorHelper {
public:
    /**
     * @brief Default constructor.
     */
    PchipInterpolatorHelper() = default;

    /**
     * @brief Constructs the PCHIP interpolator with given data points.
     * @param x The x-coordinates of the data points.
     * @param y The y-coordinates of the data points.
     */
    PchipInterpolatorHelper(const std::vector<double>& x, const std::vector<double>& y);

    /**
     * @brief Sets the data points for the interpolator.
     * @param x The x-coordinates of the data points.
     * @param y The y-coordinates of the data points.
     * @throw std::invalid_argument if x and y have different sizes or if they contain fewer than two points.
     */
    void setData(const std::vector<double>& x, const std::vector<double>& y);

    /**
     * @brief Interpolates the value at a given point.
     * @param xi The x-coordinate at which to interpolate.
     * @return The interpolated y-coordinate.
     */
    double interpolate(double xi) const;

private:
    std::vector<double> x_; ///< The x-coordinates of the data points.
    std::vector<double> y_; ///< The y-coordinates of the data points.
    std::vector<double> h_; ///< The differences between successive x-coordinates.
    std::vector<double> delta_; ///< The slopes of the segments between successive data points.
    std::vector<double> d_; ///< The derivatives at the data points.

    /**
     * @brief Computes the PCHIP coefficients.
     */
    void computePCHIP();

    /**
     * @brief Sorts the data points by x-coordinate.
     */
    void sortData();

    /**
     * @brief Computes the difference between successive x-coordinates.
     * @param i The index of the x-coordinate.
     * @return The difference between x_[i+1] and x_[i].
     */
    double h(int i) const { return x_[i+1] - x_[i]; }

    /**
     * @brief Computes the slope of the segment between successive data points.
     * @param i The index of the segment.
     * @return The slope of the segment between y_[i] and y_[i+1].
     */
    double delta(int i) const { return (y_[i+1] - y_[i]) / h(i); }
};

#endif // PCHIPINTERPOLATORHELPER_HPP
