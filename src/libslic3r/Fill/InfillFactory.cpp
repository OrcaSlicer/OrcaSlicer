#include "InfillFactory.hpp"

#include "Fill/InfillPurpose.hpp"
#include "FillBase.hpp"
#include "Fill3DHoneycomb.hpp"
#include "FillAdaptive.hpp"
#include "FillConcentric.hpp"
#include "FillCrossHatch.hpp"
#include "FillGyroid.hpp"
#include "FillHoneycomb.hpp"
#include "FillLightning.hpp"
#include "FillLine.hpp"
#include "FillPlanePath.hpp"
#include "FillRectilinear.hpp"
#include "FillSpiralInset.hpp"
#include "FillTpmsD.hpp"
#include "FillTpmsFK.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {

// Surface purposes a built-in pattern is offered for. Named to keep the per-pattern switch below
// readable; builtin_surfaces() expands each set in the same order the pattern settings use.
enum class BuiltinSurfaceSet
{
    Sparse,
    Solid,
    All,
};

// A built-in filler's complete metadata: stable config key, display label and supported surfaces.
// The label is an L() marker like the PrintConfig enum labels it replaces, so it stays translatable
// and keeps its existing translations.
struct BuiltinFillInfo
{
    // Empty for an internal-only fill.
    std::string_view   id;
    std::string_view   name;
    BuiltinSurfaceSet  surfaces{BuiltinSurfaceSet::Sparse};
};

// The single explicit mapping from a built-in InfillPattern to its config key, display label and
// supported surfaces. This is the source of truth for built-in metadata; it deliberately does not
// infer any of it from PrintConfig or from the enum map at runtime.
BuiltinFillInfo builtin_fill_info(const InfillPattern pattern)
{
    switch (pattern) {
    // Surface-only patterns: offered by the internal solid, top and bottom settings.
    case ipMonotonic:           return { "monotonic",         L("Monotonic"),           BuiltinSurfaceSet::Solid };
    case ipMonotonicLine:       return { "monotonicline",     L("Monotonic line"),      BuiltinSurfaceSet::Solid };
    // Patterns offered by all four pattern settings.
    case ipRectilinear:         return { "rectilinear",       L("Rectilinear"),         BuiltinSurfaceSet::All };
    case ipAlignedRectilinear:  return { "alignedrectilinear", L("Aligned Rectilinear"), BuiltinSurfaceSet::All };
    // Sparse-only patterns: offered by the sparse infill setting alone.
    case ipZigZag:              return { "zigzag",            L("Zig Zag"),             BuiltinSurfaceSet::Sparse };
    case ipCrossZag:            return { "crosszag",          L("Cross Zag"),           BuiltinSurfaceSet::Sparse };
    case ipLockedZag:           return { "lockedzag",         L("Locked Zag"),          BuiltinSurfaceSet::Sparse };
    case ipLine:                return { "line",              L("Line"),                BuiltinSurfaceSet::Sparse };
    case ipGrid:                return { "grid",              L("Grid"),                BuiltinSurfaceSet::Sparse };
    case ipTriangles:           return { "triangles",         L("Triangles"),           BuiltinSurfaceSet::Sparse };
    case ipStars:               return { "tri-hexagon",       L("Tri-hexagon"),         BuiltinSurfaceSet::Sparse };
    case ipCubic:               return { "cubic",             L("Cubic"),               BuiltinSurfaceSet::Sparse };
    case ipAdaptiveCubic:       return { "adaptivecubic",     L("Adaptive Cubic"),      BuiltinSurfaceSet::Sparse };
    case ipQuarterCubic:        return { "quartercubic",      L("Quarter Cubic"),       BuiltinSurfaceSet::Sparse };
    case ipSupportCubic:        return { "supportcubic",      L("Support Cubic"),       BuiltinSurfaceSet::Sparse };
    case ipLightning:           return { "lightning",         L("Lightning"),           BuiltinSurfaceSet::Sparse };
    case ipHoneycomb:           return { "honeycomb",         L("Honeycomb"),           BuiltinSurfaceSet::Sparse };
    case ip3DHoneycomb:         return { "3dhoneycomb",       L("3D Honeycomb"),        BuiltinSurfaceSet::Sparse };
    case ipLateralHoneycomb:    return { "lateral-honeycomb", L("Lateral Honeycomb"),   BuiltinSurfaceSet::Sparse };
    case ipLateralLattice:      return { "lateral-lattice",   L("Lateral Lattice"),     BuiltinSurfaceSet::Sparse };
    case ipCrossHatch:          return { "crosshatch",        L("Cross Hatch"),         BuiltinSurfaceSet::Sparse };
    case ipTpmsD:               return { "tpmsd",             L("TPMS-D"),              BuiltinSurfaceSet::Sparse };
    case ipTpmsFK:              return { "tpmsfk",            L("TPMS-FK"),             BuiltinSurfaceSet::Sparse };
    case ipGyroid:              return { "gyroid",            L("Gyroid"),              BuiltinSurfaceSet::Sparse };
    // More patterns offered by all four pattern settings.
    case ipConcentric:          return { "concentric",        L("Concentric"),          BuiltinSurfaceSet::All };
    // Surface-only and all-surface patterns that complete the top/bottom dropdown.
    case ipSpiralInset:         return { "spiralinset",       L("Spiral Inset"),        BuiltinSurfaceSet::Solid };
    case ipHilbertCurve:        return { "hilbertcurve",      L("Hilbert Curve"),       BuiltinSurfaceSet::All };
    case ipArchimedeanChords:   return { "archimedeanchords", L("Archimedean Chords"),  BuiltinSurfaceSet::All };
    case ipOctagramSpiral:      return { "octagramspiral",    L("Octagram Spiral"),     BuiltinSurfaceSet::All };
    // Internal-only fills: no pattern setting offers them, so they have no key, label or surfaces.
    case ipSupportBase:
    case ipConcentricInternal:
    case ipCount:
    case ipPlugin:
    default:                    return {};
    }
}

