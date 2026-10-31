#ifndef slic3r_TransmissionDistance_hpp_
#define slic3r_TransmissionDistance_hpp_

#include <string>
#include <vector>

#include "Color.hpp"

namespace Slic3r {

// The default `filament_transmission_distance` carries until somebody sets one. A filament nobody
// has measured reads as this, which is how "not known" is told from "known to be 4".
static constexpr float FILAMENT_TD_DEFAULT_MM = 4.f;

// One filament the shipped table knows a transmission distance for.
//
// A transmission distance is a property of a *colour*, not of a product line: one maker's own values
// for a single line of matte PLA run from 0.1 to 4.1 across its colours. A filament preset is the
// line, and carries no colour - so the preset is the right home for the number and the wrong thing
// to look it up by. The pair that does identify a spool is the preset's vendor and the colour the
// filament is set to, and both of those a project already knows.
struct FilamentTd
{
    std::string brand;   // as the table spells it, lower case
    std::string name;    // the full name, line and colour together
    std::string type;    // PLA, PETG, ...
    ColorRGB    color;
    float       td_mm = 0.f;
};

// The table that ships with OrcaSlicer, read once on first use. Empty when it is missing or will not
// parse, which is not an error: the option it fills in has a working default.
const std::vector<FilamentTd> &filament_td_table();

// The table's best guess at a filament's transmission distance, or nullptr when it has none worth
// offering. `vendor` and `name` come from the filament preset, `color` from the filament itself.
//
// Both halves have to agree: the name has to put the filament in a line the table knows, and the
// colour has to be close enough to one of that line's colours to be that colour. A match on the name
// alone would hand a white filament the number measured for a black one, which is worse than the
// default - it is wrong, and it looks measured.
const FilamentTd *filament_td_lookup(const std::string &vendor, const std::string &name, const ColorRGB &color);

// The same search against a table handed in rather than the shipped one. The matching is the part
// with judgement in it, so it is reachable without a resources directory to read.
const FilamentTd *filament_td_lookup(const std::vector<FilamentTd> &table, const std::string &vendor,
                                     const std::string &name, const ColorRGB &color);

} // namespace Slic3r

#endif // slic3r_TransmissionDistance_hpp_
