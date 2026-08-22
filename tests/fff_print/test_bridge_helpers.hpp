#ifndef SLIC3R_TEST_BRIDGE_HELPERS_HPP
#define SLIC3R_TEST_BRIDGE_HELPERS_HPP

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Surface.hpp"

#include <cmath>
#include <set>
#include <vector>

namespace Slic3r { namespace Test { namespace BridgeHelpers {

// ---------------------------------------------------------------------------
// Role collection helpers
// ---------------------------------------------------------------------------

/// Recursively collect extrusion roles from any ExtrusionEntity tree.
inline void collect_roles_from_entity(const ExtrusionEntity &entity,
                                      std::vector<ExtrusionRole> &out)
{
    if (entity.is_collection()) {
        const auto &coll = static_cast<const ExtrusionEntityCollection &>(entity);
        for (const ExtrusionEntity *child : coll.entities)
            if (child != nullptr)
                collect_roles_from_entity(*child, out);
        return;
    }

    if (entity.is_loop()) {
        const auto &loop = static_cast<const ExtrusionLoop &>(entity);
        for (const ExtrusionPath &path : loop.paths)
            out.push_back(path.role());
        return;
    }

    out.push_back(entity.role());
}

/// Collect all perimeter extrusion roles at a given Z height (within tolerance).
inline std::vector<ExtrusionRole>
collect_perimeter_roles_at_z(const Print &print, double z, double tolerance = 0.02)
{
    std::vector<ExtrusionRole> roles;
    for (const PrintObject *obj : print.objects())
        for (const Layer *layer : obj->layers()) {
            if (std::abs(layer->print_z - z) > tolerance)
                continue;
            for (const LayerRegion *region : layer->regions())
                collect_roles_from_entity(region->perimeters, roles);
        }
    return roles;
}

/// Collect all fill extrusion roles at a given Z height (within tolerance).
inline std::vector<ExtrusionRole>
collect_fill_roles_at_z(const Print &print, double z, double tolerance = 0.02)
{
    std::vector<ExtrusionRole> roles;
    for (const PrintObject *obj : print.objects())
        for (const Layer *layer : obj->layers()) {
            if (std::abs(layer->print_z - z) > tolerance)
                continue;
            for (const LayerRegion *region : layer->regions())
                collect_roles_from_entity(region->fills, roles);
        }
    return roles;
}

/// Return true if the role vector contains erBridgeInfill, erOverhangPerimeter, or erBridgePerimeter.
inline bool contains_bridge_role(const std::vector<ExtrusionRole> &roles)
{
    for (auto role : roles)
        if (role == erBridgeInfill || role == erOverhangPerimeter || role == erBridgePerimeter)
            return true;
    return false;
}

/// Return true if the role vector contains erBridgePerimeter specifically.
inline bool contains_erBridgePerimeter(const std::vector<ExtrusionRole> &roles)
{
    for (auto role : roles)
        if (role == erBridgePerimeter)
            return true;
    return false;
}

/// Return true if the role vector contains erBridgeInfill specifically.
inline bool contains_erBridgeInfill(const std::vector<ExtrusionRole> &roles)
{
    for (auto role : roles)
        if (role == erBridgeInfill)
            return true;
    return false;
}

/// Return true if the role vector contains erInternalBridgeInfill.
inline bool contains_erInternalBridgeInfill(const std::vector<ExtrusionRole> &roles)
{
    for (auto role : roles)
        if (role == erInternalBridgeInfill)
            return true;
    return false;
}

/// Return true if the role vector contains erExternalPerimeter.
inline bool contains_erExternalPerimeter(const std::vector<ExtrusionRole> &roles)
{
    for (auto role : roles)
        if (role == erExternalPerimeter)
            return true;
    return false;
}

/// Return true if the role vector contains erPerimeter.
inline bool contains_erPerimeter(const std::vector<ExtrusionRole> &roles)
{
    for (auto role : roles)
        if (role == erPerimeter)
            return true;
    return false;
}

/// Return true if every role in the vector is erBridgePerimeter.
inline bool all_roles_are_bridge_perimeter(const std::vector<ExtrusionRole> &roles)
{
    if (roles.empty()) return false;
    for (auto role : roles)
        if (role != erBridgePerimeter)
            return false;
    return true;
}

/// Return a set of unique roles for diagnostic purposes.
inline std::set<ExtrusionRole> unique_roles(const std::vector<ExtrusionRole> &roles)
{
    return {roles.begin(), roles.end()};
}

/// Recursively collect the extrusion widths (mm) of all perimeter paths that
/// carry a given role.
inline void collect_widths_for_role(const ExtrusionEntity &entity, ExtrusionRole target,
                                    std::vector<float> &out)
{
    if (entity.is_collection()) {
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection &>(entity).entities)
            if (child != nullptr)
                collect_widths_for_role(*child, target, out);
        return;
    }
    if (entity.is_loop()) {
        for (const ExtrusionPath &path : static_cast<const ExtrusionLoop &>(entity).paths)
            if (path.role() == target)
                out.push_back(path.width);
        return;
    }
}

/// Recursively sum the extrusion length (scaled) of paths with a given role.
inline void collect_role_length(const ExtrusionEntity &entity, ExtrusionRole target, double &len)
{
    if (entity.is_collection()) {
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection &>(entity).entities)
            if (child != nullptr)
                collect_role_length(*child, target, len);
        return;
    }
    if (entity.is_loop()) {
        for (const ExtrusionPath &path : static_cast<const ExtrusionLoop &>(entity).paths)
            if (path.role() == target)
                len += path.length();
    }
}

/// Total extrusion length (scaled) of perimeter paths with a given role at a Z.
inline double perimeter_role_length_at_z(const Print &print, double z, ExtrusionRole role,
                                         double tolerance = 0.02)
{
    double len = 0;
    for (const PrintObject *obj : print.objects())
        for (const Layer *layer : obj->layers()) {
            if (std::abs(layer->print_z - z) > tolerance)
                continue;
            for (const LayerRegion *region : layer->regions())
                collect_role_length(region->perimeters, role, len);
        }
    return len;
}

/// Collect extrusion roles from a layer's gap-fill (LayerRegion::thin_fills) at a Z.
inline std::vector<ExtrusionRole>
collect_thinfill_roles_at_z(const Print &print, double z, double tolerance = 0.02)
{
    std::vector<ExtrusionRole> roles;
    for (const PrintObject *obj : print.objects())
        for (const Layer *layer : obj->layers()) {
            if (std::abs(layer->print_z - z) > tolerance)
                continue;
            for (const LayerRegion *region : layer->regions())
                collect_roles_from_entity(region->thin_fills, roles);
        }
    return roles;
}

/// Count thin-fill (gap-fill) paths with a given role at a Z.
inline size_t count_thinfill_role_at_z(const Print &print, double z, ExtrusionRole role,
                                       double tolerance = 0.02)
{
    auto roles = collect_thinfill_roles_at_z(print, z, tolerance);
    size_t n = 0;
    for (auto r : roles) if (r == role) ++n;
    return n;
}

/// Recursively expand [minx,maxx] (mm) over points of paths with a given role.
inline void collect_role_x_range(const ExtrusionEntity &entity, ExtrusionRole target,
                                 double &minx, double &maxx)
{
    if (entity.is_collection()) {
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection &>(entity).entities)
            if (child != nullptr)
                collect_role_x_range(*child, target, minx, maxx);
        return;
    }
    if (entity.is_loop()) {
        for (const ExtrusionPath &path : static_cast<const ExtrusionLoop &>(entity).paths)
            if (path.role() == target)
                for (const auto &p : path.polyline.points) {
                    double x = unscale<double>(p.x());
                    if (x < minx) minx = x;
                    if (x > maxx) maxx = x;
                }
    }
}