std::vector<InfillSurfacePurpose> builtin_surfaces(const BuiltinSurfaceSet set)
{
    switch (set) {
    case BuiltinSurfaceSet::Sparse: return { InfillSurfacePurpose::Sparse };
    case BuiltinSurfaceSet::Solid:
        return { InfillSurfacePurpose::InternalSolid, InfillSurfacePurpose::TopSurface,
                 InfillSurfacePurpose::BottomSurface };
    case BuiltinSurfaceSet::All:
        return { InfillSurfacePurpose::Sparse, InfillSurfacePurpose::InternalSolid,
                 InfillSurfacePurpose::TopSurface, InfillSurfacePurpose::BottomSurface };
    }
    return {};
}

struct RegistryEntry
{
    std::shared_ptr<InfillInfo> info;
    // Set when the owning capability is disabled or unloaded while a Selection still holds the
    // pattern. A retired entry is hidden from new selections but remains creatable for the jobs
    // that already pinned it, and keeps its provider lease until the last pin is released.
    bool         retired = false;
    std::size_t  selections = 0;
};

std::mutex& registry_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, RegistryEntry>& infills()
{
    static std::map<std::string, RegistryEntry> registered;
    return registered;
}

// Register one built-in filler under the key builtin_fill_info() gives its enum.
template<typename T> void register_builtin(const InfillPattern pattern)
{
    const BuiltinFillInfo info = builtin_fill_info(pattern);
    if (info.id.empty())
        return;
    InfillFactory::register_infill(std::string(info.id), std::string(info.name),
                                   builtin_surfaces(info.surfaces),
                                   []() -> std::unique_ptr<Fill> { return std::make_unique<T>(); });
}

