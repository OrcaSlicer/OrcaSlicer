#ifndef slic3r_ThumbnailData_hpp_
#define slic3r_ThumbnailData_hpp_

#include <vector>
#include "libslic3r/Point.hpp"
#include "nlohmann/json.hpp"

namespace Slic3r {

//BBS: thumbnail_size in gcode file
static std::vector<Vec2d> THUMBNAIL_SIZE = { Vec2d(50, 50) };

struct ThumbnailData
{
    unsigned int width;
    unsigned int height;
    std::vector<unsigned char> pixels;

    ThumbnailData() { reset(); }
    void set(unsigned int w, unsigned int h);
    void reset();

    bool is_valid() const;
    void load_from(ThumbnailData &data) {
        this->set(data.width, data.height);
        pixels = data.pixels;
    }
};

//BBS: add plate id into thumbnail render logic
using ThumbnailsList = std::vector<ThumbnailData>;

struct ThumbnailsParams
{
	const Vec2ds 	sizes;
	bool 			printable_only;
	bool 			parts_only;
	bool 			show_bed;
	bool 			transparent_background;
    int             plate_id;
    bool            use_plate_box{true};
};

typedef std::function<ThumbnailsList(const ThumbnailsParams&)> ThumbnailsGeneratorCallback;

struct BBoxData
{
    int id;  // object id
    std::vector<coordf_t> bbox; // first layer bounding box: min.{x,y}, max.{x,y}
    float area;  // first layer area
    float layer_height;
    std::string name;
    void to_json(nlohmann::json& j) const{
        j = nlohmann::json{
            {"id",id},
            {"bbox",bbox},
            {"area",area},
            {"layer_height",layer_height},
            {"name",name}
        };
    }
    void from_json(const nlohmann::json& j) {
        j.at("id").get_to(id);
        j.at("bbox").get_to(bbox);
        j.at("area").get_to(area);
        j.at("layer_height").get_to(layer_height);
        j.at("name").get_to(name);
    }
};

struct PlateBBoxData
{
    std::vector<coordf_t> bbox_all;  // total bounding box of all objects including brim
    std::vector<BBoxData> bbox_objs; // BBoxData of seperate object
    std::vector<int>      filament_ids; // filament id used in curr plate
    std::vector<std::string> filament_colors;
    bool is_seq_print = false;
    int first_extruder = 0;
    float nozzle_diameter = 0.4;
    std::string bed_type;
    float first_layer_time;
    // version 1: use view type ColorPrint (filament color)
    // version 2: use view type FilamentId (filament id)
    int version = 2;

    void to_json(nlohmann::json& j) const{
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
    void from_json(const nlohmann::json& j) {
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
    bool is_valid() const {
        return !bbox_objs.empty();
    }
};

} // namespace Slic3r

#endif // slic3r_ThumbnailData_hpp_
