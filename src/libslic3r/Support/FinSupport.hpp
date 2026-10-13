#pragma once

namespace Slic3r {
class PrintObject;
class PrintRegion;

struct FinSupportDimensions {
    double contact_width;
    double thickness;
};

FinSupportDimensions fin_support_dimensions(const PrintObject &object, const PrintRegion &region);

// Build vertically projected fin polygons with closed outlines, painted contacts and filled feet.
void generate_fin_support(PrintObject &object);
}