void register_all_builtin_patterns()
{
    // One line per built-in filler. builtin_fill_info() owns each enum's key, label and surface
    // purposes, so a pattern's metadata is never inferred from PrintConfig at runtime.
    register_builtin<FillMonotonic>(ipMonotonic);
    register_builtin<FillMonotonicLines>(ipMonotonicLine);
    register_builtin<FillRectilinear>(ipRectilinear);
    register_builtin<FillAlignedRectilinear>(ipAlignedRectilinear);
    register_builtin<FillZigZag>(ipZigZag);
    register_builtin<FillCrossZag>(ipCrossZag);
    register_builtin<FillLockedZag>(ipLockedZag);
    register_builtin<FillLine>(ipLine);
    register_builtin<FillGrid>(ipGrid);
    register_builtin<FillTriangles>(ipTriangles);
    register_builtin<FillStars>(ipStars);
    register_builtin<FillCubic>(ipCubic);
    register_builtin<FillQuarterCubic>(ipQuarterCubic);
    register_builtin<FillLightning::Filler>(ipLightning);
    register_builtin<FillHoneycomb>(ipHoneycomb);
    register_builtin<Fill3DHoneycomb>(ip3DHoneycomb);
    register_builtin<FillLateralHoneycomb>(ipLateralHoneycomb);
    register_builtin<FillLateralLattice>(ipLateralLattice);
    register_builtin<FillCrossHatch>(ipCrossHatch);
    register_builtin<FillTpmsD>(ipTpmsD);
    register_builtin<FillTpmsFK>(ipTpmsFK);
    register_builtin<FillGyroid>(ipGyroid);
    register_builtin<FillConcentric>(ipConcentric);
    register_builtin<FillSpiralInset>(ipSpiralInset);
    register_builtin<FillHilbertCurve>(ipHilbertCurve);
    register_builtin<FillArchimedeanChords>(ipArchimedeanChords);
    register_builtin<FillOctagramSpiral>(ipOctagramSpiral);

    // FillAdaptive::Filler implements both cubic patterns, so it is registered once per key with the
    // key's own display label and surfaces.
    register_builtin<FillAdaptive::Filler>(ipAdaptiveCubic);
    register_builtin<FillAdaptive::Filler>(ipSupportCubic);

    // Not registered, because no pattern setting offers them a key:
    //   FillSupportBase        - ipSupportBase is internal-only.
    //   FillConcentricInternal - ipConcentricInternal is internal-only.
    //   FillMonotonicLine      - no InfillPattern creates it; FillMonotonicLines serves ipMonotonicLine.
}

std::once_flag& builtin_registration_once()
{
    static std::once_flag once;
    return once;
}

std::shared_ptr<InfillInfo> make_info(const std::string&    id,
                                      const std::string&    display_name,
                                      const std::string&    plugin_key,
                                      const std::string&    capability_name,
                                      std::vector<InfillSurfacePurpose> supported_surfaces,
                                      std::shared_ptr<void> provider_lease,
                                      InfillFactoryFn       factory)
{
    auto info                = std::make_shared<InfillInfo>();
    info->id                 = id;
    info->display_name       = display_name;
    info->plugin_key         = plugin_key;
    info->capability_name    = capability_name;
    info->supported_surfaces = std::move(supported_surfaces);
    info->factory            = std::move(factory);
    info->provider_lease     = std::move(provider_lease);
    return info;
}

} // namespace

std::string InfillFactory::builtin_infill_key(const InfillPattern pattern)
{
    // The key is also the value the pattern settings store and the key of
    // ConfigOptionEnum<InfillPattern>; builtin_fill_info() owns that mapping.
    return std::string(builtin_fill_info(pattern).id);
}

InfillFactory::Selection::Selection(Selection&& other) noexcept : m_info(std::move(other.m_info))
{
    other.m_info.reset();
}

InfillFactory::Selection& InfillFactory::Selection::operator=(Selection&& other) noexcept
{
    if (this != &other) {
        reset();
        m_info = std::move(other.m_info);
        other.m_info.reset();
    }
    return *this;
}

InfillFactory::Selection::~Selection() { reset(); }

const std::string& InfillFactory::Selection::id() const
{
    static const std::string empty;
    return m_info ? m_info->id : empty;
}

void InfillFactory::Selection::reset()
{
    if (!m_info)
        return;
    // Keep the provider leased until the pin is gone, then release both. release_selection() may
    // erase a retired entry; the entry that m_info still holds keeps the provider alive until this
    // object drops it, so no other thread can observe a half-released pair.
    const std::string id = m_info->id;
    InfillFactory::release_selection(id);
    m_info.reset();
}

