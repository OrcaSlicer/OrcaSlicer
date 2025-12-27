#ifndef BBS_3MF_hpp_
#define BBS_3MF_hpp_

#include "../GCode/ThumbnailData.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <functional>

namespace Slic3r {
class Model;
class ModelObject;
struct ConfigSubstitutionContext;
class DynamicPrintConfig;
class Preset;
struct FilamentInfo;
struct ThumbnailData;


#define PLATE_THUMBNAIL_SMALL_WIDTH     128
#define PLATE_THUMBNAIL_SMALL_HEIGHT    128

#define GCODE_FILE_FORMAT               "Metadata/plate_%1%.gcode"
#define THUMBNAIL_FILE_FORMAT           "Metadata/plate_%1%.png"
#define NO_LIGHT_THUMBNAIL_FILE_FORMAT  "Metadata/plate_no_light_%1%.png"
#define TOP_FILE_FORMAT                 "Metadata/top_%1%.png"
#define PICK_FILE_FORMAT                "Metadata/pick_%1%.png"
//#define PATTERN_FILE_FORMAT             "Metadata/plate_%1%_pattern_layer_0.png"
#define PATTERN_CONFIG_FILE_FORMAT      "Metadata/plate_%1%.json"
#define EMBEDDED_PRINT_FILE_FORMAT      "Metadata/process_settings_%1%.config"
#define EMBEDDED_FILAMENT_FILE_FORMAT      "Metadata/filament_settings_%1%.config"
#define EMBEDDED_PRINTER_FILE_FORMAT      "Metadata/machine_settings_%1%.config"

#define BBL_DESIGNER_MODEL_TITLE_TAG     "Title"
#define BBL_DESIGNER_PROFILE_ID_TAG      "DesignProfileId"
#define BBL_DESIGNER_PROFILE_TITLE_TAG   "ProfileTitle"
#define BBL_DESIGNER_MODEL_ID_TAG        "DesignModelId"


//BBS: define assistant struct to store temporary variable during exporting 3mf
class PackingTemporaryData
{
public:
    std::string _3mf_thumbnail;
    std::string _3mf_printer_thumbnail_middle;
    std::string _3mf_printer_thumbnail_small;

    PackingTemporaryData() {}
};


//BBS: define plate data list related structures
struct PlateData
{
    PlateData(int plate_id, std::set<std::pair<int, int>> &obj_to_inst_list, bool lock_state) : plate_index(plate_id), locked(lock_state)
    {
        objects_and_instances.clear();
        for (std::set<std::pair<int, int>>::iterator it = obj_to_inst_list.begin(); it != obj_to_inst_list.end(); ++it)
            objects_and_instances.emplace_back(it->first, it->second);
    }
    PlateData() : plate_index(-1), locked(false)
    {
        objects_and_instances.clear();
    }
    ~PlateData()
    {
        objects_and_instances.clear();
    }

    void parse_filament_info(GCodeProcessorResult *result);

    int plate_index;
    std::vector<std::pair<int, int>> objects_and_instances;
    std::map<int, std::pair<int, int>> obj_inst_map;
    std::string     printer_model_id;
    std::string     nozzle_diameters;
    std::string     gcode_file;
    std::string     gcode_file_md5;
    std::string     thumbnail_file;
    std::string     no_light_thumbnail_file;
    ThumbnailData   plate_thumbnail;
    std::string     top_file;
    std::string     pick_file;
    //ThumbnailData   pattern_thumbnail;
    //std::string     pattern_file;
    std::string     pattern_bbox_file;
    std::string     gcode_prediction;
    std::string     gcode_weight;
    std::string     first_layer_time;
    std::string     plate_name;
    std::vector<FilamentInfo> slice_filaments_info;
    std::vector<size_t> skipped_objects;
    DynamicPrintConfig config;
    bool            is_support_used {false};
    bool            is_sliced_valid = false;
    bool            toolpath_outside {false};
    bool            is_label_object_enabled {false};
    int             timelapse_warning_code = 0; // 1<<0 sprial vase, 1<<1 by object
    std::vector<int>          filament_maps;   // 1 base
    using LayerFilaments = std::unordered_map<std::vector<unsigned int>, std::vector<std::pair<int, int>>, GCodeProcessorResult::FilamentSequenceHash>;
    LayerFilaments layer_filaments;

