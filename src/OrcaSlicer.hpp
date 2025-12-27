#ifndef SLIC3R_HPP
#define SLIC3R_HPP

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {

namespace IO {
	enum ExportFormat : int {
        AMF,
        OBJ,
        STL,
        // SVG,
        TMF,
        Gcode
    };
}

#define JSON_ASSEMPLE_PLATES                   "plates"
#define JSON_ASSEMPLE_PLATE_PARAMS             "plate_params"
#define JSON_ASSEMPLE_PLATE_NAME               "plate_name"
#define JSON_ASSEMPLE_PLATE_NEED_ARRANGE       "need_arrange"
#define JSON_ASSEMPLE_OBJECTS                  "objects"
#define JSON_ASSEMPLE_OBJECT_PATH              "path"
#define JSON_ASSEMPLE_OBJECT_COUNT             "count"
#define JSON_ASSEMPLE_OBJECT_FILAMENTS         "filaments"
#define JSON_ASSEMPLE_OBJECT_POS_X             "pos_x"
#define JSON_ASSEMPLE_OBJECT_POS_Y             "pos_y"
#define JSON_ASSEMPLE_OBJECT_POS_Z             "pos_z"
#define JSON_ASSEMPLE_OBJECT_ASSEMBLE_INDEX    "assemble_index"
#define JSON_ASSEMPLE_OBJECT_PRINT_PARAMS      "print_params"
#define JSON_ASSEMPLE_ASSEMBLE_PARAMS         "assembled_params"


#define JSON_ASSEMPLE_OBJECT_MIN_Z              "min_z"
#define JSON_ASSEMPLE_OBJECT_MAX_Z              "max_z"
#define JSON_ASSEMPLE_OBJECT_HEIGHT_RANGES      "height_ranges"
#define JSON_ASSEMPLE_OBJECT_RANGE_PARAMS       "range_params"

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


typedef struct _printer_plate_info {
    std::string         printer_name;
    int                 printable_width{0};
    int                 printable_depth{0};
    int                 printable_height{0};

    int                 exclude_width{0};
    int                 exclude_depth{0};
    int                 exclude_x{0};
    int                 exclude_y{0};

    int                 wrapping_width{0};
    int                 wrapping_depth{0};
    int                 wrapping_x{0};
    int                 wrapping_y{0};
}printer_plate_info_t;

typedef struct _plate_obj_size_info {
    bool         has_wipe_tower{false};
    float        wipe_x{0.f};
    float        wipe_y{0.f};
    float        wipe_width{0.f};
    float        wipe_depth{0.f};
    BoundingBoxf3 obj_bbox;
}plate_obj_size_info_t;


class CLI {
public:
    int run(int argc, char **argv);

private:
    DynamicPrintAndCLIConfig    m_config;
    DynamicPrintConfig			m_print_config;
    DynamicPrintConfig          m_extra_config;
    std::vector<std::string>    m_input_files;
    std::vector<std::string>    m_actions;
    std::vector<std::string>    m_transforms;
    std::vector<Model>          m_models;

    bool setup(int argc, char **argv);

    /// Prints usage of the CLI.
    void print_help(bool include_print_options = false, PrinterTechnology printer_technology = ptAny) const;

    /// Exports loaded models to a file of the specified format, according to the options affecting output filename.
    bool export_models(IO::ExportFormat format, std::string path = std::string());
    //BBS: add export_project function
    bool export_project(Model *model, std::string& path, PlateDataPtrs &partplate_data, std::vector<Preset*>& project_presets,
                        std::vector<ThumbnailData *> &thumbnails,
                        std::vector<ThumbnailData *> &no_light_thumbnails,
                        std::vector<ThumbnailData *> &top_thumbnails,
                        std::vector<ThumbnailData *> &pick_thumbnails,
        std::vector<ThumbnailData*>& calibration_thumbnails,
        std::vector<PlateBBoxData*>& plate_bboxes, const DynamicPrintConfig* config, bool minimum_save, int plate_to_export = -1);

    bool has_print_action() const { return m_config.opt_bool("export_gcode") || m_config.opt_bool("export_sla"); }

    std::string output_filepath(const Model &model, IO::ExportFormat format) const;
    std::string output_filepath(const ModelObject &object, unsigned int index, IO::ExportFormat format, std::string path_dir) const;
};

}

#endif
