// Synthetic dependency stand-ins around the exact production source excerpt.
// No slicing, actual polygon containment, retraction emission, or lift is tested.
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

enum ExtrusionRole { erInternalInfill, erPerimeter, erSupportMaterial, erSupportTransition };
enum SupportType { stNormal, stInnerTree };

bool is_perimeter(ExtrusionRole role) { return role == erPerimeter; }

struct Polyline {};

struct Layer {
    virtual ~Layer() = default;
};

struct Area {
    bool contains_travel;
    bool contains(const Polyline&) const { return contains_travel; }
};

using ExPolygon = Area;

struct SupportLayer : Layer {
    std::vector<ExPolygon> support_islands;
    std::vector<Area> base_areas;
    SupportType support_type = stNormal;
};

struct Print {
    struct SliceContext {
        bool disable_interference_travel = false;
    };

    SliceContext context;
    const SliceContext& slice_context() const { return context; }
};

struct Config {
    bool reduce_infill_retraction = false;
    struct { double value = 15; } sparse_infill_density;
};

struct InternalRegions {
    bool contains_travel = false;
    bool travel_inside_internal_regions(const Layer&, const Polyline&) const { return contains_travel; }
};

struct GCode {
    Config m_config;
    const Layer* m_layer = nullptr;
    const Print* m_print = nullptr;
    InternalRegions m_retract_when_crossing_perimeters;
    bool reaches_retraction_selection(const Polyline& travel, ExtrusionRole role);
};

bool GCode::reaches_retraction_selection(const Polyline& travel, ExtrusionRole role)
{
    // SOURCE_EXCERPT
    // Reaching this sentinel means the excerpt did not take an early return.
    // The actual retraction/lift selector and its prerequisites are outside scope.
    return true;
}

int main(int argc, char** argv)
{
    if (argc != 4)
        return EXIT_FAILURE;

    const std::string geometry = argv[1];
    Print print;
    print.context.disable_interference_travel = std::string(argv[2]) == "1";

    Layer model;
    SupportLayer support;
    ExtrusionRole role = erSupportMaterial;
    if (geometry == "normal") {
        support.support_islands.push_back({true});
    } else if (geometry == "tree") {
        support.support_type = stInnerTree;
        support.support_islands.push_back({false});
        support.base_areas.push_back({true});
        role = erSupportTransition;
    } else if (geometry == "outside-normal") {
        support.support_islands.push_back({false});
    } else if (geometry == "outside-tree") {
        support.support_type = stInnerTree;
        support.support_islands.push_back({false});
        support.base_areas.push_back({false});
        role = erSupportTransition;
    } else if (geometry == "infill") {
        role = erInternalInfill;
    } else {
        return EXIT_FAILURE;
    }

    GCode code;
    code.m_config.reduce_infill_retraction = std::string(argv[3]) == "1";
    code.m_layer = geometry == "infill" ? &model : &support;
    code.m_print = &print;
    code.m_retract_when_crossing_perimeters.contains_travel = geometry == "infill";
    std::cout << (code.reaches_retraction_selection(Polyline{}, role) ? "selection" : "skip") << '\n';
}