/// X-extent (mm, maxx-minx) of perimeter paths with a given role at a Z.
/// Returns 0 if no such paths exist.
inline double perimeter_role_x_span_at_z(const Print &print, double z, ExtrusionRole role,
                                         double tolerance = 0.02)
{
    double minx = 1e30, maxx = -1e30;
    for (const PrintObject *obj : print.objects())
        for (const Layer *layer : obj->layers()) {
            if (std::abs(layer->print_z - z) > tolerance)
                continue;
            for (const LayerRegion *region : layer->regions())
                collect_role_x_range(region->perimeters, role, minx, maxx);
        }
    return (maxx >= minx) ? (maxx - minx) : 0.0;
}

/// Count perimeter paths carrying a given role across ALL layers/objects.
inline size_t count_all_perimeter_role(const Print &print, ExtrusionRole role)
{
    size_t n = 0;
    for (const PrintObject *obj : print.objects())
        for (const Layer *layer : obj->layers())
            for (const LayerRegion *region : layer->regions()) {
                std::vector<ExtrusionRole> roles;
                collect_roles_from_entity(region->perimeters, roles);
                for (auto r : roles)
                    if (r == role) ++n;
            }
    return n;
}

/// Count perimeter paths carrying a given role at a Z height.
inline size_t count_perimeter_role_at_z(const Print &print, double z, ExtrusionRole role,
                                        double tolerance = 0.02)
{
    auto roles = collect_perimeter_roles_at_z(print, z, tolerance);
    size_t n = 0;
    for (auto r : roles)
        if (r == role)
            ++n;
    return n;
}

