#include "libslic3r/CAD/ThreadStandards.hpp"

namespace Slic3r {

// Imperial helpers: convert nominal inch diameter / threads-per-inch to mm.
static constexpr double IN  = 25.4;
static inline double tpi_pitch(double tpi) { return IN / tpi; }

const std::vector<ThreadSpec>& thread_standards()
{
    using S = ThreadSpec::Series;
    static const std::vector<ThreadSpec> table = {
        // --- ISO metric, coarse pitch (ISO 261 preferred series) ---
        {"M1",    1.0,  0.25, S::MetricCoarse},
        {"M1.2",  1.2,  0.25, S::MetricCoarse},
        {"M1.6",  1.6,  0.35, S::MetricCoarse},
        {"M2",    2.0,  0.40, S::MetricCoarse},
        {"M2.5",  2.5,  0.45, S::MetricCoarse},
        {"M3",    3.0,  0.50, S::MetricCoarse},
        {"M4",    4.0,  0.70, S::MetricCoarse},
        {"M5",    5.0,  0.80, S::MetricCoarse},
        {"M6",    6.0,  1.00, S::MetricCoarse},
        {"M8",    8.0,  1.25, S::MetricCoarse},
        {"M10",  10.0,  1.50, S::MetricCoarse},
        {"M12",  12.0,  1.75, S::MetricCoarse},
        {"M14",  14.0,  2.00, S::MetricCoarse},
        {"M16",  16.0,  2.00, S::MetricCoarse},
        {"M20",  20.0,  2.50, S::MetricCoarse},
        {"M24",  24.0,  3.00, S::MetricCoarse},
        {"M30",  30.0,  3.50, S::MetricCoarse},
        {"M36",  36.0,  4.00, S::MetricCoarse},
        {"M42",  42.0,  4.50, S::MetricCoarse},
        {"M48",  48.0,  5.00, S::MetricCoarse},
        {"M56",  56.0,  5.50, S::MetricCoarse},
        {"M64",  64.0,  6.00, S::MetricCoarse},

        // --- ISO metric, common fine pitches (ISO 261 fine series) ---
        {"M8x1",      8.0,  1.00, S::MetricFine},
        {"M10x1.25", 10.0,  1.25, S::MetricFine},
        {"M10x1",    10.0,  1.00, S::MetricFine},
        {"M12x1.5",  12.0,  1.50, S::MetricFine},
        {"M12x1.25", 12.0,  1.25, S::MetricFine},
        {"M16x1.5",  16.0,  1.50, S::MetricFine},
        {"M20x1.5",  20.0,  1.50, S::MetricFine},
        {"M24x2",    24.0,  2.00, S::MetricFine},

        // --- Unified National Coarse (UTS / ASME B1.1) ---
        {"#1-64 UNC",   0.073 * IN, tpi_pitch(64), S::UNC},
        {"#2-56 UNC",   0.086 * IN, tpi_pitch(56), S::UNC},
        {"#3-48 UNC",   0.099 * IN, tpi_pitch(48), S::UNC},
        {"#4-40 UNC",   0.112 * IN, tpi_pitch(40), S::UNC},
        {"#5-40 UNC",   0.125 * IN, tpi_pitch(40), S::UNC},
        {"#6-32 UNC",   0.138 * IN, tpi_pitch(32), S::UNC},
        {"#8-32 UNC",   0.164 * IN, tpi_pitch(32), S::UNC},
        {"#10-24 UNC",  0.190 * IN, tpi_pitch(24), S::UNC},
        {"#12-24 UNC",  0.216 * IN, tpi_pitch(24), S::UNC},
        {"1/4-20 UNC",  0.250 * IN, tpi_pitch(20), S::UNC},
        {"5/16-18 UNC", 0.3125 * IN, tpi_pitch(18), S::UNC},
        {"3/8-16 UNC",  0.375 * IN, tpi_pitch(16), S::UNC},
        {"7/16-14 UNC", 0.4375 * IN, tpi_pitch(14), S::UNC},
        {"1/2-13 UNC",  0.500 * IN, tpi_pitch(13), S::UNC},
        {"9/16-12 UNC", 0.5625 * IN, tpi_pitch(12), S::UNC},
        {"5/8-11 UNC",  0.625 * IN, tpi_pitch(11), S::UNC},
        {"3/4-10 UNC",  0.750 * IN, tpi_pitch(10), S::UNC},
        {"7/8-9 UNC",   0.875 * IN, tpi_pitch(9),  S::UNC},
        {"1-8 UNC",     1.000 * IN, tpi_pitch(8),  S::UNC},

        // --- Unified National Fine (UTS / ASME B1.1) ---
        {"#2-64 UNF",   0.086 * IN, tpi_pitch(64), S::UNF},
        {"#4-48 UNF",   0.112 * IN, tpi_pitch(48), S::UNF},
        {"#6-40 UNF",   0.138 * IN, tpi_pitch(40), S::UNF},
        {"#8-36 UNF",   0.164 * IN, tpi_pitch(36), S::UNF},
        {"#10-32 UNF",  0.190 * IN, tpi_pitch(32), S::UNF},
        {"1/4-28 UNF",  0.250 * IN, tpi_pitch(28), S::UNF},
        {"5/16-24 UNF", 0.3125 * IN, tpi_pitch(24), S::UNF},
        {"3/8-24 UNF",  0.375 * IN, tpi_pitch(24), S::UNF},
        {"7/16-20 UNF", 0.4375 * IN, tpi_pitch(20), S::UNF},
        {"1/2-20 UNF",  0.500 * IN, tpi_pitch(20), S::UNF},
        {"9/16-18 UNF", 0.5625 * IN, tpi_pitch(18), S::UNF},
        {"5/8-18 UNF",  0.625 * IN, tpi_pitch(18), S::UNF},
        {"3/4-16 UNF",  0.750 * IN, tpi_pitch(16), S::UNF},
        {"1-12 UNF",    1.000 * IN, tpi_pitch(12), S::UNF},
    };
    return table;
}

const ThreadSpec* find_thread_standard(const std::string& name)
{
    for (const ThreadSpec& s : thread_standards())
        if (s.name == name)
            return &s;
    return nullptr;
}

} // namespace Slic3r
