#ifndef SLIC3R_HPP
#define SLIC3R_HPP

#include <map>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/AssembleList.hpp"

namespace Slic3r { class Preset; }
namespace Slic3r { struct PlateBBoxData; }
namespace Slic3r { struct ThumbnailData; }

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
    // Options the user typed; setup() fills the CLI's own options with defaults afterwards.
    std::set<std::string>       m_given_option_keys;
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
