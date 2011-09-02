// AdaptivePAInterpolator.hpp
// OrcaSlicer
//
// Header file for the AdaptivePAInterpolator class, responsible for interpolating pressure advance (PA) values based on flow rate and acceleration using PCHIP interpolation.

#ifndef ADAPTIVEPAINTERPOLATOR_HPP
#define ADAPTIVEPAINTERPOLATOR_HPP

#include <vector>
#include <string>
#include <map>
#include "PchipInterpolatorHelper.hpp"

/**
 * @class AdaptivePAInterpolator
 * @brief A class to interpolate pressure advance (PA) values based on flow rate and acceleration using Piecewise Cubic Hermite Interpolating Polynomial (PCHIP) interpolation.
 */
class AdaptivePAInterpolator {
public:
    /**
     * @brief Default constructor.
     */
    AdaptivePAInterpolator() : m_isInitialised(false) {}

    /**
     * @brief Parses the input data and sets up the interpolators.
     * @param data A string containing the data in CSV format (PA, flow rate, acceleration).
     * @return 0 on success, -1 on error.
     */
    int parseAndSetData(const std::string& data);

    /**
     * @brief Interpolates the PA value for the given flow rate and acceleration.
     * @param flow_rate The flow rate at which to interpolate.
     * @param acceleration The acceleration at which to interpolate.
     * @return The interpolated PA value, or -1 if interpolation fails.
     */
    double operator()(double flow_rate, double acceleration);
    
    /**
     * @brief Returns the initialization status.
     * @return The value of m_isInitialised.
     */
    bool isInitialised() const {
        return m_isInitialised;
    }

private:
    std::map<double, PchipInterpolatorHelper> flow_interpolators_; ///< Map each acceleration to a flow-rate-to-PA interpolator.
    std::vector<double> accelerations_; ///< Store unique accelerations.
    bool m_isInitialised;
};

#endif // ADAPTIVEPAINTERPOLATOR_HPP
