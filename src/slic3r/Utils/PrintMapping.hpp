#pragma once

// Typed print mapping for printer agents.
//
// The GUI still produces the mapping as legacy PrintParams strings. This header defines the
// typed contract agents should consume and the single conversion from those legacy strings.
// It does not write or reinterpret the legacy encodings outside that conversion.
//
// Encodings accepted here:
//   ams_mapping       (v0) JSON array of integers. Negative is unmapped; 254/255 are the
//                          virtual deputy/main trays; otherwise the value is a flat tray index.
//   ams_mapping2      (v1) JSON array of {"ams_id", "slot_id"} objects. ams_id 255 with
//                          slot_id 255 is the external-spool/unmapped sentinel, ams_id 254 is
//                          the virtual deputy tray, and ams_id 0..253 with a slot is an AMS slot.
//   ams_mapping_info       JSON array of per-filament objects:
//                          ams, targetColor, filamentId, filamentType, nozzleId, sourceColor.
//                          Entries are matched to mapping entries by array index.
//   nozzle_mapping         JSON array of integers, index -> physical nozzle/tool id.
//   nozzles_info           JSON array of {id, type, flowSize, diameter} objects.
//
// v1 (ams_mapping2) is authoritative whenever it is present, even when it is an empty array.
// v0 is read only when v1 is absent.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "bambu_networking.hpp" // PrintParams

namespace Slic3r {

// Typed mapping selected for a print. Conversion is non-throwing: malformed or internally
// inconsistent input returns false, leaves `error` non-empty, and clears `out`.
struct PrintMapping
{
    static constexpr int unmapped_id           = -1;
    static constexpr int virtual_deputy_ams_id = 254;
    // Also the external-spool/unmapped sentinel when paired with slot_id 255 in v1.
    static constexpr int virtual_main_ams_id   = 255;

    // Optional destination of one filament/tool in the mapping.
    enum class DestinationKind
    {
        unmapped,
        external_spool,
        ams_slot,
    };

    struct Destination
    {
        DestinationKind kind = DestinationKind::unmapped;
        // ams_slot: AMS unit id, except for v0 where the encoding only carries a flat tray index
        // and ams_id stays unmapped_id. external_spool: the virtual tray id (254 or 255).
        int ams_id = unmapped_id;
        // ams_slot: AMS slot id, or the flat tray index for v0. external_spool/unmapped: unmapped_id.
        int slot_id = unmapped_id;
    };

    // Per-filament metadata carried by ams_mapping_info. Fields absent from the JSON stay empty or
    // unset; `ams` keeps the legacy flat tray index semantics (-1 unmapped, 254/255 virtual trays).
    struct FilamentMetadata
    {
        int                ams = unmapped_id;
        std::string        target_color;
        std::string        filament_id;
        std::string        filament_type;
        std::optional<int> nozzle_id;
        std::string        source_color;
    };

    struct Filament
    {
        std::size_t                     filament_index = 0;
        Destination                     destination;
        std::optional<FilamentMetadata> metadata;
    };

    // One entry of nozzle_mapping. The index is a filament id for the v0 nozzle mapping and a
    // logical nozzle id for the v1 nozzle mapping; the array length and index meaning are the
    // printer's, so both are preserved verbatim.
    struct NozzleMapping
    {
        std::size_t index     = 0;
        int         nozzle_id = unmapped_id;
    };

    struct NozzleInfo
    {
        int                        id = unmapped_id;
        std::optional<std::string> type;
        std::optional<std::string> flow_size;
        std::optional<double>      diameter;
    };

    enum class MappingVersion
    {
        none,
        v0,
        v1,
    };

    bool                       use_ams = false;
    MappingVersion             ams_mapping_version = MappingVersion::none;
    std::vector<Filament>      filaments;
    std::vector<NozzleMapping> nozzle_mapping;
    std::vector<NozzleInfo>    nozzles;

