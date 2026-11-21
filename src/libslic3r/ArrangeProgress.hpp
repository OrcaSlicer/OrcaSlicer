#pragma once

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace arrangement {

// An estimate of work, independent of the actual number of objects placed.
// Each new plate gets a share of the remaining displayed progress. Re-anchor
// after early finishes or revised estimates, never revising progress downward.
class ArrangeProgress
{
    double m_value = 0.;
    double m_start = 0.;
    double m_end = 0.;

public:
    double observe(double fraction)
    {
        if (std::isfinite(fraction))
            m_value = std::max(m_value, std::clamp(fraction, 0., 0.99));
        return m_value;
    }

    void begin_plate(double completed_fraction, double remaining_plates)
    {
        m_start = observe(0.99 * completed_fraction);
        m_end = m_start + (0.99 - m_start) / std::max(1., remaining_plates);
    }

    double advance(double elapsed, double budget)
    {
        const double fraction = budget > 0. ? std::clamp(elapsed / budget, 0., 1.) : 0.;
        return observe(m_start + (m_end - m_start) * fraction);
    }
};

}} // namespace Slic3r::arrangement
