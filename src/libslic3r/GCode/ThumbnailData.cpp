#include "ThumbnailData.hpp"
#include <vector>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r {

void BBoxData::to_json(nlohmann::json& j) const
{
    j = nlohmann::json{
        {"id",id},
        {"bbox",bbox},
        {"area",area},
        {"layer_height",layer_height},
        {"name",name}
    };
}

void BBoxData::from_json(const nlohmann::json& j)
{
    j.at("id").get_to(id);
    j.at("bbox").get_to(bbox);
    j.at("area").get_to(area);
    j.at("layer_height").get_to(layer_height);
    j.at("name").get_to(name);
}

void PlateBBoxData::to_json(nlohmann::json& j) const
{
    j = nlohmann::json{ {"bbox_all",bbox_all} };
    j["filament_ids"] = filament_ids;
    j["filament_colors"] = filament_colors;
    j["is_seq_print"]    = is_seq_print;
    j["first_extruder"] = first_extruder;
    j["nozzle_diameter"] = nozzle_diameter;
    j["version"]         = version;
    j["bed_type"]        = bed_type;
    j["first_layer_time"] = first_layer_time;
    for (const auto& bbox : bbox_objs) {
        nlohmann::json j_bbox;
        bbox.to_json(j_bbox);
        j["bbox_objects"].push_back(j_bbox);
    }
}

void PlateBBoxData::from_json(const nlohmann::json& j)
{
    j.at("bbox_all").get_to(bbox_all);
    j.at("filament_ids").get_to(filament_ids);
    j.at("filament_colors").get_to(filament_colors);
    j.at("is_seq_print").get_to(is_seq_print);
    j.at("first_extruder").get_to(first_extruder);
    j.at("nozzle_diameter").get_to(nozzle_diameter);
    j.at("version").get_to(version);
    j.at("bed_type").get_to(bed_type);
    for (auto& bbox_j : j.at("bbox_objects")) {
        BBoxData bbox_data;
        bbox_data.from_json(bbox_j);
        bbox_objs.push_back(bbox_data);
    }
}

void ThumbnailData::set(unsigned int w, unsigned int h)
{
    if ((w == 0) || (h == 0))
        return;

    if ((width != w) || (height != h))
    {
        width = w;
        height = h;
        // defaults to white texture
        pixels.clear();
        pixels = std::vector<unsigned char>(width * height * 4, 255);
    }
}

void ThumbnailData::reset()
{
    width = 0;
    height = 0;
    pixels.clear();
}

bool ThumbnailData::is_valid() const
{
    return (width != 0) && (height != 0) && ((unsigned int)pixels.size() == 4 * width * height);
}

} // namespace Slic3r
