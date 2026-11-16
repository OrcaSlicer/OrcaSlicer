#ifndef slic3r_ThumbnailData_hpp_
#define slic3r_ThumbnailData_hpp_

#include <functional>
#include <string>
#include <vector>
#include "libslic3r/libslic3r.h"
#include "libslic3r/Point.hpp"
#include <nlohmann/json_fwd.hpp>

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
	const Vec2ds 	sizes{};
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
    void to_json(nlohmann::json& j) const;
    void from_json(const nlohmann::json& j);
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
    float first_layer_time = 0.0f;
    // version 1: use view type ColorPrint (filament color)
    // version 2: use view type FilamentId (filament id)
    int version = 2;

    void to_json(nlohmann::json& j) const;
    void from_json(const nlohmann::json& j);
    bool is_valid() const {
        return !bbox_objs.empty();
    }
};

} // namespace Slic3r

#endif // slic3r_ThumbnailData_hpp_
