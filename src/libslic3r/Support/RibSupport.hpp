#pragma once

namespace Slic3r {
class PrintObject;
class PrintRegion;

struct RibSupportDimensions {
    double contact_width;
    double thickness;
};

RibSupportDimensions rib_support_dimensions(const PrintObject &object, const PrintRegion &region);

// Build vertically projected rib polygons with closed outlines, painted contacts and filled feet.
void generate_rib_support(PrintObject &object);
}
