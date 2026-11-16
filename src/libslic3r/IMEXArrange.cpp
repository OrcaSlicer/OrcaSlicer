#include "libslic3r/IMEXArrange.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <libnest2d/common.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/IMEXHelpers.hpp"
#include "libslic3r/IMEXZones.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

namespace {

// The arranger's names for the keep-outs. finish() leaves the fences out when it arranges a
// plate inside its zone, where the zone itself is the bed shape.
constexpr const char* kFenceName = "IMEXOutsidePrimaryZone";
constexpr const char* kStripName = "IMEXCollisionZone";

// Each plate computes its zones from where it sits, so the same zone differs in the last bits.
bool same_zone(const BoundingBoxf& a, const BoundingBoxf& b)
{
    return is_approx(a.min, b.min) && is_approx(a.max, b.max);
}

BoundingBox inset(BoundingBox box, const Point& by)
{
    box.min += by;
    box.max -= by;
    return box;
}

// The parts of `bed` outside `zone`: full-depth boxes left and right of it, and the pieces in
// front of and behind it. Empty pieces are left out.
std::vector<BoundingBox> boxes_around(const BoundingBox& bed, const BoundingBox& zone)
{
    std::vector<BoundingBox> out;
    auto add = [&out](coord_t x0, coord_t y0, coord_t x1, coord_t y1) {
        if (x1 > x0 && y1 > y0)
            out.emplace_back(Point(x0, y0), Point(x1, y1));
    };
    const coord_t x0 = std::max(zone.min.x(), bed.min.x());
    const coord_t x1 = std::min(zone.max.x(), bed.max.x());
    add(bed.min.x(), bed.min.y(), zone.min.x(), bed.max.y());
    add(zone.max.x(), bed.min.y(), bed.max.x(), bed.max.y());
    add(x0, bed.min.y(), x1, zone.min.y());
    add(x0, zone.max.y(), x1, bed.max.y());
    return out;
}

} // namespace

std::optional<ImexArrangeZones> imex_arrange_zones(const ImexZoneLayout& layout)
{
    if (!layout.primary_zone_box)
        return std::nullopt;
    ImexArrangeZones zones{*layout.primary_zone_box, {}};
    for (const BoundingBoxf3& strip : layout.collision_zones)
        zones.collision_zones.emplace_back(Vec2d(strip.min.head<2>()), Vec2d(strip.max.head<2>()));
    return zones;
}

void ImexArrangeInput::read_config(const DynamicPrintConfig& full_config)
{
    // A plate the arrange adds has no mode of its own, so it takes the process preset's.
    added_plate.reset();
    if (adds_plates) {
        const auto* mode = full_config.option<ConfigOptionString>("imex_parallel_mode");
        added_plate      = imex_arrange_zones(compute_imex_zone_layout(full_config, kImexPrimaryMode, mode ? mode->value : std::string(),
                                                                       get_extents(full_config.opt<ConfigOptionPoints>("printable_area")->values)));
    }

    // An estimate: auto brim picks its own width, which brim_width stands in for.
    const auto* brim_type = full_config.option<ConfigOptionEnum<BrimType>>("brim_type");
    brim = !brim_type || brim_type->value == btNoBrim || brim_type->value == btInnerOnly ?
               0. :
               full_config.opt_float("brim_width") + full_config.opt_float("brim_object_gap");
}

ImexArranger::ImexArranger(const ImexArrangeInput& input, const arrangement::ArrangeParams& params, const Points& bed)
    : m_bed(bed)
    , m_bed_bb(bed)
    , m_margin(scaled(std::max(0.f, params.bed_shrink_x)), scaled(std::max(0.f, params.bed_shrink_y)))
    , m_adds_plates(input.adds_plates)
{
    for (size_t i = 0; i < input.beds.size(); ++i)
        if (input.beds[i])
            m_beds.push_back({int(i), *input.beds[i]});

    // The bed shape can be a zone only when every plate being arranged has it, and so does every
    // plate the arrange may add.
    const auto& first = input.beds.empty() ? std::nullopt : input.beds.front();
    if (first &&
        std::all_of(input.beds.begin(), input.beds.end(),
                    [&](const std::optional<ImexArrangeZones>& b) { return b && same_zone(b->primary_zone, first->primary_zone); }) &&
        (!input.adds_plates || (input.added_plate && same_zone(input.added_plate->primary_zone, first->primary_zone))))
        m_shared_room = room_in(first->primary_zone);

    // The beds past the plates being arranged become new plates.
    if (input.adds_plates && input.added_plate)
        for (int bed_idx = int(input.beds.size()); bed_idx < MAX_NUM_PLATES; ++bed_idx)
            m_beds.push_back({bed_idx, *input.added_plate});

    // A zone narrower than its margins leaves no room, and its fences then fill the whole bed.
    const Point strip_margin = m_margin + Point::Constant(scaled(input.brim));
    for (const Bed& b : m_beds) {
        if (!m_shared_room)
            for (const BoundingBox& box : boxes_around(m_bed_bb, inset(scaled(b.zones.primary_zone), m_margin)))
                m_keep_outs.push_back({b.bed_idx, box, true});
        for (const BoundingBoxf& strip : b.zones.collision_zones)
            m_keep_outs.push_back({b.bed_idx, inset(scaled(strip), -strip_margin), false});
    }
}

