#ifndef slic3r_Format_AssembleList_hpp_
#define slic3r_Format_AssembleList_hpp_

#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class ModelObject;

typedef struct _height_range_info {
    float         min_z;
    float         max_z;

    std::map<std::string, std::string> range_params;
}height_range_info_t;

typedef struct _assembled_param_info {
    std::map<std::string, std::string> print_params;
    std::vector<height_range_info_t> height_ranges;
}assembled_param_info_t;

typedef struct _assemble_object_info {
    std::string         path;
    int                 count;

    std::vector<int>    filaments;
    std::vector<int>    assemble_index;
    std::vector<float>  pos_x;
    std::vector<float>  pos_y;
    std::vector<float>  pos_z;
    std::map<std::string, std::string> print_params;
    std::vector<height_range_info_t> height_ranges;
}assemble_object_info_t;

typedef struct _assemble_plate_info {
    std::string         plate_name;
    bool                need_arrange {false};
    int                 filaments_count {0};

    std::map<std::string, std::string> plate_params;
    std::vector<assemble_object_info_t> assemble_obj_list;
    std::vector<ModelObject *> loaded_obj_list;
    std::map<int, assembled_param_info_t> assembled_param_list;
}assemble_plate_info_t;

enum class AssembleListResult {
    Success,
    FileNotFound,
    // Malformed JSON, a missing required field, or a value that fails validation.
    ConfigError
};

// Read the JSON assemble list used by the CLI's --load-assemble-list into one entry per plate.
AssembleListResult load_assemble_plate_list(const std::string &config_file, std::vector<assemble_plate_info_t> &assemble_plate_info_list, int max_plate_count);

} // namespace Slic3r

#endif /* slic3r_Format_AssembleList_hpp_ */
