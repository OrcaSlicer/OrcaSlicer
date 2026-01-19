#include "Circle.hpp"

#include "../Polygon.hpp"

#include <numeric>
#include <random>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace Geometry {

Point circle_center_taubin_newton(const Points::const_iterator& input_begin, const Points::const_iterator& input_end, size_t cycles)
{
    Vec2ds tmp;
    tmp.reserve(std::distance(input_begin, input_end));
    std::transform(input_begin, input_end, std::back_inserter(tmp), [] (const Point& in) { return unscale(in); } );
    Vec2d center = circle_center_taubin_newton(tmp.cbegin(), tmp.end(), cycles);
	return Point::new_scale(center.x(), center.y());
}

/// Adapted from work in "Circular and Linear Regression: Fitting circles and lines by least squares", pg 126
/// Returns a point corresponding to the center of a circle for which all of the points from input_begin to input_end
/// lie on.
Vec2d circle_center_taubin_newton(const Vec2ds::const_iterator& input_begin, const Vec2ds::const_iterator& input_end, size_t cycles)
{
    // calculate the centroid of the data set
    const Vec2d sum = std::accumulate(input_begin, input_end, Vec2d(0,0));
    const size_t n = std::distance(input_begin, input_end);
    const double n_flt = static_cast<double>(n);
    const Vec2d centroid { sum / n_flt };

    // Compute the normalized moments of the data set.
    double Mxx = 0, Myy = 0, Mxy = 0, Mxz = 0, Myz = 0, Mzz = 0;
    for (auto it = input_begin; it < input_end; ++it) {
        // center/normalize the data.
        double Xi {it->x() - centroid.x()};
        double Yi {it->y() - centroid.y()};
        double Zi {Xi*Xi + Yi*Yi};
        Mxy += (Xi*Yi);
        Mxx += (Xi*Xi);
        Myy += (Yi*Yi);
        Mxz += (Xi*Zi);
        Myz += (Yi*Zi);
        Mzz += (Zi*Zi);
    }

    // divide by number of points to get the moments
    Mxx /= n_flt;
    Myy /= n_flt;
    Mxy /= n_flt;
    Mxz /= n_flt;
    Myz /= n_flt;
    Mzz /= n_flt;

    // Compute the coefficients of the characteristic polynomial for the circle
    // eq 5.60
    const double Mz {Mxx + Myy}; // xx + yy = z
    const double Cov_xy {Mxx*Myy - Mxy*Mxy}; // this shows up a couple times so cache it here.
    const double C3 {4.0*Mz};
    const double C2 {-3.0*(Mz*Mz) - Mzz};
    const double C1 {Mz*(Mzz - (Mz*Mz)) + 4.0*Mz*Cov_xy - (Mxz*Mxz) - (Myz*Myz)};
    const double C0 {(Mxz*Mxz)*Myy + (Myz*Myz)*Mxx - 2.0*Mxz*Myz*Mxy - Cov_xy*(Mzz - (Mz*Mz))};

    const double C22 = {C2 + C2};
    const double C33 = {C3 + C3 + C3};

    // solve the characteristic polynomial with Newton's method.
    double xnew = 0.0;
    double ynew = 1e20;

    for (size_t i = 0; i < cycles; ++i) {
        const double yold {ynew};
        ynew = C0 + xnew * (C1 + xnew*(C2 + xnew * C3));
        if (std::abs(ynew) > std::abs(yold)) {
			BOOST_LOG_TRIVIAL(error) << "Geometry: Fit is going in the wrong direction.\n";
            return Vec2d(std::nan(""), std::nan(""));
        }
        const double Dy {C1 + xnew*(C22 + xnew*C33)};

        const double xold {xnew};
        xnew = xold - (ynew / Dy);

        if (std::abs((xnew-xold) / xnew) < 1e-12) i = cycles; // converged, we're done here

        if (xnew < 0) {
            // reset, we went negative
            xnew = 0.0;
        }
    }
    
    // compute the determinant and the circle's parameters now that we've solved.
    double DET = xnew*xnew - xnew*Mz + Cov_xy;

    Vec2d center(Mxz * (Myy - xnew) - Myz * Mxy, Myz * (Mxx - xnew) - Mxz*Mxy);
    center /= (DET * 2.);
    return center + centroid;
}

Circled circle_taubin_newton(const Vec2ds& input, size_t cycles)
{
    Circled out;
    if (input.size() < 3) {
        out = Circled::make_invalid();
    } else {
        out.center = circle_center_taubin_newton(input, cycles);
        out.radius = std::accumulate(input.begin(), input.end(), 0., [&out](double acc, const Vec2d& pt) { return (pt - out.center).norm() + acc; });
        out.radius /= double(input.size());
    }
    return out;
}

Circled circle_ransac(const Vec2ds& input, size_t iterations, double* min_error)
{
    if (input.size() < 3)
        return Circled::make_invalid();

    std::mt19937 rng;
    std::vector<Vec2d> samples;
    Circled circle_best = Circled::make_invalid();
    double  err_min = std::numeric_limits<double>::max();
    for (size_t iter = 0; iter < iterations; ++ iter) {
        samples.clear();
        std::sample(input.begin(), input.end(), std::back_inserter(samples), 3, rng);
        Circled c;
        c.center = Geometry::circle_center(samples[0], samples[1], samples[2], EPSILON);
        c.radius = std::accumulate(input.begin(), input.end(), 0., [&c](double acc, const Vec2d& pt) { return (pt - c.center).norm() + acc; });
        c.radius /= double(input.size());
        double err = 0;
        for (const Vec2d &pt : input)
            err = std::max(err, std::abs((pt - c.center).norm() - c.radius));
        if (err < err_min) {
            err_min = err;
            circle_best = c;
        }
    }
    if (min_error)
        *min_error = err_min;
    return circle_best;
}

} } // namespace Slic3r::Geometry
