#pragma once
#include <wx/string.h>
#include "slic3r/Utils/json_diff.hpp"

namespace Slic3r
{
class DevFilaBlacklist
{
public:
    static bool load_filaments_blacklist_config();
    static void check_filaments_in_blacklist(std::string model_id, std::string tag_vendor, std::string tag_type, const std::string& filament_id, int ams_id, int slot_id, std::string tag_name, bool& in_blacklist, std::string& ac, wxString& info);
    static void check_filaments_in_blacklist_url(std::string model_id, std::string tag_vendor, std::string tag_type, const std::string& filament_id, int ams_id, int slot_id, std::string tag_name, bool& in_blacklist, std::string& ac, wxString& info, wxString& wiki_url);

public:
    static json filaments_blacklist;
};// class DevFilaBlacklist

}// namespace Slic3r