/// Collect widths (mm) of perimeter paths with a given role at a Z height.
inline std::vector<float>
collect_perimeter_widths_for_role_at_z(const Print &print, double z, ExtrusionRole role,
                                       double tolerance = 0.02)
{
    std::vector<float> widths;
    for (const PrintObject *obj : print.objects())
        for (const Layer *layer : obj->layers()) {
            if (std::abs(layer->print_z - z) > tolerance)
                continue;
            for (const LayerRegion *region : layer->regions())
                collect_widths_for_role(region->perimeters, role, widths);
        }
    return widths;
}

// ---------------------------------------------------------------------------
// Surface type helpers
// ---------------------------------------------------------------------------

/// Collect all SurfaceType values from fill_surfaces at a given Z height.
inline std::vector<SurfaceType>
collect_surface_types_at_z(const Print &print, double z, double tolerance = 0.02)
{
    std::vector<SurfaceType> types;
    for (const PrintObject *obj : print.objects())
        for (const Layer *layer : obj->layers()) {
            if (std::abs(layer->print_z - z) > tolerance)
                continue;
            for (const LayerRegion *region : layer->regions())
                for (const Surface &surface : region->fill_surfaces.surfaces)
                    types.push_back(surface.surface_type);
        }
    return types;
}

/// Return true if the type vector contains a given SurfaceType.
inline bool contains_surface_type(const std::vector<SurfaceType> &types, SurfaceType target)
{
    for (auto t : types)
        if (t == target)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Dynamic bridge-layer finding
// ---------------------------------------------------------------------------

/// Find the print_z of the first layer that has at least one stBottomBridge
/// fill surface.  Returns -1.0 if none found.
inline double find_first_bridge_z(const Print &print)
{
    for (const PrintObject *obj : print.objects())
        for (const Layer *layer : obj->layers())
            for (const LayerRegion *region : layer->regions())
                for (const Surface &surface : region->fill_surfaces.surfaces)
                    if (surface.surface_type == stBottomBridge)
                        return layer->print_z;
    return -1.0;
}

/// Find the print_z of the first layer that has at least one stInternalBridge
/// fill surface.  Returns -1.0 if none found.
inline double find_first_internal_bridge_z(const Print &print)
{
    for (const PrintObject *obj : print.objects())
        for (const Layer *layer : obj->layers())
            for (const LayerRegion *region : layer->regions())
                for (const Surface &surface : region->fill_surfaces.surfaces)
                    if (surface.surface_type == stInternalBridge)
                        return layer->print_z;
    return -1.0;
}

/// Find the print_z of the first layer whose perimeters contain erBridgePerimeter.
/// Returns -1.0 if none found.
inline double find_first_bridge_perimeter_z(const Print &print)
{
    for (const PrintObject *obj : print.objects())
        for (const Layer *layer : obj->layers())
            for (const LayerRegion *region : layer->regions()) {
                std::vector<ExtrusionRole> roles;
                collect_roles_from_entity(region->perimeters, roles);
                if (contains_erBridgePerimeter(roles))
                    return layer->print_z;
            }
    return -1.0;
}

}}} // namespace Slic3r::Test::BridgeHelpers

#endif // SLIC3R_TEST_BRIDGE_HELPERS_HPP