    // Single conversion from the legacy PrintParams mapping fields. Returns true on success and
    // clears `error`; returns false and clears `out` on malformed or inconsistent input.
    static bool from_legacy(const PrintParams& params, PrintMapping& out, std::string& error);
};

namespace PrintMappingDetail {

inline bool is_blank(const std::string& text) noexcept
{
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

inline bool read_json(const std::string& text, const char* field, nlohmann::json& out, bool& present, std::string& error)
{
    present = !is_blank(text);
    if (!present)
        return true;

    out = nlohmann::json::parse(text, nullptr, false);
    if (out.is_discarded()) {
        error = std::string(field) + " is not valid JSON";
        return false;
    }
    return true;
}

inline bool json_to_int(const nlohmann::json& value, int& out)
{
    if (!value.is_number_integer())
        return false;

    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<unsigned long long>();
        if (unsigned_value > static_cast<unsigned long long>(std::numeric_limits<int>::max()))
            return false;
        out = static_cast<int>(unsigned_value);
        return true;
    }

    const auto signed_value = value.get<long long>();
    if (signed_value < std::numeric_limits<int>::min() || signed_value > std::numeric_limits<int>::max())
        return false;
    out = static_cast<int>(signed_value);
    return true;
}

inline bool read_string(const nlohmann::json& object, const char* key, std::string& out, const std::string& context, std::string& error)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null())
        return true;
    if (!it->is_string()) {
        error = context + "." + key + " is not a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

inline bool parse_metadata(const nlohmann::json& array, std::vector<PrintMapping::FilamentMetadata>& out, std::string& error)
{
    if (!array.is_array()) {
        error = "ams_mapping_info is not an array";
        return false;
    }

    out.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i) {
        const nlohmann::json& item = array[i];
        const std::string context   = "ams_mapping_info[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            error = context + " is not an object";
            return false;
        }

        PrintMapping::FilamentMetadata metadata;

        if (const auto it = item.find("ams"); it != item.end() && !it->is_null()) {
            if (!json_to_int(*it, metadata.ams)) {
                error = context + ".ams is not an integer";
                return false;
            }
            if (metadata.ams < PrintMapping::unmapped_id || metadata.ams > PrintMapping::virtual_main_ams_id) {
                error = context + ".ams is outside the supported range";
                return false;
            }
        }
        if (!read_string(item, "targetColor", metadata.target_color, context, error))
            return false;
        if (!read_string(item, "filamentId", metadata.filament_id, context, error))
            return false;
        if (!read_string(item, "filamentType", metadata.filament_type, context, error))
            return false;
        if (!read_string(item, "sourceColor", metadata.source_color, context, error))
            return false;

        if (const auto it = item.find("nozzleId"); it != item.end() && !it->is_null()) {
            int nozzle_id = PrintMapping::unmapped_id;
            if (!json_to_int(*it, nozzle_id)) {
                error = context + ".nozzleId is not an integer";
                return false;
            }
            metadata.nozzle_id = nozzle_id;
        }

        out.push_back(std::move(metadata));
    }
    return true;
}

inline bool parse_v1(const nlohmann::json& array, std::vector<PrintMapping::Filament>& out, std::string& error)
{
    if (!array.is_array()) {
        error = "ams_mapping2 is not an array";
        return false;
    }

    out.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i) {
        const nlohmann::json& item = array[i];
        const std::string context   = "ams_mapping2[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            error = context + " is not an object";
            return false;
        }

        const auto ams_it  = item.find("ams_id");
        const auto slot_it = item.find("slot_id");
        if (ams_it == item.end() || slot_it == item.end()) {
            error = context + " is missing ams_id or slot_id";
            return false;
        }

        int ams_id  = PrintMapping::unmapped_id;
        int slot_id = PrintMapping::unmapped_id;
        if (!json_to_int(*ams_it, ams_id)) {
            error = context + ".ams_id is not an integer";
            return false;
        }
        if (!json_to_int(*slot_it, slot_id)) {
            error = context + ".slot_id is not an integer";
            return false;
        }
        if (ams_id < 0 || ams_id > PrintMapping::virtual_main_ams_id) {
            error = context + ".ams_id is outside the supported range";
            return false;
        }
        if (slot_id < 0 || slot_id > PrintMapping::virtual_main_ams_id) {
            error = context + ".slot_id is outside the supported range";
            return false;
        }

        PrintMapping::Destination destination;
        if (ams_id == PrintMapping::virtual_main_ams_id) {
            if (slot_id != PrintMapping::virtual_main_ams_id) {
                error = context + " pairs the external/unmapped ams_id with a physical slot";
                return false;
            }
            // 255/255 is ambiguous; metadata resolves it to external_spool when present.
            destination.kind = PrintMapping::DestinationKind::unmapped;
        } else if (ams_id == PrintMapping::virtual_deputy_ams_id) {
            destination.kind   = PrintMapping::DestinationKind::external_spool;
            destination.ams_id = PrintMapping::virtual_deputy_ams_id;
        } else {
            if (slot_id == PrintMapping::virtual_main_ams_id) {
                error = context + " pairs a physical ams_id with the slot sentinel";
                return false;
            }
            destination.kind    = PrintMapping::DestinationKind::ams_slot;
            destination.ams_id  = ams_id;
            destination.slot_id = slot_id;
        }

        PrintMapping::Filament filament;
        filament.filament_index = i;
        filament.destination    = destination;
        out.push_back(std::move(filament));
    }
    return true;
}

