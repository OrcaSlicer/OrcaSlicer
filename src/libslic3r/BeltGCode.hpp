#pragma once

#include "GCode.hpp"
#include "Point.hpp"
#include "Print.hpp"

namespace Slic3r {

// Belt-printer-specific GCode export.
//
// Inherits from GCode and overrides virtual hooks to:
// - Install a BeltKinematics on the GCodeWriter
// - Write belt configuration to the G-code header
// - Adjust the origin for global pre-slice transforms when switching instances
// (Arc fitting is disabled for belt printers by BeltKinematics::supports_arc_moves(),
//  which the base GCode::should_disable_arc_fitting() consults -- no override needed.)
class BeltGCode : public GCode
{
protected:
    void init_belt_writer(Print &print) override;
    void write_belt_header(GCodeOutputStream &file, const Print &print) override;
    void on_set_origin(const PrintObject *obj, const Point &inst_shift) override;
};

} // namespace Slic3r