std::optional<BoundingBox> ImexArranger::room_in(const BoundingBoxf& zone) const
{
    BoundingBox room = inset(scaled(zone), m_margin);
    room.min         = room.min.cwiseMax(m_bed_bb.min);
    room.max         = room.max.cwiseMin(m_bed_bb.max);
    if (room.min.x() >= room.max.x() || room.min.y() >= room.max.y())
        return std::nullopt;
    return room;
}

Points ImexArranger::bed_shape() const { return m_shared_room ? m_shared_room->polygon().points : m_bed; }

void ImexArranger::add_keep_outs(arrangement::ArrangePolygons& fixed) const
{
    for (const KeepOut& keep_out : m_keep_outs) {
        arrangement::ArrangePolygon ap;
        ap.poly.contour   = keep_out.box.polygon();
        ap.is_virt_object = true;
        ap.bed_idx        = keep_out.bed_idx;
        ap.height         = 1;
        ap.name           = keep_out.fence ? kFenceName : kStripName;
        fixed.push_back(std::move(ap));
    }
}

bool ImexArranger::crosses_keep_out(const arrangement::ArrangePolygon& part, int bed_idx) const
{
    const Polygon hull = part.transformed_poly().contour;
    return std::any_of(m_keep_outs.begin(), m_keep_outs.end(), [&](const KeepOut& keep_out) {
        return keep_out.bed_idx == bed_idx && !intersection(hull, keep_out.box.polygon()).empty();
    });
}

void ImexArranger::finish(arrangement::ArrangePolygons&       items,
                          const arrangement::ArrangePolygons& before,
                          const arrangement::ArrangePolygons& fixed,
                          const arrangement::ArrangeParams&   params) const
{
    for (arrangement::ArrangePolygon& part : items)
        if (crosses_keep_out(part, part.bed_idx))
            part.bed_idx = arrangement::UNARRANGED;

    // Only fenced plates need centering, and arranging one plate never fences.
    if (m_shared_room || !m_adds_plates)
        return;

    arrangement::ArrangeParams one_plate = params;
    one_plate.progressind                = {};
    for (const Bed& b : m_beds) {
        const std::optional<BoundingBox> room = room_in(b.zones.primary_zone);
        std::vector<size_t>              on_bed;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].bed_idx == b.bed_idx)
                on_bed.push_back(i);
        if (on_bed.empty() || !room)
            continue;

        arrangement::ArrangePolygons parts, bed_fixed;
        // Bed 0, where the nester takes a part as placeable rather than as one that does not fit.
        for (size_t i : on_bed) {
            parts.push_back(before[i]);
            parts.back().bed_idx = 0;
        }
        for (const arrangement::ArrangePolygon& ap : fixed)
            if (ap.bed_idx == b.bed_idx && ap.name != kFenceName) {
                bed_fixed.push_back(ap);
                bed_fixed.back().bed_idx = 0;
            }
        arrangement::arrange(parts, bed_fixed, room->polygon().points, one_plate);
        // A canceled pass leaves parts it never reached on bed 0 with their old ids.
        if (one_plate.stopcondition && one_plate.stopcondition())
            return;

        // Keep the first layout when this one does not fit.
        if (!std::all_of(parts.begin(), parts.end(), [&](const arrangement::ArrangePolygon& part) {
                return part.bed_idx == 0 && !crosses_keep_out(part, b.bed_idx);
            }))
            continue;

        // By-object printing follows the packing order, which this pass numbered 0..n-1: carry
        // it over onto the ids the first pass gave these parts.
        std::vector<int> ids;
        for (size_t i : on_bed)
            ids.push_back(items[i].itemid);
        std::sort(ids.begin(), ids.end());
        for (size_t k = 0; k < on_bed.size(); ++k) {
            items[on_bed[k]].translation = parts[k].translation;
            items[on_bed[k]].rotation    = parts[k].rotation;
            items[on_bed[k]].itemid      = ids[parts[k].itemid];
        }
    }
}

} // namespace Slic3r
