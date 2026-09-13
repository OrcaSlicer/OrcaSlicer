#ifndef slic3r_ThreadStandards_hpp_
#define slic3r_ThreadStandards_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// Canonical mechanical thread specifications (ISO metric + Unified imperial).
// All dimensions are stored in millimetres so the CAD kernel can consume them
// directly. The profile is the common 60deg V shared by ISO 261/965 and ASME
// B1.1 (UTS), so the cut/ridge depth used by the Design-tab Thread tool is the
// basic external thread height h = 0.6134 * pitch, and the internal (tapped)
// minor diameter is D1 = D - 1.0825 * pitch (= D - 2*5H/8).
struct ThreadSpec {
    enum class Series { MetricCoarse, MetricFine, UNC, UNF };

    std::string name;              // designation, e.g. "M6", "1/4-20 UNC"
    double      major_diameter_mm; // nominal (crest) diameter
    double      pitch_mm;          // axial advance per turn
    Series      series;

    // 60deg basic external thread height (radial crest-to-root engagement).
    double thread_depth_mm() const { return 0.6134 * pitch_mm; }
    // Internal/tapped minor (tap-drill) diameter for the same nominal thread.
    double minor_diameter_mm() const { return major_diameter_mm - 1.0825 * pitch_mm; }

    bool imperial() const { return series == Series::UNC || series == Series::UNF; }
};

// Full ordered table (metric coarse, metric fine, UNC, UNF) for GUI listing.
const std::vector<ThreadSpec>& thread_standards();

// Exact case-sensitive designation lookup; nullptr if not a known standard.
const ThreadSpec* find_thread_standard(const std::string& name);

} // namespace Slic3r

#endif
