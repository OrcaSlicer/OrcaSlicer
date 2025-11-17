#pragma once
#include "libslic3r/CommonDefs.hpp"
#include "libslic3r/ProjectTask.hpp"

#include "slic3r/Utils/json_diff.hpp"
#include <wx/string.h>

namespace Slic3r
{
//Previous definitions
class MachineObject;

enum MappingOption
{
    USE_LEFT_AMS = 0,
    USE_RIGHT_AMS,
    USE_LEFT_EXT,
    USE_RIGHT_EXT
};

class DevMappingUtil
{
public:
    DevMappingUtil() = delete;
    ~DevMappingUtil() = delete;

public:
    static bool is_valid_mapping_result(const MachineObject* obj, std::vector<FilamentInfo>& result, bool check_empty_slot = false);

    static int ams_filament_mapping(const MachineObject* obj, const std::vector<FilamentInfo>& filaments, std::vector<FilamentInfo>& result, std::vector<bool> map_opt, std::vector<int> exclude_id = std::vector<int>(), bool nozzle_has_ams_then_ignore_ext = false);
};

};