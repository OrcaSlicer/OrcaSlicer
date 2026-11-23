#ifndef slic3r_Fill_hpp_
#define slic3r_Fill_hpp_

#include <cstddef>
#include <memory.h>
#include <float.h>
#include <stdint.h>
#include <string>

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../PrintConfig.hpp"

#include "FillBase.hpp"

namespace Slic3r {

class ExtrusionEntityCollection;
class LayerRegion;
class PrintObject;

// Orca: Share the layer rotation calculation between infill generation and internal
// bridge angle selection so both interpret rotation templates in the same way.
double calculate_infill_rotation_angle(const PrintObject *object, size_t layer_id,
                                      const double &fixed_infill_angle, const std::string &template_string);

// Orca: The parameters that are taken into account when calculating a rotating infill.
struct InfillParams
{
    double angle     = 0.;           // radians
    Vec2d  shift     = Vec2d(0, 0);  // scaled
    double density   = 100.;         // percent
    int    multiline = 1;
};

// The infill parameters that a rotation template sets for a layer.
InfillParams calculate_infill_position_rad(const PrintObject *object, size_t layer_id,
                                           const double &fixed_infill_angle, // the model's direction when a template is used
                                           const std::string &template_string, const double &fixed_infill_density = 100.,
                                           const int &fixed_multiline = 1,
                                           double line_width = 0.); // 0 takes the object's line width

// An interface class to Perl, aggregating an instance of a Fill and a FillData.
class Filler
{
public:
    Filler() : fill(nullptr) {}
    ~Filler() { 
        delete fill; 
        fill = nullptr;
    }
    Fill        *fill;
    FillParams   params;
};

} // namespace Slic3r

#endif // slic3r_Fill_hpp_
