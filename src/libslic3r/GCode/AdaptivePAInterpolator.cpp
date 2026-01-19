// AdaptivePAInterpolator.cpp
// OrcaSlicer
//
// Implementation file for the AdaptivePAInterpolator class, providing methods to parse data and perform PA interpolation.

#include "AdaptivePAInterpolator.hpp"
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <sstream>

/**
 * @brief Parses the input data and sets up the interpolators.
 * @param data A string containing the data in CSV format (PA, flow rate, acceleration).
 * @return 0 on success, -1 on error.
 */
int AdaptivePAInterpolator::parseAndSetData(const std::string& data) {
    flow_interpolators_.clear();
    accelerations_.clear();

    try {
        std::istringstream ss(data);
        std::string line;
        std::map<double, std::vector<std::pair<double, double>>> acc_to_flow_pa;

        while (std::getline(ss, line)) {
            std::istringstream lineStream(line);
            std::string value;
            double paValue, flowRate, acceleration;
            paValue = flowRate = acceleration = 0.f; // initialize all to zero.

            // Parse PA value
            if (std::getline(lineStream, value, ',')) {
                paValue = std::stod(value);
            }

            // Parse flow rate value
            if (std::getline(lineStream, value, ',')) {
                flowRate = std::stod(value);
            }

            // Parse acceleration value
            if (std::getline(lineStream, value, ',')) {
                acceleration = std::stod(value);
            }

            // Store the parsed values in a map with acceleration as the key
            acc_to_flow_pa[acceleration].emplace_back(flowRate, paValue);
        }

        // Iterate through the map to set up the interpolators
        for (const auto& kv : acc_to_flow_pa) {
            double acceleration = kv.first;
            const auto& data = kv.second;

            std::vector<double> flowRates;
            std::vector<double> paValues;

            for (const auto& pair : data) {
                flowRates.push_back(pair.first);
                paValues.push_back(pair.second);
            }

            // Only set up the interpolator if there are enough data points
            if (flowRates.size() > 1) {
                PchipInterpolatorHelper interpolator(flowRates, paValues);
                flow_interpolators_[acceleration] = interpolator;
                accelerations_.push_back(acceleration);
            }
        }
    } catch (const std::exception&) {
        m_isInitialised = false;
        return -1; // Error: Exception during parsing
    }
    m_isInitialised = true;
    return 0; // Success
}

/**
 * @brief Interpolates the PA value for the given flow rate and acceleration.
 * @param flow_rate The flow rate at which to interpolate.
 * @param acceleration The acceleration at which to interpolate.
 * @return The interpolated PA value, or -1 if interpolation fails.
 */
double AdaptivePAInterpolator::operator()(double flow_rate, double acceleration) {
    std::vector<double> pa_values;
    std::vector<double> acc_values;

    // Estimate PA value for every flow to PA model for the given flow rate
    for (const auto& kv : flow_interpolators_) {
        double pa_value = kv.second.interpolate(flow_rate);
        
        // Check if the interpolated PA value is valid
        if (pa_value != -1) {
            pa_values.push_back(pa_value);
            acc_values.push_back(kv.first);
        }
    }

    // Check if there are enough acceleration values for interpolation
    if (acc_values.size() < 2) {
        // Special case: Only one acceleration value
        if (acc_values.size() == 1) {
            return std::round(pa_values[0] * 1000.0) / 1000.0; // Rounded to 3 decimal places
        }
        return -1; // Error: Not enough data points for interpolation
    }

    // Create a new PchipInterpolatorHelper for PA-acceleration interpolation
    // Use the estimated PA values from the for loop above and their corresponding accelerations to
    // generate the new PCHIP model. Then run this model to interpolate the PA value for the given acceleration value.
    PchipInterpolatorHelper pa_accel_interpolator(acc_values, pa_values);
    return std::round(pa_accel_interpolator.interpolate(acceleration) * 1000.0) / 1000.0; // Rounded to 3 decimal places
}