    // Hexadecimal number,
    // the 0th digit corresponds to extruder 1
    // the 1th digit corresponds to extruder 2
    // ...  and so on.
    // 0 means can be print on this extruder, 1 means cannot
    std::vector<int>          limit_filament_maps;

    std::vector<GCodeProcessorResult::SliceWarning> warnings;

    std::string get_gcode_prediction_str() {
        return gcode_prediction;
    }

    std::string get_gcode_weight_str() {
        return gcode_weight;
    }
    bool locked;
};

// BBS: encrypt
enum class SaveStrategy
{
    Default = 0,
    FullPathSources     = 1,
    Zip64               = 1 << 1,
    ProductionExt       = 1 << 2,
    SecureContentExt    = 1 << 3,
    WithGcode           = 1 << 4,
    Silence             = 1 << 5,
    SkipStatic          = 1 << 6,
    SkipModel           = 1 << 7,
    WithSliceInfo       = 1 << 8,
    SkipAuxiliary       = 1 << 9,
    UseLoadedId         = 1 << 10,
    ShareMesh           = 1 << 11,

    SplitModel = 0x1000 | ProductionExt,
    Encrypted  = SecureContentExt | SplitModel,
    Backup = 0x10000 | WithGcode | Silence | SkipStatic | SplitModel,
};

inline SaveStrategy operator | (SaveStrategy lhs, SaveStrategy rhs)
{
    using T = std::underlying_type_t <SaveStrategy>;
    return static_cast<SaveStrategy>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

inline bool operator & (SaveStrategy & lhs, SaveStrategy rhs)
{
    using T = std::underlying_type_t <SaveStrategy>;
    return ((static_cast<T>(lhs) & static_cast<T>(rhs))) == static_cast<T>(rhs);
}

enum {
    brim_points_format_version = 0
};

enum class LoadStrategy
{
    Default = 0,
    AddDefaultInstances = 1,
    CheckVersion = 2,
    LoadModel = 4,
    LoadConfig = 8,
    LoadAuxiliary = 16,
    Silence = 32,
    ImperialUnits = 64,

    Restore = 0x10000 | LoadModel | LoadConfig | LoadAuxiliary | Silence,
};

inline LoadStrategy operator | (LoadStrategy lhs, LoadStrategy rhs)
{
    using T = std::underlying_type_t <LoadStrategy>;
    return static_cast<LoadStrategy>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

inline bool operator & (LoadStrategy & lhs, LoadStrategy rhs)
{
    using T = std::underlying_type_t <LoadStrategy>;
    return (static_cast<T>(lhs) & static_cast<T>(rhs)) == static_cast<T>(rhs);
}

const int EXPORT_STAGE_OPEN_3MF         = 0;
const int EXPORT_STAGE_CONTENT_TYPES    = 1;
const int EXPORT_STAGE_ADD_THUMBNAILS   = 2;
const int EXPORT_STAGE_ADD_RELATIONS    = 3;
const int EXPORT_STAGE_ADD_MODELS       = 4;
const int EXPORT_STAGE_ADD_LAYER_RANGE  = 5;
const int EXPORT_STAGE_ADD_SUPPORT      = 6;
const int EXPORT_STAGE_ADD_CUSTOM_GCODE = 7;
const int EXPORT_STAGE_ADD_PRINT_CONFIG = 8;
const int EXPORT_STAGE_ADD_PROJECT_CONFIG = 9;
const int EXPORT_STAGE_ADD_CONFIG_FILE  = 10;
const int EXPORT_STAGE_ADD_SLICE_INFO   = 11;
const int EXPORT_STAGE_ADD_GCODE        = 12;
const int EXPORT_STAGE_ADD_AUXILIARIES  = 13;
const int EXPORT_STAGE_FINISH           = 14;

const int IMPORT_STAGE_RESTORE          = 0;
const int IMPORT_STAGE_OPEN             = 1;
const int IMPORT_STAGE_READ_FILES       = 2;
const int IMPORT_STAGE_EXTRACT          = 3;
const int IMPORT_STAGE_LOADING_OBJECTS  = 4;
const int IMPORT_STAGE_LOADING_PLATES   = 5;
const int IMPORT_STAGE_FINISH           = 6;
const int IMPORT_STAGE_ADD_INSTANCE     = 7;
const int IMPORT_STAGE_UPDATE_GCODE     = 8;
const int IMPORT_STAGE_CHECK_MODE_GCODE = 9;
const int UPDATE_GCODE_RESULT           = 10;
const int IMPORT_LOAD_CONFIG            = 11;
const int IMPORT_LOAD_MODEL_OBJECTS     = 12;
const int IMPORT_STAGE_MAX              = 13;

//BBS export 3mf progress
typedef std::function<void(int export_stage, int current, int total, bool& cancel)> Export3mfProgressFn;
typedef std::function<void(int import_stage, int current, int total, bool& cancel)> Import3mfProgressFn;

typedef std::vector<PlateData*> PlateDataPtrs;

typedef std::map<int, PlateData*> PlateDataMaps;

struct StoreParams
{
    const char* path;
    Model* model = nullptr;
    PlateDataPtrs plate_data_list;
    int export_plate_idx = -1;
    std::vector<Preset*> project_presets;
    DynamicPrintConfig* config;
    std::vector<ThumbnailData*> thumbnail_data;
    std::vector<ThumbnailData*> no_light_thumbnail_data;
    std::vector<ThumbnailData*> top_thumbnail_data;
    std::vector<ThumbnailData*> pick_thumbnail_data;
    std::vector<ThumbnailData*> calibration_thumbnail_data;
    SaveStrategy strategy = SaveStrategy::Zip64;
    Export3mfProgressFn proFn = nullptr;
    std::vector<PlateBBoxData*> id_bboxes;
    BBLProject* project = nullptr;
    BBLProfile* profile = nullptr;

    StoreParams() {}
};


//BBS: add plate data list related logic
// add restore logic
// Load the content of a 3mf file into the given model and preset bundle.
extern bool load_bbs_3mf(const char* path, DynamicPrintConfig* config, ConfigSubstitutionContext* config_substitutions, Model* model, PlateDataPtrs* plate_data_list, std::vector<Preset*>* project_presets,
        bool* is_bbl_3mf, Semver* file_version, Import3mfProgressFn proFn = nullptr, LoadStrategy strategy = LoadStrategy::Default, BBLProject *project = nullptr, int plate_id = 0);

extern std::string bbs_3mf_get_thumbnail(const char * path);

extern bool load_gcode_3mf_from_stream(std::istream & data, DynamicPrintConfig* config, Model* model, PlateDataPtrs* plate_data_list,
       Semver* file_version);


//BBS: add plate data list related logic
// add backup logic
// Save the given model and the config data contained in the given Print into a 3mf file.
// The model could be modified during the export process if meshes are not repaired or have no shared vertices
/*
extern bool store_bbs_3mf(const char* path,
                          Model* model,
                          PlateDataPtrs& plate_data_list,
                          std::vector<Preset*>& project_presets,
                          const DynamicPrintConfig* config,
                          bool fullpath_sources,
                          const std::vector<ThumbnailData*>& thumbnail_data,
                          bool zip64 = true,
                          bool skip_static = false,
                          Export3mfProgressFn proFn = nullptr,
                          bool silence = true);
*/

extern bool store_bbs_3mf(StoreParams& store_params);

extern void release_PlateData_list(PlateDataPtrs& plate_data_list);

// backup & restore project

extern void save_object_mesh(ModelObject& object);

extern void delete_object_mesh(ModelObject& object);

extern void backup_soon();

extern void remove_backup(Model& model, bool removeAll);

extern void set_backup_interval(long interval);

extern void set_backup_callback(std::function<void(int)> callback);

extern void run_backup_ui_tasks();

extern bool has_restore_data(std::string & path, std::string & origin);

extern void put_other_changes();

extern void clear_other_changes(bool backup);

extern bool has_other_changes(bool backup);

class SaveObjectGaurd {
public:
    SaveObjectGaurd(ModelObject& object);
    ~SaveObjectGaurd();
};

} // namespace Slic3r

#endif /* BBS_3MF_hpp_ */