inline bool parse_v0(const nlohmann::json& array, std::vector<PrintMapping::Filament>& out, std::string& error)
{
    if (!array.is_array()) {
        error = "ams_mapping is not an array";
        return false;
    }

    out.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i) {
        const std::string context = "ams_mapping[" + std::to_string(i) + "]";
        int tray_id = PrintMapping::unmapped_id;
        if (!json_to_int(array[i], tray_id)) {
            error = context + " is not an integer";
            return false;
        }
        if (tray_id > PrintMapping::virtual_main_ams_id) {
            error = context + " is outside the supported range";
            return false;
        }

        PrintMapping::Destination destination;
        if (tray_id < 0) {
            destination.kind = PrintMapping::DestinationKind::unmapped;
        } else if (tray_id == PrintMapping::virtual_main_ams_id || tray_id == PrintMapping::virtual_deputy_ams_id) {
            destination.kind   = PrintMapping::DestinationKind::external_spool;
            destination.ams_id = tray_id;
        } else {
            // v0 carries only a flat tray index, not an AMS unit/slot pair.
            destination.kind    = PrintMapping::DestinationKind::ams_slot;
            destination.slot_id = tray_id;
        }

        PrintMapping::Filament filament;
        filament.filament_index = i;
        filament.destination    = destination;
        out.push_back(std::move(filament));
    }
    return true;
}

inline bool is_virtual_ams_id(int ams_id) noexcept
{
    return ams_id == PrintMapping::virtual_main_ams_id || ams_id == PrintMapping::virtual_deputy_ams_id;
}

// ams_mapping_info is the only place where v0/v1 sentinel values can be told apart from real
// mappings, so resolve those cases after the authoritative array has been parsed.
inline bool apply_metadata(PrintMapping::Filament& filament, const PrintMapping::FilamentMetadata& metadata, std::string& error)
{
    filament.metadata = metadata;

    if (is_virtual_ams_id(metadata.ams)) {
        if (filament.destination.kind == PrintMapping::DestinationKind::ams_slot) {
            error = "ams_mapping_info[" + std::to_string(filament.filament_index) +
                    "] names a virtual tray but the mapping names an AMS slot";
            return false;
        }
        filament.destination.kind    = PrintMapping::DestinationKind::external_spool;
        filament.destination.ams_id  = metadata.ams;
        filament.destination.slot_id = PrintMapping::unmapped_id;
        return true;
    }

    if (metadata.ams == PrintMapping::unmapped_id) {
        // 255/255 is the shared external/unmapped sentinel in v1.
        if (filament.destination.kind == PrintMapping::DestinationKind::external_spool &&
            filament.destination.ams_id == PrintMapping::virtual_main_ams_id)
            filament.destination = PrintMapping::Destination{};
        return true;
    }

    if (filament.destination.kind == PrintMapping::DestinationKind::external_spool) {
        error = "ams_mapping_info[" + std::to_string(filament.filament_index) +
                "] names an AMS tray but the mapping names a virtual tray";
        return false;
    }
    return true;
}

inline bool parse_nozzle_mapping(const nlohmann::json& array, std::vector<PrintMapping::NozzleMapping>& out, std::string& error)
{
    if (!array.is_array()) {
        error = "nozzle_mapping is not an array";
        return false;
    }

    out.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i) {
        int nozzle_id = PrintMapping::unmapped_id;
        if (!json_to_int(array[i], nozzle_id)) {
            error = "nozzle_mapping[" + std::to_string(i) + "] is not an integer";
            return false;
        }
        out.push_back(PrintMapping::NozzleMapping{i, nozzle_id});
    }
    return true;
}

