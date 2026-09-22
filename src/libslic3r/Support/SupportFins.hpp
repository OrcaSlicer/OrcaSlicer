#pragma once

#include <cstddef>
#include <vector>

namespace Slic3r {

class PrintObject;

// For each object layer, whether its fins get tines: support_fin_tine_spacing apart, with extra rows
// just above the foot when support_fin_tine_base_rows is set. The layers on the bed never get tines.
std::vector<bool> fin_tine_layers(const PrintObject &object);

// Support type "fins": generates breakaway fins for the object and stores them as its support layers.
// See SupportFins.cpp for the geometry. Returns true if some fin is slender enough to fail mid-print.
bool generate_fin_support(PrintObject &object);

} // namespace Slic3r