void InfillFactory::register_all_infills()
{
    // Registration stays lazy so a namespace-scope probe such as Fill::use_bridge_flow() cannot
    // build the registry during static initialization. Registry lookups reach it on first use, and
    // call_once keeps an explicit call and the lazy path idempotent.
    std::call_once(builtin_registration_once(), register_all_builtin_patterns);
}

bool InfillFactory::register_infill(const std::string& id,
                                    const std::string& display_name,
                                    std::vector<InfillSurfacePurpose> supported_surfaces,
                                    InfillFactoryFn    infill_factory)
{
    if (id.empty() || !infill_factory)
        return false;

    std::lock_guard<std::mutex> lock(registry_mutex());
    auto&                       registered = infills();
    auto                        it         = registered.find(id);
    if (it != registered.end())
        return false;

    registered.emplace(id, RegistryEntry{make_info(id, display_name, {}, {}, std::move(supported_surfaces), {},
                                                   std::move(infill_factory)),
                                         false, 0});
    return true;
}

bool InfillFactory::register_plugin_infill(const std::string&    id,
                                           const std::string&    display_name,
                                           const std::string&    plugin_key,
                                           const std::string&    capability_name,
                                           std::vector<InfillSurfacePurpose> supported_surfaces,
                                           std::shared_ptr<void> provider_lease,
                                           InfillFactoryFn       infill_factory)
{
    if (id.empty() || plugin_key.empty() || capability_name.empty() || supported_surfaces.empty() ||
        !infill_factory)
        return false;
    // Built-in keys are reserved even before the lazy built-in registration has run.
    if (is_builtin_infill_pattern_key(id))
        return false;

    std::lock_guard<std::mutex> lock(registry_mutex());
    auto&                       registered = infills();
    auto                        it         = registered.find(id);
    if (it != registered.end()) {
        const RegistryEntry& existing = it->second;
        if (!existing.info || !existing.info->is_plugin() ||
            existing.info->plugin_key != plugin_key ||
            existing.info->capability_name != capability_name)
            return false;

        // Same provider publishing again: a repeated sync, or a capability re-enabled while a job
        // still pins its retired entry. Keep the pin count so the already-validated job keeps
        // exactly this entry, and make the ID selectable again. A different provider instance
        // cannot reach this branch: the loader rejects a pattern ID that is still registered before
        // it commits a new package.
        it->second.info = make_info(id, display_name, plugin_key, capability_name,
                                    std::move(supported_surfaces), std::move(provider_lease),
                                    std::move(infill_factory));
        it->second.retired = false;
        return true;
    }

    registered.emplace(id, RegistryEntry{make_info(id, display_name, plugin_key, capability_name,
                                                   std::move(supported_surfaces), std::move(provider_lease),
                                                   std::move(infill_factory)),
                                         false, 0});
    return true;
}

void InfillFactory::unregister_plugin_infills(const std::string& plugin_key,
                                              const std::string& capability_name)
{
    if (plugin_key.empty() || capability_name.empty())
        return;

    std::lock_guard<std::mutex> lock(registry_mutex());
    auto&                       registered = infills();
    for (auto it = registered.begin(); it != registered.end();) {
        const RegistryEntry& entry = it->second;
        if (!entry.info || !entry.info->is_plugin() ||
            entry.info->plugin_key != plugin_key ||
            entry.info->capability_name != capability_name) {
            ++it;
            continue;
        }

        if (entry.selections > 0) {
            // Keep the already-validated entry for in-flight jobs, but never offer it to a new
            // one. It is erased by release_selection() once the last pin is gone.
            it->second.retired = true;
            ++it;
        } else {
            it = registered.erase(it);
        }
    }
}

void InfillFactory::rekey_plugin_infills(const std::string& old_plugin_key,
                                         const std::string& new_plugin_key,
                                         const std::string& capability_name)
{
    if (old_plugin_key.empty() || new_plugin_key.empty() || capability_name.empty() ||
        old_plugin_key == new_plugin_key)
        return;

    std::lock_guard<std::mutex> lock(registry_mutex());
    for (auto& [id, entry] : infills()) {
        (void) id;
        if (!entry.info || !entry.info->is_plugin() || entry.info->plugin_key != old_plugin_key ||
            entry.info->capability_name != capability_name)
            continue;

        // Replace the published metadata atomically while keeping the old shared info alive for
        // selections that already validated against the previous project identity. Both point at
        // the same capability/factory; future selections validate against the new owner.
        auto updated = std::make_shared<InfillInfo>(*entry.info);
        updated->plugin_key = new_plugin_key;
        entry.info = std::move(updated);
    }
}

