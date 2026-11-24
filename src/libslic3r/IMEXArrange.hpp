#pragma once

#include <optional>
#include <vector>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {

class DynamicPrintConfig;
struct ImexZoneLayout;

// A plate's IDEX/IQEX zones as Arrange sees them, in plate-local millimeters.
struct ImexArrangeZones
{
    BoundingBoxf              primary_zone;
    std::vector<BoundingBoxf> collision_zones;
};

// The zones of `layout`, none when it has no primary zone, i.e. outside a parallel mode.
std::optional<ImexArrangeZones> imex_arrange_zones(const ImexZoneLayout& layout);

// What an arrange needs to know about the plates it packs into.
struct ImexArrangeInput
{
    // Per arranger bed, in order: the zones of the plate it packs into, none for a plate whose
    // parts may go anywhere on the bed.
    std::vector<std::optional<ImexArrangeZones>> beds;
    // Whether the beds past `beds` become new plates, as when arranging every plate, and the
    // zones such a plate gets.
    bool                            adds_plates = false;
    std::optional<ImexArrangeZones> added_plate;
    // Room a strip keeps for the brim: brim width plus its gap to the part, in mm.
    double brim = 0.;

    // Sets `added_plate` and `brim` from the full config. Call after setting `adds_plates`.
    void read_config(const DynamicPrintConfig& full_config);
};

// Keeps an arrange inside each IDEX/IQEX plate's primary zone and out of that zone's carriage
// collision strips.
//
// The arranger packs every bed into one bed shape. When every bed has the same primary zone,
// that shape becomes the zone, which keeps the parts centered in it. Otherwise each plate in a
// parallel mode fences off the rest of its bed with fixed items on its own bed, leaving the
// other plates the whole bed, and finish() arranges each fenced plate again inside its zone,
// since a fenced pile sits against the zone edge nearest the bed's center.
//
// A zone edge inside the bed gets the margin the bed's own edges get, since a copy printed from
// a part at the zone edge sits at the far edge of the bed. A strip also gets the brim, which
// would take the nozzle into it.
class ImexArranger
{
public:
    // `bed` is the arranger's bed shape, shrunk by params.bed_shrink_x/y.
    ImexArranger(const ImexArrangeInput& input, const arrangement::ArrangeParams& params, const Points& bed);

    // Whether any bed has a zone. Nothing below has an effect otherwise.
    bool active() const { return !m_beds.empty(); }

    // The bed shape to arrange in.
    Points bed_shape() const;

    // Adds the fixed items that keep parts out of each bed's keep-outs.
    void add_keep_outs(arrangement::ArrangePolygons& fixed) const;

    // Call after arrangement::arrange(items, fixed, bed_shape(), params), with `before` holding
    // `items` as they were before it. Leaves unarranged any part across a keep-out: one that does
    // not fit an empty bed is retried there without the bed's fixed items, so a part too big for
    // its zone comes back across them. Then centers each fenced plate's parts in its zone.
    void finish(arrangement::ArrangePolygons& items, const arrangement::ArrangePolygons& before,
                const arrangement::ArrangePolygons& fixed, const arrangement::ArrangeParams& params) const;

private:
    struct Bed
    {
        int              bed_idx;
        ImexArrangeZones zones;
    };
    struct KeepOut
    {
        int         bed_idx;
        BoundingBox box;
        bool        fence; // outside the primary zone, rather than a strip
    };

    std::optional<BoundingBox> room_in(const BoundingBoxf& zone) const;
    bool                       crosses_keep_out(const arrangement::ArrangePolygon& part, int bed_idx) const;

    Points                     m_bed;
    BoundingBox                m_bed_bb;
    Point                      m_margin;
    bool                       m_adds_plates = false;
    std::vector<Bed>           m_beds;
    std::optional<BoundingBox> m_shared_room;
    std::vector<KeepOut>       m_keep_outs;
};

} // namespace Slic3r
