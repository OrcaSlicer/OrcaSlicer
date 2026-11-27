#pragma once

#include <string>
#include <vector>

namespace Slic3r {

// One loaded slot of a Creality CFS, as the Creality Hi reports it in Moonraker's `box` printer object.
struct CrealityCfsSlot {
    int         slot_index = 0;
    std::string material_type;
    std::string color;
};

// Parse a /printer/objects/query?box response without querying printer hardware.
// Empty slots, disconnected boxes and unknown material ids are omitted.
bool parse_creality_cfs_response(const std::string& response, std::vector<CrealityCfsSlot>& slots);

} // namespace Slic3r