bool InfillFactory::is_infill_registered(const std::string& id)
{
    register_all_infills();
    std::lock_guard<std::mutex> lock(registry_mutex());
    return infills().find(id) != infills().end();
}

bool InfillFactory::has_pinned_infills(const std::string& plugin_key, const std::string& capability_name)
{
    if (plugin_key.empty() || capability_name.empty())
        return false;

    std::lock_guard<std::mutex> lock(registry_mutex());
    for (const auto& entry : infills())
        if (entry.second.selections > 0 && entry.second.info && entry.second.info->is_plugin() &&
            entry.second.info->plugin_key == plugin_key &&
            entry.second.info->capability_name == capability_name)
            return true;
    return false;
}

std::shared_ptr<const InfillInfo> InfillFactory::find_infill(const std::string& id)
{
    register_all_infills();
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto&                       registered = infills();
    auto                        it         = registered.find(id);
    if (it == registered.end() || it->second.retired)
        return {};
    return it->second.info;
}

std::vector<InfillInfo> InfillFactory::get_registered_infills()
{
    register_all_infills();
    std::lock_guard<std::mutex> lock(registry_mutex());

    std::vector<InfillInfo> result;
    result.reserve(infills().size());
    for (const auto& entry : infills())
        if (!entry.second.retired && entry.second.info)
            result.push_back(*entry.second.info);
    return result;
}

std::unique_ptr<Fill> InfillFactory::create_infill_by_id(const std::string& id)
{
    register_all_infills();

    InfillFactoryFn factory;
    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        auto&                       registered = infills();
        auto                        it         = registered.find(id);
        if (it == registered.end() || !it->second.info)
            return nullptr;
        // A retired entry is still creatable: a job that pinned it before unload is allowed to try,
        // and its weak capability reference fails cleanly if the provider is already gone.
        factory = it->second.info->factory;
    }

    // A factory may itself query the registry; run it outside the lock.
    return factory ? factory() : nullptr;
}

InfillFactory::Selection InfillFactory::select_infill(const std::string& id,
                                                      const std::string& plugin_key,
                                                      const std::string& capability_name,
                                                      const std::vector<InfillSurfacePurpose>& purposes,
                                                      std::string&       error)
{
    register_all_infills();

    std::lock_guard<std::mutex> lock(registry_mutex());
    auto&                       registered = infills();
    auto                        it         = registered.find(id);
    if (it == registered.end()) {
        error = "Infill pattern '" + id + "' is not available";
        return {};
    }

    RegistryEntry& entry = it->second;
    if (!entry.info || entry.retired) {
        error = "Infill pattern '" + id + "' is no longer available";
        return {};
    }
    if (!entry.info->is_plugin()) {
        error = "Infill pattern '" + id + "' is not plugin-provided";
        return {};
    }
    if (entry.info->plugin_key != plugin_key || entry.info->capability_name != capability_name) {
        error = "Infill pattern '" + id + "' is not owned by the referenced plugin capability";
        return {};
    }
    if (purposes.empty()) {
        error = "Infill pattern '" + id + "' was selected without a surface purpose";
        return {};
    }
    for (const InfillSurfacePurpose purpose : purposes) {
        if (!entry.info->supports_surface(purpose)) {
            error = "Infill pattern '" + id + "' was not advertised for this surface";
            return {};
        }
    }

    ++entry.selections;
    error.clear();
    return Selection(entry.info);
}

void InfillFactory::release_selection(const std::string& id)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto&                       registered = infills();
    auto                        it         = registered.find(id);
    if (it == registered.end())
        return;

    RegistryEntry& entry = it->second;
    if (entry.selections > 0)
        --entry.selections;
    if (entry.selections == 0 && entry.retired)
        registered.erase(it);
}

} // namespace Slic3r
