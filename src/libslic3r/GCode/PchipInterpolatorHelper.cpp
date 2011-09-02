// PchipInterpolatorHelper.cpp
// OrcaSlicer
//
// Implementation file for the PchipInterpolatorHelper class

#include "PchipInterpolatorHelper.hpp"
#include <stdexcept>
#include <cmath>
#include <algorithm>

/**
 * @brief Constructs the PCHIP interpolator with given data points.
 * @param x The x-coordinates of the data points.
 * @param y The y-coordinates of the data points.
 */
PchipInterpolatorHelper::PchipInterpolatorHelper(const std::vector<double>& x, const std::vector<double>& y) {
    setData(x, y);
}

/**
 * @brief Sets the data points for the interpolator.
 * @param x The x-coordinates of the data points.
 * @param y The y-coordinates of the data points.
 * @throw std::invalid_argument if x and y have different sizes or if they contain fewer than two points.
 */
void PchipInterpolatorHelper::setData(const std::vector<double>& x, const std::vector<double>& y) {
    if (x.size() != y.size() || x.size() < 2) {
        throw std::invalid_argument("Input vectors must have the same size and contain at least two points.");
    }
    x_ = x;
    y_ = y;
    sortData();
    computePCHIP();
}

/**
 * @brief Sorts the data points by x-coordinate.
 */
void PchipInterpolatorHelper::sortData() {
    std::vector<std::pair<double, double>> data;
    for (size_t i = 0; i < x_.size(); ++i) {
        data.emplace_back(x_[i], y_[i]);
    }
    std::sort(data.begin(), data.end());

    for (size_t i = 0; i < data.size(); ++i) {
        x_[i] = data[i].first;
        y_[i] = data[i].second;
    }
}

/**
 * @brief Computes the PCHIP coefficients.
 */
void PchipInterpolatorHelper::computePCHIP() {
    size_t n = x_.size() - 1;
    h_.resize(n);
    delta_.resize(n);
    d_.resize(n+1);

    for (size_t i = 0; i < n; ++i) {
        h_[i] = h(i);
        delta_[i] = delta(i);
    }

    d_[0] = delta_[0];
    d_[n] = delta_[n-1];
    for (size_t i = 1; i < n; ++i) {
        if (delta_[i-1] * delta_[i] > 0) {
            double w1 = 2 * h_[i] + h_[i-1];
            double w2 = h_[i] + 2 * h_[i-1];
            d_[i] = (w1 + w2) / (w1 / delta_[i-1] + w2 / delta_[i]);
        } else {
            d_[i] = 0;
        }
    }
}

/**
 * @brief Interpolates the value at a given point.
 */
double PchipInterpolatorHelper::interpolate(double xi) const {
    if (xi <= x_.front()) return y_.front();
    if (xi >= x_.back()) return y_.back();

    auto it = std::lower_bound(x_.begin(), x_.end(), xi);
    size_t i = std::distance(x_.begin(), it) - 1;

    double h_i = h_[i];
    double t = (xi - x_[i]) / h_i;
    double t2 = t * t;
    double t3 = t2 * t;

    double h00 = 2 * t3 - 3 * t2 + 1;
    double h10 = t3 - 2 * t2 + t;
    double h01 = -2 * t3 + 3 * t2;
    double h11 = t3 - t2;

    return h00 * y_[i] + h10 * h_i * d_[i] + h01 * y_[i+1] + h11 * h_i * d_[i+1];
}
