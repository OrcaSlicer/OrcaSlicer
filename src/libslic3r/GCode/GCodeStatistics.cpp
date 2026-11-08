#include "GCodeStatistics.hpp"

#include "GCodeProcessor.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r_version.h"

#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <ios>
#include <map>
#include <math.h>
#include <string>
#include <vector>

namespace Slic3r {

namespace {

using json = nlohmann::ordered_json;

double rounded(double value, int decimals)
{
    const double scale = std::pow(10., decimals);
    return std::round(value * scale) / scale;
}

// As the object masses weigh it, at the default density for a filament without one.
double weight_of(const GCodeProcessorResult &result, size_t filament, double volume)
{
    const bool has_density = filament < result.filament_densities.size() && result.filament_densities[filament] > 0.f;
    // mm³ at g/cm³ weigh mg.
    return 0.001 * volume * (has_density ? result.filament_densities[filament] : DEFAULT_FILAMENT_DENSITY);
}

struct Usage
{
    double time{ 0. };
    double volume{ 0. };
    double weight{ 0. };

    void add(const Usage &other)
    {
        time += other.time;
        volume += other.volume;
        weight += other.weight;
    }

    json to_json() const { return json{ { "time", rounded(time, 3) }, { "volume", rounded(volume, 3) }, { "weight", rounded(weight, 4) } }; }
};

json features_json(const std::map<ExtrusionRole, Usage> &features)
{
    json out = json::object();
    for (const auto &[role, usage] : features)
        out[ExtrusionEntity::role_to_string(role)] = usage.to_json();
    return out;
}

} // namespace

std::string gcode_statistics_json(const GCodeProcessorResult &result)
{
    constexpr size_t normal = static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal);

    struct Layer
    {
        float                          z{ 0.f };
        double                         travel_time{ 0. };
        Usage                          total;
        std::map<ExtrusionRole, Usage> features;
    };
    std::vector<Layer>             layers;
    std::map<ExtrusionRole, Usage> features;
    double                         travel_time = 0.;

    // As the preview sums them: a layer takes the time of all its moves, a feature that of its extrusions.
    for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
        if (layers.size() <= move.layer_id)
            layers.resize(move.layer_id + 1);
        Layer       &layer = layers[move.layer_id];
        const double time  = move.time[normal];
        layer.total.time += time;
        if (move.type == EMoveType::Travel) {
            layer.travel_time += time;
            travel_time += time;
        }
        // A vertex added inside a move to show its acceleration repeats the move's extrusion.
        if (move.type != EMoveType::Extrude || move.internal_only)
            continue;
        if (move.extrusion_role != erCustom)
            layer.z = move.position.z();
        Usage usage;
        usage.time   = time;
        usage.volume = double(move.delta_extruder) * M_PI * sqr(0.5 * double(result.filament_diameters[move.extruder_id]));
        usage.weight = weight_of(result, move.extruder_id, usage.volume);
        layer.total.volume += usage.volume;
        layer.total.weight += usage.weight;
        layer.features[move.extrusion_role].add(usage);
        features[move.extrusion_role].add(usage);
    }

    // As the preview's filament table, which counts the flush too.
    const PrintEstimatedStatistics &stats  = result.print_statistics;
    double                          volume = 0., weight = 0., cost = 0.;
    json                            filaments_json = json::array();
    for (const auto &entry : stats.total_volumes_per_extruder) {
        const size_t filament = entry.first;
        const auto   use      = [&result, filament](const std::map<size_t, double> &volumes) {
            const auto   it   = volumes.find(filament);
            const double used = it == volumes.end() ? 0. : it->second;
            return json{ { "volume", rounded(used, 3) }, { "weight", rounded(weight_of(result, filament, used), 4) } };
        };
        const double filament_weight = weight_of(result, filament, entry.second);
        // Priced per kilogram.
        const double filament_cost = filament < result.filament_costs.size() ? 0.001 * filament_weight * result.filament_costs[filament] : 0.;
        volume += entry.second;
        weight += filament_weight;
        cost += filament_cost;
        filaments_json.push_back(json{ { "id", filament + 1 },
                                       { "length", rounded(entry.second / (M_PI * sqr(0.5 * double(result.filament_diameters[filament]))), 2) },
                                       { "volume", rounded(entry.second, 3) },
                                       { "weight", rounded(filament_weight, 4) },
                                       { "cost", rounded(filament_cost, 2) },
                                       { "model", use(stats.model_volumes_per_extruder) },
                                       { "support", use(stats.support_volumes_per_extruder) },
                                       { "prime_tower", use(stats.wipe_tower_volumes_per_extruder) },
                                       { "flush", use(stats.flush_per_filament) } });
    }

    json summary = json{ { "time", rounded(stats.modes[normal].time, 3) },
                         { "prepare_time", rounded(stats.modes[normal].prepare_time, 3) },
                         { "travel_time", rounded(travel_time, 3) },
                         { "travel_distance", rounded(stats.total_travel_distance, 2) },
                         { "layer_count", layers.size() },
                         { "volume", rounded(volume, 3) },
                         { "weight", rounded(weight, 4) },
                         { "filament_cost", rounded(cost, 2) },
                         { "filament_changes", stats.total_filament_changes },
                         { "filaments", filaments_json },
                         { "features", features_json(features) } };

    json layers_json = json::array();
    for (const Layer &layer : layers)
        layers_json.push_back(json{ { "z", rounded(layer.z, 3) },
                                    { "time", rounded(layer.total.time, 3) },
                                    { "travel_time", rounded(layer.travel_time, 3) },
                                    { "volume", rounded(layer.total.volume, 3) },
                                    { "weight", rounded(layer.total.weight, 4) },
                                    { "features", features_json(layer.features) } });

    // The supports of an instance are numbered as the instance.
    json objects_json = json::array();
    for (size_t i = 0; i < result.object_masses.size(); ++i) {
        const GCodeProcessorResult::ObjectMass      &object  = result.object_masses[i];
        const GCodeProcessorResult::ObjectMass::Sum  part    = object.total();
        const GCodeProcessorResult::ObjectMass::Sum  support = i < result.support_masses.size() ? result.support_masses[i].total() :
                                                                                                  GCodeProcessorResult::ObjectMass::Sum{};
        // In mg, as volumes in mm³ at g/cm³.
        objects_json.push_back(json{ { "name", object.name },
                                     { "instance", object.instance },
                                     { "volume", rounded(part.volume, 3) },
                                     { "weight", rounded(0.001 * part.mass, 4) },
                                     { "support_volume", rounded(support.volume, 3) },
                                     { "support_weight", rounded(0.001 * support.mass, 4) } });
    }

    // The version goes up only when a key is removed, renamed or changes meaning.
    const json out = json{ { "version", 1 },
                           { "generator", std::string(SLIC3R_APP_NAME) + " " + SoftFever_VERSION },
                           { "units", json{ { "time", "s" }, { "length", "mm" }, { "volume", "mm3" }, { "weight", "g" } } },
                           { "summary", summary },
                           { "layers", layers_json },
                           { "objects", objects_json } };
    return out.dump(2);
}

std::string gcode_statistics_path(const std::string &gcode_path)
{
    return boost::filesystem::path(gcode_path).replace_extension(".statistics.json").string();
}

void export_gcode_statistics(const GCodeProcessorResult &result, const std::string &path)
{
    // Binary, so the file reads the same on every platform and inside a 3MF.
    boost::nowide::ofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    file << gcode_statistics_json(result);
    file.close();
    if (file.fail())
        throw ExportError(format(_u8L("Failed to write the G-code statistics to %1%."), path));
}

} // namespace Slic3r