inline bool parse_nozzles(const nlohmann::json& array, std::vector<PrintMapping::NozzleInfo>& out, std::string& error)
{
    if (!array.is_array()) {
        error = "nozzles_info is not an array";
        return false;
    }

    out.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i) {
        const nlohmann::json& item = array[i];
        const std::string context   = "nozzles_info[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            error = context + " is not an object";
            return false;
        }

        PrintMapping::NozzleInfo nozzle;

        const auto id_it = item.find("id");
        if (id_it == item.end() || !json_to_int(*id_it, nozzle.id)) {
            error = context + ".id is missing or not an integer";
            return false;
        }

        if (const auto it = item.find("type"); it != item.end() && !it->is_null()) {
            if (!it->is_string()) {
                error = context + ".type is not a string";
                return false;
            }
            nozzle.type = it->get<std::string>();
        }
        if (const auto it = item.find("flowSize"); it != item.end() && !it->is_null()) {
            if (!it->is_string()) {
                error = context + ".flowSize is not a string";
                return false;
            }
            nozzle.flow_size = it->get<std::string>();
        }
        if (const auto it = item.find("diameter"); it != item.end() && !it->is_null()) {
            if (!it->is_number()) {
                error = context + ".diameter is not a number";
                return false;
            }
            nozzle.diameter = it->get<double>();
        }

        out.push_back(std::move(nozzle));
    }
    return true;
}

} // namespace PrintMappingDetail

inline bool PrintMapping::from_legacy(const PrintParams& params, PrintMapping& out, std::string& error)
{
    // On every failure path `out` is cleared so callers never observe a partial mapping.
    auto fail = [&out]() {
        out = PrintMapping{};
        return false;
    };

    try {
        out = PrintMapping{};
        error.clear();
        out.use_ams = params.task_use_ams;

        nlohmann::json ams_mapping_v0;
        nlohmann::json ams_mapping_v1;
        nlohmann::json ams_mapping_info;
        nlohmann::json nozzle_mapping;
        nlohmann::json nozzles_info;

        bool has_v0 = false;
        bool has_v1 = false;
        bool has_info = false;
        bool has_nozzle_mapping = false;
        bool has_nozzles_info = false;

        if (!PrintMappingDetail::read_json(params.ams_mapping2, "ams_mapping2", ams_mapping_v1, has_v1, error))
            return fail();
        // v0 is legacy fallback only; malformed stale v0 must not override authoritative v1.
        if (!has_v1 && !PrintMappingDetail::read_json(params.ams_mapping, "ams_mapping", ams_mapping_v0, has_v0, error))
            return fail();
        if (!PrintMappingDetail::read_json(params.ams_mapping_info, "ams_mapping_info", ams_mapping_info, has_info, error))
            return fail();
        if (!PrintMappingDetail::read_json(params.nozzle_mapping, "nozzle_mapping", nozzle_mapping, has_nozzle_mapping, error))
            return fail();
        if (!PrintMappingDetail::read_json(params.nozzles_info, "nozzles_info", nozzles_info, has_nozzles_info, error))
            return fail();

        std::vector<FilamentMetadata> metadata;
        if (has_info && !PrintMappingDetail::parse_metadata(ams_mapping_info, metadata, error))
            return fail();

        if (has_v1) {
            out.ams_mapping_version = MappingVersion::v1;
            if (!PrintMappingDetail::parse_v1(ams_mapping_v1, out.filaments, error))
                return fail();
        } else if (has_v0) {
            out.ams_mapping_version = MappingVersion::v0;
            if (!PrintMappingDetail::parse_v0(ams_mapping_v0, out.filaments, error))
                return fail();
        } else if (!metadata.empty()) {
            error = "ams_mapping_info is present without ams_mapping or ams_mapping2";
            return fail();
        }

        if (metadata.size() > out.filaments.size()) {
            error = "ams_mapping_info has more entries than the AMS mapping";
            return fail();
        }

        const std::size_t enriched = std::min(metadata.size(), out.filaments.size());
        for (std::size_t i = 0; i < enriched; ++i) {
            if (!PrintMappingDetail::apply_metadata(out.filaments[i], metadata[i], error))
                return fail();
        }

        if (has_nozzle_mapping && !PrintMappingDetail::parse_nozzle_mapping(nozzle_mapping, out.nozzle_mapping, error))
            return fail();
        if (has_nozzles_info && !PrintMappingDetail::parse_nozzles(nozzles_info, out.nozzles, error))
            return fail();

        return true;
    } catch (const std::exception& exception) {
        error = std::string("failed to parse print mapping: ") + exception.what();
        return fail();
    } catch (...) {
        error = "failed to parse print mapping: unknown error";
        return fail();
    }
}

} // namespace Slic3r
