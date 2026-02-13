#include "../libslic3r.h"
#include "../Exception.hpp"
#include "../Model.hpp"
#include "../Preset.hpp"
#include "../Utils.hpp"
#include "../LocalesUtils.hpp"
#include "../GCode.hpp"
#include "../Geometry.hpp"
#include "../GCode/ThumbnailData.hpp"
#include "../Semver.hpp"
#include "../Time.hpp"

#include "../I18N.hpp"

#include "bbs_3mf.hpp"

#include <limits>
#include <stdexcept>
#include <iomanip>

#include <boost/assign.hpp>
#include <boost/bimap.hpp>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/spirit/include/karma.hpp>
#include <boost/spirit/include/qi_int.hpp>
#include <boost/log/trivial.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/foreach.hpp>
#include <openssl/md5.h>

namespace pt = boost::property_tree;

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <expat.h>
#include <Eigen/Dense>
#include "miniz_extension.hpp"
#include "nlohmann/json.hpp"

#include "TextConfiguration.hpp"
#include "EmbossShape.hpp"
#include "ExPolygonSerialize.hpp" 

#include "NSVGUtils.hpp"

#include <fast_float/fast_float.h>

// Slightly faster than sprintf("%.9g"), but there is an issue with the karma floating point formatter,
// https://github.com/boostorg/spirit/pull/586
// where the exported string is one digit shorter than it should be to guarantee lossless round trip.
// The code is left here for the ocasion boost guys improve.
#define EXPORT_3MF_USE_SPIRIT_KARMA_FP 0

#define WRITE_ZIP_LANGUAGE_ENCODING 1

// @see https://commons.apache.org/proper/commons-compress/apidocs/src-html/org/apache/commons/compress/archivers/zip/AbstractUnicodeExtraField.html
struct ZipUnicodePathExtraField
{
    static std::string encode(std::string const& u8path, std::string const& path) {
        std::string extra;
        if (u8path != path) {
            // 0x7075 - for Unicode filenames
            extra.push_back('\x75');
            extra.push_back('\x70');
            boost::uint16_t len = 5 + u8path.length();
            extra.push_back((char)(len & 0xff));
            extra.push_back((char)(len >> 8));
            auto crc = mz_crc32(0, (unsigned char *) path.c_str(), path.length());
            extra.push_back('\x01'); // version 1
            extra.append((char *)&crc, (char *)&crc + 4); // Little Endian
            extra.append(u8path);
        }
        return extra;
    }
    static std::string decode(std::string const& extra, std::string const& path = {}) {
        char const * p = extra.data();
        char const * e = p + extra.length();
        while (p + 4 < e) {
            boost::uint16_t len = ((boost::uint16_t)p[2]) | ((boost::uint16_t)p[3] << 8);
            if (p[0] == '\x75' && p[1] == '\x70' && len >= 5 && p + 4 + len < e && p[4] == '\x01') {
                return std::string(p + 9, p + 4 + len);
            }
            else {
                p += 4 + len;
            }
        }
        return Slic3r::decode_path(path.c_str());
    }
};

// VERSION NUMBERS
// 0 : .3mf, files saved by older slic3r or other applications. No version definition in them.
// 1 : Introduction of 3mf versioning. No other change in data saved into 3mf files.
// 2 : Volumes' matrices and source data added to Metadata/Slic3r_PE_model.config file, meshes transformed back to their coordinate system on loading.
// WARNING !! -> the version number has been rolled back to 1
//               the next change should use 3
const unsigned int VERSION_BBS_3MF = 1;
// Allow loading version 2 file as well.
const unsigned int VERSION_BBS_3MF_COMPATIBLE = 2;
const char* BBS_3MF_VERSION1 = "bamboo_slicer:Version3mf"; // definition of the metadata name saved into .model file
const char* BBS_3MF_VERSION = "BambuStudio:3mfVersion"; //compatible with prusa currently
// Painting gizmos data version numbers
// 0 : initial version of fdm, seam, mm
const unsigned int FDM_SUPPORTS_PAINTING_VERSION = 0;
const unsigned int SEAM_PAINTING_VERSION         = 0;
const unsigned int MM_PAINTING_VERSION           = 0;

const std::string BBS_FDM_SUPPORTS_PAINTING_VERSION = "BambuStudio:FdmSupportsPaintingVersion";
const std::string BBS_SEAM_PAINTING_VERSION         = "BambuStudio:SeamPaintingVersion";
const std::string BBS_MM_PAINTING_VERSION           = "BambuStudio:MmPaintingVersion";
const std::string BBL_MODEL_ID_TAG                  = "model_id";
const std::string BBL_MODEL_NAME_TAG                = "Title";
const std::string BBL_ORIGIN_TAG                    = "Origin";
const std::string BBL_DESIGNER_TAG                  = "Designer";
const std::string BBL_DESIGNER_USER_ID_TAG          = "DesignerUserId";
const std::string BBL_DESIGNER_COVER_FILE_TAG       = "DesignerCover";
const std::string BBL_DESCRIPTION_TAG               = "Description";
const std::string BBL_COPYRIGHT_TAG                 = "CopyRight";
const std::string BBL_COPYRIGHT_NORMATIVE_TAG       = "Copyright";
const std::string BBL_LICENSE_TAG                   = "License";
const std::string BBL_REGION_TAG                    = "Region";
const std::string BBL_MODIFICATION_TAG              = "ModificationDate";
const std::string BBL_CREATION_DATE_TAG             = "CreationDate";
const std::string BBL_APPLICATION_TAG               = "Application";
const std::string BBL_MAKERLAB_TAG                  = "MakerLab";
const std::string BBL_MAKERLAB_VERSION_TAG          = "MakerLabVersion";


const std::string BBL_PROFILE_TITLE_TAG             = "ProfileTitle";
const std::string BBL_PROFILE_COVER_TAG             = "ProfileCover";
const std::string BBL_PROFILE_DESCRIPTION_TAG       = "ProfileDescription";
const std::string BBL_PROFILE_USER_ID_TAG           = "ProfileUserId";
const std::string BBL_PROFILE_USER_NAME_TAG         = "ProfileUserName";

const std::string MODEL_FOLDER = "3D/";
const std::string MODEL_EXTENSION = ".model";
const std::string MODEL_FILE = "3D/3dmodel.model"; // << this is the only format of the string which works with CURA
const std::string MODEL_RELS_FILE = "3D/_rels/3dmodel.model.rels";
//BBS: add metadata_folder
const std::string METADATA_DIR = "Metadata/";
const std::string ACCESOR_DIR = "accesories/";
const std::string GCODE_EXTENSION = ".gcode";
const std::string THUMBNAIL_EXTENSION = ".png";
const std::string CALIBRATION_INFO_EXTENSION = ".json";
const std::string CONTENT_TYPES_FILE = "[Content_Types].xml";
const std::string RELATIONSHIPS_FILE = "_rels/.rels";
const std::string THUMBNAIL_FILE = "Metadata/plate_1.png";
const std::string THUMBNAIL_FOR_PRINTER_FILE = "Metadata/bbl_thumbnail.png";
const std::string PRINTER_THUMBNAIL_SMALL_FILE = "/Auxiliaries/.thumbnails/thumbnail_small.png";
const std::string PRINTER_THUMBNAIL_MIDDLE_FILE = "/Auxiliaries/.thumbnails/thumbnail_middle.png";
const std::string _3MF_COVER_FILE = "/Auxiliaries/.thumbnails/thumbnail_3mf.png";
//const std::string PRINT_CONFIG_FILE = "Metadata/Slic3r_PE.config";
//const std::string MODEL_CONFIG_FILE = "Metadata/Slic3r_PE_model.config";
const std::string BBS_PRINT_CONFIG_FILE = "Metadata/print_profile.config";
const std::string BBS_PROJECT_CONFIG_FILE = "Metadata/project_settings.config";
const std::string BBS_MODEL_CONFIG_FILE = "Metadata/model_settings.config";
const std::string BBS_MODEL_CONFIG_RELS_FILE = "Metadata/_rels/model_settings.config.rels";
const std::string SLICE_INFO_CONFIG_FILE = "Metadata/slice_info.config";
const std::string BBS_LAYER_HEIGHTS_PROFILE_FILE = "Metadata/layer_heights_profile.txt";
const std::string LAYER_CONFIG_RANGES_FILE = "Metadata/layer_config_ranges.xml";
const std::string BRIM_EAR_POINTS_FILE = "Metadata/brim_ear_points.txt";
/*const std::string SLA_SUPPORT_POINTS_FILE = "Metadata/Slic3r_PE_sla_support_points.txt";
const std::string SLA_DRAIN_HOLES_FILE = "Metadata/Slic3r_PE_sla_drain_holes.txt";*/
const std::string CUSTOM_GCODE_PER_PRINT_Z_FILE = "Metadata/custom_gcode_per_layer.xml";
const std::string AUXILIARY_DIR = "Auxiliaries/";
const std::string PROJECT_EMBEDDED_PRINT_PRESETS_FILE = "Metadata/print_setting_";
const std::string PROJECT_EMBEDDED_SLICE_PRESETS_FILE = "Metadata/process_settings_";
const std::string PROJECT_EMBEDDED_FILAMENT_PRESETS_FILE = "Metadata/filament_settings_";
const std::string PROJECT_EMBEDDED_PRINTER_PRESETS_FILE = "Metadata/machine_settings_";
const std::string CUT_INFORMATION_FILE = "Metadata/cut_information.xml";

const unsigned int AUXILIARY_STR_LEN = 12;
const unsigned int METADATA_STR_LEN = 9;


static constexpr const char* MODEL_TAG = "model";
static constexpr const char* RESOURCES_TAG = "resources";
static constexpr const char* COLOR_GROUP_TAG = "m:colorgroup";
static constexpr const char* COLOR_TAG = "m:color";
static constexpr const char* OBJECT_TAG = "object";
static constexpr const char* MESH_TAG = "mesh";
static constexpr const char* MESH_STAT_TAG = "mesh_stat";
static constexpr const char* VERTICES_TAG = "vertices";
static constexpr const char* VERTEX_TAG = "vertex";
static constexpr const char* TRIANGLES_TAG = "triangles";
static constexpr const char* TRIANGLE_TAG = "triangle";
static constexpr const char* COMPONENTS_TAG = "components";
static constexpr const char* COMPONENT_TAG = "component";
static constexpr const char* BUILD_TAG = "build";
static constexpr const char* ITEM_TAG = "item";
static constexpr const char* METADATA_TAG = "metadata";
static constexpr const char* FILAMENT_TAG = "filament";
static constexpr const char* SLICE_WARNING_TAG = "warning";
static constexpr const char* WARNING_MSG_TAG = "msg";
static constexpr const char *FILAMENT_ID_TAG   = "id";
static constexpr const char* FILAMENT_TYPE_TAG = "type";
static constexpr const char *FILAMENT_COLOR_TAG = "color";
static constexpr const char *FILAMENT_USED_M_TAG = "used_m";
static constexpr const char *FILAMENT_USED_G_TAG = "used_g";
static constexpr const char *FILAMENT_TRAY_INFO_ID_TAG     = "tray_info_idx";
static constexpr const char *LAYER_FILAMENT_LISTS_TAG      = "layer_filament_lists";
static constexpr const char *LAYER_FILAMENT_LIST_TAG       = "layer_filament_list";


static constexpr const char* CONFIG_TAG = "config";
static constexpr const char* VOLUME_TAG = "volume";
static constexpr const char* PART_TAG = "part";
static constexpr const char* PLATE_TAG = "plate";
static constexpr const char* INSTANCE_TAG = "model_instance";
//BBS
static constexpr const char* ASSEMBLE_TAG = "assemble";
static constexpr const char* ASSEMBLE_ITEM_TAG = "assemble_item";
static constexpr const char* SLICE_HEADER_TAG = "header";
static constexpr const char* SLICE_HEADER_ITEM_TAG = "header_item";

// Deprecated: text_info
static constexpr const char* TEXT_INFO_TAG        = "text_info";
static constexpr const char* TEXT_ATTR            = "text";
static constexpr const char* FONT_NAME_ATTR       = "font_name";
static constexpr const char* FONT_INDEX_ATTR      = "font_index";
static constexpr const char* FONT_SIZE_ATTR       = "font_size";
static constexpr const char* THICKNESS_ATTR       = "thickness";
static constexpr const char* EMBEDED_DEPTH_ATTR   = "embeded_depth";
static constexpr const char* ROTATE_ANGLE_ATTR    = "rotate_angle";
static constexpr const char* TEXT_GAP_ATTR        = "text_gap";
static constexpr const char* BOLD_ATTR            = "bold";
static constexpr const char* ITALIC_ATTR          = "italic";
static constexpr const char* SURFACE_TEXT_ATTR    = "surface_text";
static constexpr const char* KEEP_HORIZONTAL_ATTR = "keep_horizontal";
static constexpr const char* HIT_MESH_ATTR        = "hit_mesh";
static constexpr const char* HIT_POSITION_ATTR    = "hit_position";
static constexpr const char* HIT_NORMAL_ATTR      = "hit_normal";

// BBS: encrypt
static constexpr const char* RELATIONSHIP_TAG = "Relationship";
static constexpr const char* PID_ATTR = "pid";
static constexpr const char* PUUID_ATTR = "p:UUID";
static constexpr const char* PUUID_LOWER_ATTR = "p:uuid";
static constexpr const char* PPATH_ATTR = "p:path";
static constexpr const char *OBJECT_UUID_SUFFIX = "-61cb-4c03-9d28-80fed5dfa1dc";
static constexpr const char *OBJECT_UUID_SUFFIX2 = "-71cb-4c03-9d28-80fed5dfa1dc";
static constexpr const char *SUB_OBJECT_UUID_SUFFIX = "-81cb-4c03-9d28-80fed5dfa1dc";
static constexpr const char *COMPONENT_UUID_SUFFIX = "-b206-40ff-9872-83e8017abed1";
static constexpr const char* BUILD_UUID = "2c7c17d8-22b5-4d84-8835-1976022ea369";
static constexpr const char* BUILD_UUID_SUFFIX = "-b1ec-4553-aec9-835e5b724bb4";
static constexpr const char* TARGET_ATTR = "Target";
static constexpr const char* RELS_TYPE_ATTR = "Type";

static constexpr const char* UNIT_ATTR = "unit";
static constexpr const char* NAME_ATTR = "name";
static constexpr const char* COLOR_ATTR = "color";
static constexpr const char* TYPE_ATTR = "type";
static constexpr const char* ID_ATTR = "id";
static constexpr const char* X_ATTR = "x";
static constexpr const char* Y_ATTR = "y";
static constexpr const char* Z_ATTR = "z";
static constexpr const char* V1_ATTR = "v1";
static constexpr const char* V2_ATTR = "v2";
static constexpr const char* V3_ATTR = "v3";
static constexpr const char* OBJECTID_ATTR = "objectid";
static constexpr const char* TRANSFORM_ATTR = "transform";
// BBS
static constexpr const char* OFFSET_ATTR = "offset";
static constexpr const char* PRINTABLE_ATTR = "printable";
static constexpr const char* INSTANCESCOUNT_ATTR = "instances_count";
static constexpr const char* CUSTOM_SUPPORTS_ATTR = "paint_supports";
static constexpr const char* CUSTOM_FUZZY_SKIN_ATTR  = "paint_fuzzy_skin";
static constexpr const char* CUSTOM_SEAM_ATTR = "paint_seam";
static constexpr const char* MMU_SEGMENTATION_ATTR = "paint_color";
// BBS
static constexpr const char* FACE_PROPERTY_ATTR = "face_property";

static constexpr const char* KEY_ATTR = "key";
static constexpr const char* VALUE_ATTR = "value";
static constexpr const char* FIRST_TRIANGLE_ID_ATTR = "firstid";
static constexpr const char* LAST_TRIANGLE_ID_ATTR = "lastid";
static constexpr const char* SUBTYPE_ATTR = "subtype";
static constexpr const char* LOCK_ATTR = "locked";
static constexpr const char* BED_TYPE_ATTR = "bed_type";
static constexpr const char* PRINT_SEQUENCE_ATTR = "print_sequence";
static constexpr const char* FIRST_LAYER_PRINT_SEQUENCE_ATTR = "first_layer_print_sequence";
static constexpr const char* OTHER_LAYERS_PRINT_SEQUENCE_ATTR = "other_layers_print_sequence";
static constexpr const char* OTHER_LAYERS_PRINT_SEQUENCE_NUMS_ATTR = "other_layers_print_sequence_nums";
static constexpr const char* SPIRAL_VASE_MODE = "spiral_mode";
static constexpr const char* FILAMENT_MAP_MODE_ATTR = "filament_map_mode";
static constexpr const char* FILAMENT_MAP_ATTR = "filament_maps";
static constexpr const char* LIMIT_FILAMENT_MAP_ATTR = "limit_filament_maps";
static constexpr const char* GCODE_FILE_ATTR = "gcode_file";
static constexpr const char* THUMBNAIL_FILE_ATTR = "thumbnail_file";
static constexpr const char* NO_LIGHT_THUMBNAIL_FILE_ATTR = "thumbnail_no_light_file";
static constexpr const char* TOP_FILE_ATTR = "top_file";
static constexpr const char* PICK_FILE_ATTR = "pick_file";
static constexpr const char* PATTERN_FILE_ATTR = "pattern_file";
static constexpr const char* PATTERN_BBOX_FILE_ATTR = "pattern_bbox_file";
static constexpr const char* OBJECT_ID_ATTR = "object_id";
static constexpr const char* INSTANCEID_ATTR = "instance_id";
static constexpr const char* IDENTIFYID_ATTR = "identify_id";
static constexpr const char* PLATERID_ATTR = "plater_id";
static constexpr const char* PLATER_NAME_ATTR = "plater_name";
static constexpr const char* PLATE_IDX_ATTR = "index";
static constexpr const char* PRINTER_MODEL_ID_ATTR = "printer_model_id";
static constexpr const char* EXTRUDER_TYPE_ATTR = "extruder_type";
static constexpr const char* NOZZLE_VOLUME_TYPE_ATTR = "nozzle_volume_type";
static constexpr const char* NOZZLE_TYPE_ATTR          = "nozzle_types";
static constexpr const char* NOZZLE_DIAMETERS_ATTR = "nozzle_diameters";
static constexpr const char* SLICE_PREDICTION_ATTR = "prediction";
static constexpr const char* SLICE_WEIGHT_ATTR = "weight";
static constexpr const char* FIRST_LAYER_TIME_ATTR = "first_layer_time";
static constexpr const char* TIMELAPSE_TYPE_ATTR = "timelapse_type";
static constexpr const char* OUTSIDE_ATTR = "outside";
static constexpr const char* SUPPORT_USED_ATTR = "support_used";
static constexpr const char* LABEL_OBJECT_ENABLED_ATTR = "label_object_enabled";
static constexpr const char* SKIPPED_ATTR = "skipped";

static constexpr const char* OBJECT_TYPE = "object";
static constexpr const char* VOLUME_TYPE = "volume";
static constexpr const char* PART_TYPE = "part";

static constexpr const char* NAME_KEY = "name";
static constexpr const char* VOLUME_TYPE_KEY = "volume_type";
static constexpr const char* PART_TYPE_KEY = "part_type";
static constexpr const char* MATRIX_KEY = "matrix";
static constexpr const char* SOURCE_FILE_KEY = "source_file";
static constexpr const char* SOURCE_OBJECT_ID_KEY = "source_object_id";
static constexpr const char* SOURCE_VOLUME_ID_KEY = "source_volume_id";
static constexpr const char* SOURCE_OFFSET_X_KEY = "source_offset_x";
static constexpr const char* SOURCE_OFFSET_Y_KEY = "source_offset_y";
static constexpr const char* SOURCE_OFFSET_Z_KEY = "source_offset_z";
static constexpr const char* SOURCE_IN_INCHES    = "source_in_inches";
static constexpr const char* SOURCE_IN_METERS    = "source_in_meters";

static constexpr const char* MESH_SHARED_KEY = "mesh_shared";

static constexpr const char* MESH_STAT_EDGES_FIXED          = "edges_fixed";
static constexpr const char* MESH_STAT_DEGENERATED_FACETS   = "degenerate_facets";
static constexpr const char* MESH_STAT_FACETS_REMOVED       = "facets_removed";
static constexpr const char* MESH_STAT_FACETS_RESERVED      = "facets_reversed";
static constexpr const char* MESH_STAT_BACKWARDS_EDGES      = "backwards_edges";

// Store / load of TextConfiguration
static constexpr const char *TEXT_TAG = "slic3rpe:text";
static constexpr const char *TEXT_DATA_ATTR = "text";
// TextConfiguration::EmbossStyle
static constexpr const char *STYLE_NAME_ATTR      = "style_name";
static constexpr const char *FONT_DESCRIPTOR_ATTR = "font_descriptor";
static constexpr const char *FONT_DESCRIPTOR_TYPE_ATTR = "font_descriptor_type";

// TextConfiguration::FontProperty
static constexpr const char *CHAR_GAP_ATTR    = "char_gap";
static constexpr const char *LINE_GAP_ATTR    = "line_gap";
static constexpr const char *LINE_HEIGHT_ATTR = "line_height";
static constexpr const char *BOLDNESS_ATTR    = "boldness";
static constexpr const char *SKEW_ATTR        = "skew";
static constexpr const char *PER_GLYPH_ATTR   = "per_glyph";
static constexpr const char *HORIZONTAL_ALIGN_ATTR  = "horizontal";
static constexpr const char *VERTICAL_ALIGN_ATTR    = "vertical";
static constexpr const char *COLLECTION_NUMBER_ATTR = "collection";

static constexpr const char *FONT_FAMILY_ATTR    = "family";
static constexpr const char *FONT_FACE_NAME_ATTR = "face_name";
static constexpr const char *FONT_STYLE_ATTR     = "style";
static constexpr const char *FONT_WEIGHT_ATTR    = "weight";

// Store / load of EmbossShape
static constexpr const char *SHAPE_TAG = "slic3rpe:shape";
static constexpr const char *SHAPE_SCALE_ATTR   = "scale";
static constexpr const char *UNHEALED_ATTR = "unhealed";
static constexpr const char *SVG_FILE_PATH_ATTR = "filepath";
static constexpr const char *SVG_FILE_PATH_IN_3MF_ATTR = "filepath3mf";

// EmbossProjection
static constexpr const char *DEPTH_ATTR       = "depth";
static constexpr const char *USE_SURFACE_ATTR = "use_surface";
// static constexpr const char *FIX_TRANSFORMATION_ATTR = "transform";


const unsigned int BBS_VALID_OBJECT_TYPES_COUNT = 2;
const char* BBS_VALID_OBJECT_TYPES[] =
{
    "model",
    "other"
};

const char* BBS_INVALID_OBJECT_TYPES[] =
{
    "solidsupport",
    "support",
    "surface"
};

template <typename T>
struct hex_wrap
{
    T t;
};

namespace std {
    template <class _Elem, class _Traits, class _Arg>
    basic_ostream<_Elem, _Traits>& operator<<(basic_ostream<_Elem, _Traits>& ostr,
        const hex_wrap<_Arg>& wrap) { // insert by calling function with output stream and argument
        auto of = ostr.fill('0');
        ostr << setw(sizeof(_Arg) * 2) << std::hex << wrap.t;
        ostr << std::dec << setw(0);
        ostr.fill(of);
        return ostr;
    }
}

class version_error : public Slic3r::FileIOError
{
public:
    version_error(const std::string& what_arg) : Slic3r::FileIOError(what_arg) {}
    version_error(const char* what_arg) : Slic3r::FileIOError(what_arg) {}
};

const char* bbs_get_attribute_value_charptr(const char** attributes, unsigned int attributes_size, const char* attribute_key)
{
    if ((attributes == nullptr) || (attributes_size == 0) || (attributes_size % 2 != 0) || (attribute_key == nullptr))
        return nullptr;

    for (unsigned int a = 0; a < attributes_size; a += 2) {
        if (::strcmp(attributes[a], attribute_key) == 0)
            return attributes[a + 1];
    }

    return nullptr;
}

std::string bbs_get_attribute_value_string(const char** attributes, unsigned int attributes_size, const char* attribute_key)
{
    const char* text = bbs_get_attribute_value_charptr(attributes, attributes_size, attribute_key);
    return (text != nullptr) ? text : "";
}

float bbs_get_attribute_value_float(const char** attributes, unsigned int attributes_size, const char* attribute_key)
{
    float value = 0.0f;
    if (const char *text = bbs_get_attribute_value_charptr(attributes, attributes_size, attribute_key); text != nullptr)
        fast_float::from_chars(text, text + strlen(text), value);
    return value;
}

int bbs_get_attribute_value_int(const char** attributes, unsigned int attributes_size, const char* attribute_key)
{
    int value = 0;
    if (const char *text = bbs_get_attribute_value_charptr(attributes, attributes_size, attribute_key); text != nullptr)
        boost::spirit::qi::parse(text, text + strlen(text), boost::spirit::qi::int_, value);
    return value;
}

bool bbs_get_attribute_value_bool(const char** attributes, unsigned int attributes_size, const char* attribute_key)
{
    const char* text = bbs_get_attribute_value_charptr(attributes, attributes_size, attribute_key);
    return (text != nullptr) ? (bool)::atoi(text) : true;
}

void add_vec3(std::stringstream &stream, const Slic3r::Vec3f &tr)
{
    for (unsigned r = 0; r < 3; ++r) {
        stream << tr(r);
        if (r != 2)
            stream << " ";
    }
}

template<typename T>
void add_vector(std::stringstream &stream, const std::vector<T> &values)
{
    for (size_t i = 0; i < values.size(); ++i) {
        stream << values[i];
        if (i != (values.size() - 1))
            stream << " ";
    }
}

Slic3r::Vec3f get_vec3_from_string(const std::string &pos_str)
{
    Slic3r::Vec3f pos(0, 0, 0);
    if (pos_str.empty())
        return pos;

    std::vector<std::string> values;
    boost::split(values, pos_str, boost::is_any_of(" "), boost::token_compress_on);

    if (values.size() != 3)
        return pos;

    for (int i = 0; i < 3; ++i)
        pos(i) = ::atof(values[i].c_str());

    return pos;
}

Slic3r::Transform3d bbs_get_transform_from_3mf_specs_string(const std::string& mat_str)
{
    // check: https://3mf.io/3d-manufacturing-format/ or https://github.com/3MFConsortium/spec_core/blob/master/3MF%20Core%20Specification.md
    // to see how matrices are stored inside 3mf according to specifications
    Slic3r::Transform3d ret = Slic3r::Transform3d::Identity();

    if (mat_str.empty())
        // empty string means default identity matrix
        return ret;

    std::vector<std::string> mat_elements_str;
    boost::split(mat_elements_str, mat_str, boost::is_any_of(" "), boost::token_compress_on);

    unsigned int size = (unsigned int)mat_elements_str.size();
    if (size != 12)
        // invalid data, return identity matrix
        return ret;

    unsigned int i = 0;
    // matrices are stored into 3mf files as 4x3
    // we need to transpose them
    for (unsigned int c = 0; c < 4; ++c) {
        for (unsigned int r = 0; r < 3; ++r) {
            ret(r, c) = ::atof(mat_elements_str[i++].c_str());
        }
    }
    return ret;
}

Slic3r::Vec3d bbs_get_offset_from_3mf_specs_string(const std::string& vec_str)
{
    Slic3r::Vec3d ofs2ass(0, 0, 0);

    if (vec_str.empty())
        // empty string means default zero offset
        return ofs2ass;

    std::vector<std::string> vec_elements_str;
    boost::split(vec_elements_str, vec_str, boost::is_any_of(" "), boost::token_compress_on);

    unsigned int size = (unsigned int)vec_elements_str.size();
    if (size != 3)
        // invalid data, return zero offset
        return ofs2ass;

    for (unsigned int i = 0; i < 3; i++) {
        ofs2ass(i) = ::atof(vec_elements_str[i].c_str());
    }

    return ofs2ass;
}

float bbs_get_unit_factor(const std::string& unit)
{
    const char* text = unit.c_str();

    if (::strcmp(text, "micron") == 0)
        return 0.001f;
    else if (::strcmp(text, "centimeter") == 0)
        return 10.0f;
    else if (::strcmp(text, "inch") == 0)
        return 25.4f;
    else if (::strcmp(text, "foot") == 0)
        return 304.8f;
    else if (::strcmp(text, "meter") == 0)
        return 1000.0f;
    else
        // default "millimeters" (see specification)
        return 1.0f;
}

bool bbs_is_valid_object_type(const std::string& type)
{
    // if the type is empty defaults to "model" (see specification)
    if (type.empty())
        return true;

    for (unsigned int i = 0; i < BBS_VALID_OBJECT_TYPES_COUNT; ++i) {
        if (::strcmp(type.c_str(), BBS_VALID_OBJECT_TYPES[i]) == 0)
            return true;
    }

    return false;
}

namespace Slic3r {

void PlateData::parse_filament_info(GCodeProcessorResult *result)
{
    if (!result) return;

    PrintEstimatedStatistics &ps                            = result->print_statistics;
    std::vector<float>        m_filament_diameters          = result->filament_diameters;
    std::vector<float>        m_filament_densities          = result->filament_densities;
    auto get_used_filament_from_volume = [m_filament_diameters, m_filament_densities](double volume, int extruder_id) {
        double                    koef = 0.001;
        double                    section_area = PI * sqr(0.5 * m_filament_diameters[extruder_id]);
        std::pair<double, double> ret = {section_area < EPSILON ? 0 : (koef * volume / section_area), volume * m_filament_densities[extruder_id] * 0.001};
        return ret;
    };

    for (auto it = ps.total_volumes_per_extruder.begin(); it != ps.total_volumes_per_extruder.end(); it++) {
        double volume                           = it->second;
        auto [used_filament_m, used_filament_g] = get_used_filament_from_volume(volume, it->first);

        FilamentInfo info;
        info.id = it->first;
        info.used_g = used_filament_g;
        info.used_m = used_filament_m;
        slice_filaments_info.push_back(info);
    }

    /* only for test
    GCodeProcessorResult::SliceWarning sw;
    sw.msg = BED_TEMP_TOO_HIGH_THAN_FILAMENT;
    sw.level = 1;
    result->warnings.push_back(sw);
    */
    warnings = result->warnings;
}


//! macro used to mark string used at localization,
//! return same string
#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

    // Base class with error messages management
    class _BBS_3MF_Base
    {
        mutable boost::mutex mutex;
        mutable std::vector<std::string> m_errors;

    protected:
        void add_error(const std::string& error) const { boost::unique_lock l(mutex); m_errors.push_back(error); }
        void clear_errors() { m_errors.clear(); }

    public:
        void log_errors()
        {
            for (const std::string& error : m_errors)
                BOOST_LOG_TRIVIAL(error) << error;
        }
    };

    class _BBS_3MF_Importer : public _BBS_3MF_Base
    {
        typedef std::pair<std::string, int> Id; // BBS: encrypt

        struct Component
        {
            Id object_id;
            Transform3d transform;

            explicit Component(Id object_id)
                : object_id(object_id)
                , transform(Transform3d::Identity())
            {
            }

            Component(Id object_id, const Transform3d& transform)
                : object_id(object_id)
                , transform(transform)
            {
            }
        };

        typedef std::vector<Component> ComponentsList;

        struct Geometry
        {
            std::vector<Vec3f> vertices;
            std::vector<Vec3i32> triangles;
            std::vector<std::string> custom_supports;
            std::vector<std::string> custom_seam;
            std::vector<std::string> mmu_segmentation;
            std::vector<std::string> fuzzy_skin;
            // BBS
            std::vector<std::string> face_properties;

            bool empty() { return vertices.empty() || triangles.empty(); }

            // backup & restore
            void swap(Geometry& o) {
                std::swap(vertices, o.vertices);
                std::swap(triangles, o.triangles);
                std::swap(custom_supports, o.custom_supports);
                std::swap(custom_seam, o.custom_seam);
            }

            void reset() {
                vertices.clear();
                triangles.clear();
                custom_supports.clear();
                custom_seam.clear();
                mmu_segmentation.clear();
                fuzzy_skin.clear();
            }
        };

        struct CurrentObject
        {
            // ID of the object inside the 3MF file, 1 based.
            int id;
            // Index of the ModelObject in its respective Model, zero based.
            int model_object_idx;
            Geometry geometry;
            ModelObject* object;
            ComponentsList components;

            //BBS: sub object id
            //int subobject_id;
            std::string name;
            std::string uuid;
            int         pid{-1};
            //bool is_model_object;

            CurrentObject() { reset(); }

            void reset() {
                id = -1;
                model_object_idx = -1;
                geometry.reset();
                object = nullptr;
                components.clear();
                //BBS: sub object id
                uuid.clear();
                name.clear();
            }
        };

        struct CurrentConfig
        {
            int object_id {-1};
            int volume_id {-1};
        };

        struct CurrentInstance
        {
            int object_id;
            int instance_id;
            int identify_id;
        };

        struct Instance
        {
            ModelInstance* instance;
            Transform3d transform;

            Instance(ModelInstance* instance, const Transform3d& transform)
                : instance(instance)
                , transform(transform)
            {
            }
        };

        struct Metadata
        {
            std::string key;
            std::string value;

            Metadata(const std::string& key, const std::string& value)
                : key(key)
                , value(value)
            {
            }
        };

        typedef std::vector<Metadata> MetadataList;

        struct ObjectMetadata
        {
            struct VolumeMetadata
            {
                //BBS: refine the part logic
                unsigned int first_triangle_id;
                unsigned int last_triangle_id;
                int subobject_id;
                MetadataList metadata;
                RepairedMeshErrors mesh_stats;
                ModelVolumeType part_type;
                std::optional<TextConfiguration> text_configuration;
                std::optional<EmbossShape> shape_configuration;
                VolumeMetadata(unsigned int first_triangle_id, unsigned int last_triangle_id, ModelVolumeType type = ModelVolumeType::MODEL_PART)
                    : first_triangle_id(first_triangle_id)
                    , last_triangle_id(last_triangle_id)
                    , part_type(type)
                    , subobject_id(-1)
                {
                }

                VolumeMetadata(int sub_id, ModelVolumeType type = ModelVolumeType::MODEL_PART)
                    : subobject_id(sub_id)
                    , part_type(type)
                    , first_triangle_id(0)
                    , last_triangle_id(0)
                {
                }
            };

            typedef std::vector<VolumeMetadata> VolumeMetadataList;

            MetadataList metadata;
            VolumeMetadataList volumes;
        };

        struct CutObjectInfo
        {
            struct Connector
            {
                int   volume_id;
                int   type;
                float r_tolerance;
                float h_tolerance;
            };
            CutObjectBase          id;
            std::vector<Connector> connectors;
        };

        // Map from a 1 based 3MF object ID to a 0 based ModelObject index inside m_model->objects.
        //typedef std::pair<std::string, int> Id; // BBS: encrypt
        typedef std::map<Id, CurrentObject> IdToCurrentObjectMap;
        typedef std::map<int, std::string> IndexToPathMap;
        typedef std::map<Id, int> IdToModelObjectMap;
        //typedef std::map<Id, ComponentsList> IdToAliasesMap;
        typedef std::vector<Instance> InstancesList;
        typedef std::map<int, ObjectMetadata> IdToMetadataMap;
        typedef std::map<int, CutObjectInfo>  IdToCutObjectInfoMap;
        //typedef std::map<Id, Geometry> IdToGeometryMap;
        typedef std::map<int, std::vector<coordf_t>> IdToLayerHeightsProfileMap;
        typedef std::map<int, t_layer_config_ranges> IdToLayerConfigRangesMap;
        typedef std::map<int, BrimPoints>             IdToBrimPointsMap;
        /*typedef std::map<int, std::vector<sla::SupportPoint>> IdToSlaSupportPointsMap;
        typedef std::map<int, std::vector<sla::DrainHole>> IdToSlaDrainHolesMap;*/
        using PathToEmbossShapeFileMap = std::map<std::string, std::shared_ptr<std::string>>;

        struct ObjectImporter
        {
            IdToCurrentObjectMap object_list;
            CurrentObject *current_object{nullptr};
            std::string object_path;
            std::string zip_path;
            _BBS_3MF_Importer *top_importer{nullptr};
            XML_Parser object_xml_parser;
            bool obj_parse_error { false };
            std::string obj_parse_error_message;

            //local parsed datas
            std::string obj_curr_metadata_name;
            std::string obj_curr_characters;
            float object_unit_factor;
            int object_current_color_group{-1};
            std::map<int, std::string> object_group_id_to_color;
            bool is_bbl_3mf { false };

            ObjectImporter(_BBS_3MF_Importer *importer, std::string file_path, std::string obj_path)
            {
                top_importer = importer;
                object_path = obj_path;
                zip_path = file_path;
            }

            ~ObjectImporter()
            {
                _destroy_object_xml_parser();
            }

            void _destroy_object_xml_parser()
            {
                if (object_xml_parser != nullptr) {
                    XML_ParserFree(object_xml_parser);
                    object_xml_parser = nullptr;
                }
            }

            void _stop_object_xml_parser(const std::string& msg = std::string())
            {
                assert(! obj_parse_error);
                assert(obj_parse_error_message.empty());
                assert(object_xml_parser != nullptr);
                obj_parse_error = true;
                obj_parse_error_message = msg;
                XML_StopParser(object_xml_parser, false);
            }

            bool        object_parse_error()         const { return obj_parse_error; }
            const char* object_parse_error_message() const {
                return obj_parse_error ?
                    // The error was signalled by the user code, not the expat parser.
                    (obj_parse_error_message.empty() ? "Invalid 3MF format" : obj_parse_error_message.c_str()) :
                    // The error was signalled by the expat parser.
                    XML_ErrorString(XML_GetErrorCode(object_xml_parser));
            }

            bool _extract_object_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);

            bool extract_object_model()
            {
                mz_zip_archive archive;
                mz_zip_zero_struct(&archive);

                if (!open_zip_reader(&archive, zip_path)) {
                    top_importer->add_error("Unable to open the zipfile "+ zip_path);
                    return false;
                }

                if (!top_importer->_extract_from_archive(archive, object_path, [this] (mz_zip_archive& archive, const mz_zip_archive_file_stat& stat) {
                    return _extract_object_from_archive(archive, stat);
                }, top_importer->m_load_restore)) {
                    std::string error_msg = std::string("Archive does not contain a valid model for ") + object_path;
                    top_importer->add_error(error_msg);

                    close_zip_reader(&archive);
                    return false;
                }

                close_zip_reader(&archive);

                if (obj_parse_error) {
                    //already add_error inside
                    //top_importer->add_error(object_parse_error_message());
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Found error while extrace object %1%\n")%object_path;
                    return false;
                }
                return true;
            }

            bool _handle_object_start_model(const char** attributes, unsigned int num_attributes);
            bool _handle_object_end_model();

            bool _handle_object_start_resources(const char** attributes, unsigned int num_attributes);
            bool _handle_object_end_resources();

            bool _handle_object_start_object(const char** attributes, unsigned int num_attributes);
            bool _handle_object_end_object();

            bool _handle_object_start_color_group(const char **attributes, unsigned int num_attributes);
            bool _handle_object_end_color_group();

            bool _handle_object_start_color(const char **attributes, unsigned int num_attributes);
            bool _handle_object_end_color();

            bool _handle_object_start_mesh(const char** attributes, unsigned int num_attributes);
            bool _handle_object_end_mesh();

            bool _handle_object_start_vertices(const char** attributes, unsigned int num_attributes);
            bool _handle_object_end_vertices();

            bool _handle_object_start_vertex(const char** attributes, unsigned int num_attributes);
            bool _handle_object_end_vertex();

            bool _handle_object_start_triangles(const char** attributes, unsigned int num_attributes);
            bool _handle_object_end_triangles();

            bool _handle_object_start_triangle(const char** attributes, unsigned int num_attributes);
            bool _handle_object_end_triangle();

            bool _handle_object_start_components(const char** attributes, unsigned int num_attributes);
            bool _handle_object_end_components();

            bool _handle_object_start_component(const char** attributes, unsigned int num_attributes);
            bool _handle_object_end_component();

            bool _handle_object_start_metadata(const char** attributes, unsigned int num_attributes);
            bool _handle_object_end_metadata();

            void _handle_object_start_model_xml_element(const char* name, const char** attributes);
            void _handle_object_end_model_xml_element(const char* name);
            void _handle_object_xml_characters(const XML_Char* s, int len);

            // callbacks to parse the .model file of an object
            static void XMLCALL _handle_object_start_model_xml_element(void* userData, const char* name, const char** attributes);
            static void XMLCALL _handle_object_end_model_xml_element(void* userData, const char* name);
            static void XMLCALL _handle_object_xml_characters(void* userData, const XML_Char* s, int len);
        };

        // Version of the 3mf file
        unsigned int m_version;
        bool m_check_version = false;
        bool m_load_model = false;
        bool m_load_aux = false;
        bool m_load_config = false;
        // backup & restore
        bool m_load_restore = false;
        std::string m_backup_path;
        std::string m_origin_file;
        // Semantic version of Orca Slicer, that generated this 3MF.
        boost::optional<Semver> m_bambuslicer_generator_version;
        unsigned int m_fdm_supports_painting_version = 0;
        unsigned int m_seam_painting_version         = 0;
        unsigned int m_mm_painting_version           = 0;
        std::string  m_model_id;
        std::string  m_contry_code;
        std::string  m_designer;
        std::string  m_designer_user_id;
        std::string  m_designer_cover;
        ModelInfo    model_info;
        BBLProject   project_info;
        std::string  m_profile_title;
        std::string  m_profile_cover;
        std::string  m_Profile_description;
        std::string  m_profile_user_id;
        std::string  m_profile_user_name;

        XML_Parser m_xml_parser;
        // Error code returned by the application side of the parser. In that case the expat may not reliably deliver the error state
        // after returning from XML_Parse() function, thus we keep the error state here.
        bool m_parse_error { false };
        std::string m_parse_error_message;
        Model* m_model;
        float m_unit_factor;
        CurrentObject* m_curr_object{nullptr};
        IdToCurrentObjectMap m_current_objects;
        IndexToPathMap       m_index_paths;
        IdToModelObjectMap m_objects;
        //IdToAliasesMap m_objects_aliases;
        InstancesList m_instances;
        //IdToGeometryMap m_geometries;
        //IdToGeometryMap m_orig_geometries; // backup & restore
        CurrentConfig m_curr_config;
        IdToMetadataMap m_objects_metadata;
        IdToCutObjectInfoMap       m_cut_object_infos;
        IdToLayerHeightsProfileMap m_layer_heights_profiles;
        IdToLayerConfigRangesMap m_layer_config_ranges;
        IdToBrimPointsMap m_brim_ear_points;
        /*IdToSlaSupportPointsMap m_sla_support_points;
        IdToSlaDrainHolesMap    m_sla_drain_holes;*/
        PathToEmbossShapeFileMap m_path_to_emboss_shape_files;
        std::string m_curr_metadata_name;
        std::string m_curr_characters;
        std::string m_name;
        std::string m_sub_model_path;

        std::string m_start_part_path;
        std::string m_thumbnail_path;
        std::string m_thumbnail_middle;
        std::string m_thumbnail_small;
        std::vector<std::string> m_sub_model_paths;
        std::vector<ObjectImporter*> m_object_importers;

        std::map<int, ModelVolume*> m_shared_meshes;

        //BBS: plater related structures
        bool m_is_bbl_3mf { false };
        bool m_parsing_slice_info { false };
        PlateDataMaps m_plater_data;
        PlateData* m_curr_plater;
        CurrentInstance m_curr_instance;

        int m_current_color_group{-1};
        std::map<int, std::string> m_group_id_to_color;

    public:
        _BBS_3MF_Importer();
        ~_BBS_3MF_Importer();

        //BBS: add plate data related logic
        // add backup & restore logic
        bool load_model_from_file(const std::string& filename, Model& model, PlateDataPtrs& plate_data_list, std::vector<Preset*>& project_presets, DynamicPrintConfig& config,
            ConfigSubstitutionContext& config_substitutions, LoadStrategy strategy, bool* is_bbl_3mf, Semver& file_version, Import3mfProgressFn proFn = nullptr, BBLProject *project = nullptr, int plate_id = 0);
        bool get_thumbnail(const std::string &filename, std::string &data);
        bool load_gcode_3mf_from_stream(std::istream & data, Model& model, PlateDataPtrs& plate_data_list, DynamicPrintConfig& config, Semver& file_version);
        unsigned int version() const { return m_version; }

    private:
        void _destroy_xml_parser();
        void _stop_xml_parser(const std::string& msg = std::string());

        bool        parse_error()         const { return m_parse_error; }
        const char* parse_error_message() const {
            return m_parse_error ?
                // The error was signalled by the user code, not the expat parser.
                (m_parse_error_message.empty() ? "Invalid 3MF format" : m_parse_error_message.c_str()) :
                // The error was signalled by the expat parser.
                XML_ErrorString(XML_GetErrorCode(m_xml_parser));
        }

        //BBS: add plate data related logic
        // add backup & restore logic
        bool _load_model_from_file(std::string filename, Model& model, PlateDataPtrs& plate_data_list, std::vector<Preset*>& project_presets, DynamicPrintConfig& config, ConfigSubstitutionContext& config_substitutions, Import3mfProgressFn proFn = nullptr,
            BBLProject* project = nullptr, int plate_id = 0);
        bool _is_svg_shape_file(const std::string &filename) const;
        bool _extract_from_archive(mz_zip_archive& archive, std::string const & path, std::function<bool (mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)>, bool restore = false);
        bool _extract_xml_from_archive(mz_zip_archive& archive, std::string const & path, XML_StartElementHandler start_handler, XML_EndElementHandler end_handler);
        bool _extract_xml_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, XML_StartElementHandler start_handler, XML_EndElementHandler end_handler);
        bool _extract_model_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);
        void _extract_cut_information_from_archive(mz_zip_archive &archive, const mz_zip_archive_file_stat &stat, ConfigSubstitutionContext &config_substitutions);
        void _extract_layer_heights_profile_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);
        void _extract_layer_config_ranges_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, ConfigSubstitutionContext& config_substitutions);
        void _extract_sla_support_points_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);
        void _extract_sla_drain_holes_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);
        void _extract_brim_ear_points_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);

        void _extract_custom_gcode_per_print_z_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);

        void _extract_print_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, DynamicPrintConfig& config, ConfigSubstitutionContext& subs_context, const std::string& archive_filename);
        //BBS: add project config file logic
        void _extract_project_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, DynamicPrintConfig& config, ConfigSubstitutionContext& subs_context, Model& model);
        //BBS: extract project embedded presets
        void _extract_project_embedded_presets_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, std::vector<Preset*>&project_presets, Model& model, Preset::Type type, bool use_json = true);

        void _extract_auxiliary_file_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, Model& model);
        void _extract_file_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);
        void _extract_embossed_svg_shape_file(const std::string &filename, mz_zip_archive &archive, const mz_zip_archive_file_stat &stat);

        // handlers to parse the .model file
        void _handle_start_model_xml_element(const char* name, const char** attributes);
        void _handle_end_model_xml_element(const char* name);
        void _handle_xml_characters(const XML_Char* s, int len);

        // handlers to parse the MODEL_CONFIG_FILE file
        void _handle_start_config_xml_element(const char* name, const char** attributes);
        void _handle_end_config_xml_element(const char* name);

        bool _handle_start_model(const char** attributes, unsigned int num_attributes);
        bool _handle_end_model();

        bool _handle_start_resources(const char** attributes, unsigned int num_attributes);
        bool _handle_end_resources();

        bool _handle_start_object(const char** attributes, unsigned int num_attributes);
        bool _handle_end_object();

        bool _handle_start_color_group(const char **attributes, unsigned int num_attributes);
        bool _handle_end_color_group();

        bool _handle_start_color(const char **attributes, unsigned int num_attributes);
        bool _handle_end_color();

        bool _handle_start_mesh(const char** attributes, unsigned int num_attributes);
        bool _handle_end_mesh();

        bool _handle_start_vertices(const char** attributes, unsigned int num_attributes);
        bool _handle_end_vertices();

        bool _handle_start_vertex(const char** attributes, unsigned int num_attributes);
        bool _handle_end_vertex();

        bool _handle_start_triangles(const char** attributes, unsigned int num_attributes);
        bool _handle_end_triangles();

        bool _handle_start_triangle(const char** attributes, unsigned int num_attributes);
        bool _handle_end_triangle();

        bool _handle_start_components(const char** attributes, unsigned int num_attributes);
        bool _handle_end_components();

        bool _handle_start_component(const char** attributes, unsigned int num_attributes);
        bool _handle_end_component();

        bool _handle_start_build(const char** attributes, unsigned int num_attributes);
        bool _handle_end_build();

        bool _handle_start_item(const char** attributes, unsigned int num_attributes);
        bool _handle_end_item();

        bool _handle_start_metadata(const char** attributes, unsigned int num_attributes);
        bool _handle_end_metadata();

        bool _handle_start_text_configuration(const char** attributes, unsigned int num_attributes);
        bool _handle_start_shape_configuration(const char **attributes, unsigned int num_attributes);

        bool _create_object_instance(std::string const & path, int object_id, const Transform3d& transform, const bool printable, unsigned int recur_counter);

        void _apply_transform(ModelInstance& instance, const Transform3d& transform);

        bool _handle_start_config(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config();

        bool _handle_start_config_object(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config_object();

        bool _handle_start_config_volume(const char** attributes, unsigned int num_attributes);
        bool _handle_start_config_volume_mesh(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config_volume();
        bool _handle_end_config_volume_mesh();

        bool _handle_start_config_metadata(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config_metadata();

        bool _handle_start_config_filament(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config_filament();

        bool _handle_start_config_warning(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config_warning();

        //BBS: add plater config parse functions
        bool _handle_start_config_plater(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config_plater();

        bool _handle_start_config_plater_instance(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config_plater_instance();

        bool _handle_start_assemble(const char** attributes, unsigned int num_attributes);
        bool _handle_end_assemble();

        bool _handle_start_assemble_item(const char** attributes, unsigned int num_attributes);
        bool _handle_end_assemble_item();

        bool _handle_start_text_info_item(const char **attributes, unsigned int num_attributes);
        bool _handle_end_text_info_item();

        // BBS: callbacks to parse the .rels file
        static void XMLCALL _handle_start_relationships_element(void* userData, const char* name, const char** attributes);
        static void XMLCALL _handle_end_relationships_element(void* userData, const char* name);

        void _handle_start_relationships_element(const char* name, const char** attributes);
        void _handle_end_relationships_element(const char* name);

        bool _handle_start_relationship(const char** attributes, unsigned int num_attributes);

        void _generate_current_object_list(std::vector<Component> &sub_objects, Id object_id, IdToCurrentObjectMap& current_objects);
        bool _generate_volumes_new(ModelObject& object, const std::vector<Component> &sub_objects, const ObjectMetadata::VolumeMetadataList& volumes, ConfigSubstitutionContext& config_substitutions);
        //bool _generate_volumes(ModelObject& object, const Geometry& geometry, const ObjectMetadata::VolumeMetadataList& volumes, ConfigSubstitutionContext& config_substitutions);

        // callbacks to parse the .model file
        static void XMLCALL _handle_start_model_xml_element(void* userData, const char* name, const char** attributes);
        static void XMLCALL _handle_end_model_xml_element(void* userData, const char* name);
        static void XMLCALL _handle_xml_characters(void* userData, const XML_Char* s, int len);

        // callbacks to parse the MODEL_CONFIG_FILE file
        static void XMLCALL _handle_start_config_xml_element(void* userData, const char* name, const char** attributes);
        static void XMLCALL _handle_end_config_xml_element(void* userData, const char* name);
    };

    _BBS_3MF_Importer::_BBS_3MF_Importer()
        : m_version(0)
        , m_check_version(false)
        , m_xml_parser(nullptr)
        , m_model(nullptr)
        , m_unit_factor(1.0f)
        , m_curr_metadata_name("")
        , m_curr_characters("")
        , m_name("")
        , m_curr_plater(nullptr)
    {
    }

    _BBS_3MF_Importer::~_BBS_3MF_Importer()
    {
        _destroy_xml_parser();
        clear_errors();

        if (m_curr_object) {
            delete m_curr_object;
            m_curr_object = nullptr;
        }
        m_current_objects.clear();
        m_index_paths.clear();
        m_objects.clear();
        m_instances.clear();
        m_objects_metadata.clear();
        m_curr_metadata_name.clear();
        m_curr_characters.clear();

        std::map<int, PlateData*>::iterator it = m_plater_data.begin();
        while (it != m_plater_data.end())
        {
            delete it->second;
            it++;
        }
        m_plater_data.clear();
    }

    //BBS: add plate data related logic
        // add backup & restore logic
    bool _BBS_3MF_Importer::load_model_from_file(const std::string& filename, Model& model, PlateDataPtrs& plate_data_list, std::vector<Preset*>& project_presets, DynamicPrintConfig& config,
        ConfigSubstitutionContext& config_substitutions, LoadStrategy strategy, bool* is_bbl_3mf, Semver& file_version, Import3mfProgressFn proFn, BBLProject *project, int plate_id)
    {
        m_version = 0;
        m_fdm_supports_painting_version = 0;
        m_seam_painting_version = 0;
        m_mm_painting_version = 0;
        m_check_version = strategy & LoadStrategy::CheckVersion;
        //BBS: auxiliary data
        m_load_model  = strategy & LoadStrategy::LoadModel;
        m_load_aux = strategy & LoadStrategy::LoadAuxiliary;
        m_load_restore = strategy & LoadStrategy::Restore;
        m_load_config = strategy & LoadStrategy::LoadConfig;
        m_model = &model;
        m_unit_factor = 1.0f;
        m_curr_object = nullptr;
        m_current_objects.clear();
        m_index_paths.clear();
        m_objects.clear();
        //m_objects_aliases.clear();
        m_instances.clear();
        //m_geometries.clear();
        m_curr_config.object_id = -1;
        m_curr_config.volume_id = -1;
        m_objects_metadata.clear();
        m_layer_heights_profiles.clear();
        m_layer_config_ranges.clear();
        m_brim_ear_points.clear();
        //m_sla_support_points.clear();
        m_curr_metadata_name.clear();
        m_curr_characters.clear();
        //BBS: plater data init
        m_plater_data.clear();
        m_curr_instance.object_id = -1;
        m_curr_instance.instance_id = -1;
        m_curr_instance.identify_id = 0;
        clear_errors();

        // restore
        if (m_load_restore) {
            m_backup_path = filename.substr(0, filename.size() - 5);
            model.set_backup_path(m_backup_path);
            try {
                if (boost::filesystem::exists(model.get_backup_path() + "/origin.txt"))
                    load_string_file(model.get_backup_path() + "/origin.txt", m_origin_file);
            } catch (...) {}
            save_string_file(
                model.get_backup_path() + "/lock.txt",
                boost::lexical_cast<std::string>(get_current_pid()));
        }
        else {
            m_backup_path = model.get_backup_path();
        }
        bool result = _load_model_from_file(filename, model, plate_data_list, project_presets, config, config_substitutions, proFn, project, plate_id);
        if (is_bbl_3mf) {
            *is_bbl_3mf = m_is_bbl_3mf;
        }
        if (m_bambuslicer_generator_version)
            file_version = *m_bambuslicer_generator_version;
        // save for restore
        if (result && m_load_aux && !m_load_restore) {
            save_string_file(model.get_backup_path() + "/origin.txt", filename);
        }
        if (m_load_restore && !result) // not clear failed backup data for later analyze
            model.set_backup_path("detach");
        return result;
    }

    bool _BBS_3MF_Importer::get_thumbnail(const std::string &filename, std::string &data)
    {
        mz_zip_archive archive;
        mz_zip_zero_struct(&archive);

        struct close_lock
        {
            mz_zip_archive *archive;
            void            close()
            {
                if (archive) {
                    close_zip_reader(archive);
                    archive = nullptr;
                }
            }
            ~close_lock() { close(); }
        } lock{&archive};

        if (!open_zip_reader(&archive, filename)) {
            add_error("Unable to open the file"+filename);
            return false;
        }

        // BBS: load relationships
        if (!_extract_xml_from_archive(archive, RELATIONSHIPS_FILE, _handle_start_relationships_element, _handle_end_relationships_element))
            return false;
        if (m_thumbnail_middle.empty()) m_thumbnail_middle = m_thumbnail_path;
        if (!m_thumbnail_middle.empty()) {
            return _extract_from_archive(archive, m_thumbnail_middle, [&data](auto &archive, auto const &stat) -> bool {
                data.resize(stat.m_uncomp_size);
                return mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, data.data(), data.size(), 0);
            });
        }
        return _extract_from_archive(archive, THUMBNAIL_FILE, [&data](auto &archive, auto const &stat) -> bool {
            data.resize(stat.m_uncomp_size);
            return mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, data.data(), data.size(), 0);
        });
    }

    static size_t mz_zip_read_istream(void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n)
    {
        auto & is = *reinterpret_cast<std::istream*>(pOpaque);
        is.seekg(file_ofs, std::istream::beg);
        if (!is)
            return 0;
        is.read((char *)pBuf, n);
        return is.gcount();
    }

    bool _BBS_3MF_Importer::load_gcode_3mf_from_stream(std::istream &data, Model &model, PlateDataPtrs &plate_data_list, DynamicPrintConfig &config, Semver &file_version)
    {
        mz_zip_archive archive;
        mz_zip_zero_struct(&archive);
        archive.m_pRead = mz_zip_read_istream;
        archive.m_pIO_opaque = &data;

        data.seekg(0, std::istream::end);
        mz_uint64 size = data.tellg();
        data.seekg(0, std::istream::beg);
        if (!mz_zip_reader_init(&archive, size, 0))
            return false;

        struct close_lock
        {
            mz_zip_archive *archive;
            void            close()
            {
                if (archive) {
                    mz_zip_reader_end(archive);
                    archive = nullptr;
                }
            }
            ~close_lock() { close(); }
        } lock{&archive};

        // BBS: load relationships
        if (!_extract_xml_from_archive(archive, RELATIONSHIPS_FILE, _handle_start_relationships_element, _handle_end_relationships_element))
            return false;
        if (m_start_part_path.empty())
            return false;

        //extract model files
        m_model = &model;
        if (!_extract_from_archive(archive, m_start_part_path, [this] (mz_zip_archive& archive, const mz_zip_archive_file_stat& stat) {
                    return _extract_model_from_archive(archive, stat);
            })) {
            add_error("Archive does not contain a valid model");
            return false;
        }

        if (!m_designer.empty()) {
            m_model->design_info                 = std::make_shared<ModelDesignInfo>();
            m_model->design_info->DesignerUserId = m_designer_user_id;
            m_model->design_info->Designer       = m_designer;
        }

        m_model->profile_info                     = std::make_shared<ModelProfileInfo>();
        m_model->profile_info->ProfileTile        = m_profile_title;
        m_model->profile_info->ProfileCover       = m_profile_cover;
        m_model->profile_info->ProfileDescription = m_Profile_description;
        m_model->profile_info->ProfileUserId      = m_profile_user_id;
        m_model->profile_info->ProfileUserName    = m_profile_user_name;

        m_model->model_info = std::make_shared<ModelInfo>();
        m_model->model_info->load(model_info);

        if (m_thumbnail_middle.empty()) m_thumbnail_middle = m_thumbnail_path;
        if (m_thumbnail_small.empty()) m_thumbnail_small = m_thumbnail_path;
        if (!m_thumbnail_small.empty() && m_thumbnail_small.front() == '/')
            m_thumbnail_small.erase(m_thumbnail_small.begin());
        if (!m_thumbnail_middle.empty() && m_thumbnail_middle.front() == '/')
            m_thumbnail_middle.erase(m_thumbnail_middle.begin());
        m_model->model_info->metadata_items.emplace("Thumbnail", m_thumbnail_middle);
        m_model->model_info->metadata_items.emplace("Poster", m_thumbnail_middle);

        // we then loop again the entries to read other files stored in the archive
        mz_uint num_entries = mz_zip_reader_get_num_files(&archive);
        mz_zip_archive_file_stat stat;
        for (mz_uint i = 0; i < num_entries; ++i) {
            if (mz_zip_reader_file_stat(&archive, i, &stat)) {
                std::string name(stat.m_filename);
                std::replace(name.begin(), name.end(), '\\', '/');

                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("extract %1%th file %2%, total=%3%\n")%(i+1)%name%num_entries;

                if (boost::algorithm::iequals(name, BBS_PROJECT_CONFIG_FILE)) {
                    // extract slic3r print config file
                    ConfigSubstitutionContext config_substitutions(ForwardCompatibilitySubstitutionRule::Disable);
                    _extract_project_config_from_archive(archive, stat, config, config_substitutions, model);
                }
                else if (boost::algorithm::iequals(name, BBS_MODEL_CONFIG_FILE)) {
                    // extract slic3r model config file
                    if (!_extract_xml_from_archive(archive, stat, _handle_start_config_xml_element, _handle_end_config_xml_element)) {
                        add_error("Archive does not contain a valid model config");
                        return false;
                    }
                }
                else if (boost::algorithm::iequals(name, SLICE_INFO_CONFIG_FILE)) {
                    m_parsing_slice_info = true;
                    //extract slice info from archive
                    _extract_xml_from_archive(archive, stat, _handle_start_config_xml_element, _handle_end_config_xml_element);
                    m_parsing_slice_info = false;
                }
            }
        }

        //BBS: load the plate info into plate_data_list
        std::map<int, PlateData*>::iterator it = m_plater_data.begin();
        plate_data_list.clear();
        plate_data_list.reserve(m_plater_data.size());
        for (unsigned int i = 0; i < m_plater_data.size(); i++)
        {
            PlateData* plate = new PlateData();
            plate_data_list.push_back(plate);
        }
        while (it != m_plater_data.end())
        {
            if (it->first > m_plater_data.size())
            {
                add_error("invalid plate index");
                return false;
            }
            PlateData * plate = plate_data_list[it->first-1];
            plate->locked = it->second->locked;
            plate->plate_index = it->second->plate_index-1;
            plate->obj_inst_map = it->second->obj_inst_map;
            plate->gcode_file = it->second->gcode_file;
            plate->gcode_prediction = it->second->gcode_prediction;
            plate->gcode_weight = it->second->gcode_weight;
            plate->toolpath_outside = it->second->toolpath_outside;
            plate->is_support_used = it->second->is_support_used;
            plate->is_label_object_enabled = it->second->is_label_object_enabled;
            plate->skipped_objects = it->second->skipped_objects;
            plate->slice_filaments_info = it->second->slice_filaments_info;
            plate->printer_model_id = it->second->printer_model_id;
            plate->nozzle_diameters = it->second->nozzle_diameters;
            plate->warnings = it->second->warnings;
            plate->thumbnail_file = it->second->thumbnail_file;
            if (plate->thumbnail_file.empty()) {
                plate->thumbnail_file = plate->gcode_file;
                boost::algorithm::replace_all(plate->thumbnail_file, ".gcode", ".png");
            }
            //plate->pattern_file = it->second->pattern_file;
            plate->no_light_thumbnail_file = it->second->no_light_thumbnail_file;
            plate->top_file = it->second->top_file;
            plate->pick_file = it->second->pick_file.empty();
            plate->pattern_bbox_file = it->second->pattern_bbox_file.empty();
            plate->config = it->second->config;

            if (!plate->thumbnail_file.empty())
                _extract_from_archive(archive, plate->thumbnail_file, [&pixels = plate_data_list[it->first - 1]->plate_thumbnail.pixels](auto &archive, auto const &stat) -> bool {
                    pixels.resize(stat.m_uncomp_size);
                    return mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, pixels.data(), pixels.size(), 0);
                });

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", plate %1%, thumbnail_file=%2%, no_light_thumbnail_file=%3%")%it->first %plate->thumbnail_file %plate->no_light_thumbnail_file;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", top_thumbnail_file=%1%, pick_thumbnail_file=%2%")%plate->top_file %plate->pick_file;
            it++;
        }

        lock.close();

        return true;
    }

    void _BBS_3MF_Importer::_destroy_xml_parser()
    {
        if (m_xml_parser != nullptr) {
            XML_ParserFree(m_xml_parser);
            m_xml_parser = nullptr;
        }
    }

    void _BBS_3MF_Importer::_stop_xml_parser(const std::string &msg)
    {
        assert(! m_parse_error);
        assert(m_parse_error_message.empty());
        assert(m_xml_parser != nullptr);
        m_parse_error = true;
        m_parse_error_message = msg;
        XML_StopParser(m_xml_parser, false);
    }

    //BBS: add plate data related logic
    bool _BBS_3MF_Importer::_load_model_from_file(
        std::string filename,
        Model& model,
        PlateDataPtrs& plate_data_list,
        std::vector<Preset*>& project_presets,
        DynamicPrintConfig& config,
        ConfigSubstitutionContext& config_substitutions,
        Import3mfProgressFn proFn,
        BBLProject *project,
        int plate_id)
    {
        bool cb_cancel = false;
        //BBS progress point
        // prepare restore
        if (m_load_restore) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("import 3mf IMPORT_STAGE_RESTORE\n");
            if (proFn) {
                proFn(IMPORT_STAGE_RESTORE, 0, 1, cb_cancel);
                if (cb_cancel)
                    return false;
            }
        }

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("import 3mf IMPORT_STAGE_OPEN, m_load_restore=%1%\n")%m_load_restore;
        if (proFn) {
            proFn(IMPORT_STAGE_OPEN, 0, 1, cb_cancel);
            if (cb_cancel)
                return false;
        }

        mz_zip_archive archive;
        mz_zip_zero_struct(&archive);

        struct close_lock
        {
            mz_zip_archive * archive;
            void close() {
                if (archive) {
                    close_zip_reader(archive);
                    archive = nullptr;
                }
            }
            ~close_lock() {
                close();
            }
        } lock{ &archive };

        if (!open_zip_reader(&archive, filename)) {
            add_error("Unable to open the file"+filename);
            return false;
        }

        mz_uint num_entries = mz_zip_reader_get_num_files(&archive);

        mz_zip_archive_file_stat stat;

        m_name = boost::filesystem::path(filename).stem().string();

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("import 3mf IMPORT_STAGE_READ_FILES\n");
        if (proFn) {
            proFn(IMPORT_STAGE_READ_FILES, 0, 3, cb_cancel);
            if (cb_cancel)
                return false;
        }
        // BBS: load relationships
        if (!_extract_xml_from_archive(archive, RELATIONSHIPS_FILE, _handle_start_relationships_element, _handle_end_relationships_element))
            return false;
        if (m_start_part_path.empty())
            return false;
        // BBS: load sub models (Production Extension)
        std::string sub_rels = m_start_part_path;
        sub_rels.insert(boost::find_last(sub_rels, "/").end() - sub_rels.begin(), "_rels/");
        sub_rels.append(".rels");
        if (sub_rels.front() == '/') sub_rels = sub_rels.substr(1);

        //check whether sub relation file is exist or not
        int sub_index = mz_zip_reader_locate_file(&archive, sub_rels.c_str(), nullptr, 0);
        if (sub_index == -1) {
            //no submodule files found, use only one 3dmodel.model
        }
        else {
            _extract_xml_from_archive(archive, sub_rels, _handle_start_relationships_element, _handle_end_relationships_element);
            int index = 0;

#if 0
            for (auto path : m_sub_model_paths) {
                if (proFn) {
                    proFn(IMPORT_STAGE_READ_FILES, ++index, 3 + m_sub_model_paths.size(), cb_cancel);
                    if (cb_cancel)
                        return false;
                }
                else
                    ++index;
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", read %1%th sub model file %2%\n")%index %path;
                m_sub_model_path = path;
                if (!_extract_from_archive(archive, path, [this] (mz_zip_archive& archive, const mz_zip_archive_file_stat& stat) {
                    return _extract_model_from_archive(archive, stat);
                }, m_load_restore)) {
                    add_error("Archive does not contain a valid model");
                    return false;
                }
                m_sub_model_path.clear();
            }
#else
            for (auto path : m_sub_model_paths) {
                ObjectImporter *object_importer = new ObjectImporter(this, filename, path);
                m_object_importers.push_back(object_importer);
            }

            bool object_load_result = true;
            boost::mutex mutex;
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, m_object_importers.size()),
                [this, &mutex, &object_load_result](const tbb::blocked_range<size_t>& importer_range) {
                    CNumericLocalesSetter locales_setter;
                    for (size_t object_index = importer_range.begin(); object_index < importer_range.end(); ++ object_index) {
                        bool result = m_object_importers[object_index]->extract_object_model();
                        {
                            boost::unique_lock l(mutex);
                            object_load_result &= result;
                        }
                    }
                }
            );

            if (!object_load_result) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", loading sub-objects error\n");
                return false;
            }

            //merge these objects into one
            for (auto obj_importer : m_object_importers) {
                for (const IdToCurrentObjectMap::value_type&  obj : obj_importer->object_list)
                    m_current_objects.insert({ std::move(obj.first), std::move(obj.second)});
                for (auto group_color : obj_importer->object_group_id_to_color)
                    m_group_id_to_color.insert(std::move(group_color));

                delete obj_importer;
            }
            m_object_importers.clear();
#endif
            // BBS: load root model
            if (proFn) {
                proFn(IMPORT_STAGE_READ_FILES, 2, 3, cb_cancel);
                if (cb_cancel)
                    return false;
            }
        }

        //extract model files
        if (!_extract_from_archive(archive, m_start_part_path, [this] (mz_zip_archive& archive, const mz_zip_archive_file_stat& stat) {
                    return _extract_model_from_archive(archive, stat);
            })) {
            add_error("Archive does not contain a valid model");
            return false;
        }

        if (!m_designer.empty()) {
            m_model->design_info = std::make_shared<ModelDesignInfo>();
            m_model->design_info->DesignerUserId = m_designer_user_id;
            m_model->design_info->Designer = m_designer;
        }

        m_model->profile_info = std::make_shared<ModelProfileInfo>();
        m_model->profile_info->ProfileTile = m_profile_title;
        m_model->profile_info->ProfileCover = m_profile_cover;
        m_model->profile_info->ProfileDescription = m_Profile_description;
        m_model->profile_info->ProfileUserId = m_profile_user_id;
        m_model->profile_info->ProfileUserName = m_profile_user_name;


        m_model->model_info = std::make_shared<ModelInfo>();
        m_model->model_info->load(model_info);
        if (!m_thumbnail_small.empty()) m_model->model_info->metadata_items.emplace("Thumbnail_Small", m_thumbnail_small);
        if (!m_thumbnail_middle.empty()) m_model->model_info->metadata_items.emplace("Thumbnail_Middle", m_thumbnail_middle);

        //got project id
        if (project) {
            project->project_model_id = m_model_id;
            project->project_country_code = m_contry_code;
        }

        // Orca: skip version check
        bool dont_load_config = !m_load_config;
        // if (m_bambuslicer_generator_version) {
        //     Semver app_version = *(Semver::parse(SoftFever_VERSION));
        //     Semver file_version = *m_bambuslicer_generator_version;
        //     if (file_version.maj() != app_version.maj())
        //         dont_load_config = true;
        // }
        // else {
        //     m_bambuslicer_generator_version = Semver::parse("0.0.0.0");
        //     dont_load_config = true;
        // }

        // we then loop again the entries to read other files stored in the archive
        for (mz_uint i = 0; i < num_entries; ++i) {
            if (mz_zip_reader_file_stat(&archive, i, &stat)) {

                //BBS progress point
                if (proFn) {
                    proFn(IMPORT_STAGE_EXTRACT, i, num_entries, cb_cancel);
                    if (cb_cancel)
                        return false;
                }

                std::string name(stat.m_filename);
                std::replace(name.begin(), name.end(), '\\', '/');

                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("extract %1%th file %2%, total=%3%")%(i+1)%name%num_entries;

                if (name.find("/../") != std::string::npos) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", find file path including /../, not valid, skip it\n");
                    continue;
                }

                if (boost::algorithm::iequals(name, BBS_LAYER_HEIGHTS_PROFILE_FILE)) {
                    // extract slic3r layer heights profile file
                    _extract_layer_heights_profile_config_from_archive(archive, stat);
                }
                else
                if (boost::algorithm::iequals(name, LAYER_CONFIG_RANGES_FILE)) {
                    // extract slic3r layer config ranges file
                    _extract_layer_config_ranges_from_archive(archive, stat, config_substitutions);
                }
                else if (boost::algorithm::iequals(name, BRIM_EAR_POINTS_FILE)) {
                    // extract slic3r config file
                    _extract_brim_ear_points_from_archive(archive, stat);
                }
                //BBS: disable SLA related files currently
                /*else if (boost::algorithm::iequals(name, SLA_SUPPORT_POINTS_FILE)) {
                    // extract sla support points file
                    _extract_sla_support_points_from_archive(archive, stat);
                }
                else if (boost::algorithm::iequals(name, SLA_DRAIN_HOLES_FILE)) {
                    // extract sla support points file
                    _extract_sla_drain_holes_from_archive(archive, stat);
                }*/
                //BBS: project setting file
                //if (!dont_load_config && boost::algorithm::iequals(name, BBS_PRINT_CONFIG_FILE)) {
                    // extract slic3r print config file
                //    _extract_print_config_from_archive(archive, stat, config, config_substitutions, filename);
                //} else
                if (!dont_load_config && boost::algorithm::iequals(name, BBS_PROJECT_CONFIG_FILE)) {
                    // extract slic3r print config file
                    _extract_project_config_from_archive(archive, stat, config, config_substitutions, model);
                }
                else if (boost::algorithm::iequals(name, CUT_INFORMATION_FILE)) {
                    // extract object cut info
                    _extract_cut_information_from_archive(archive, stat, config_substitutions);
                }
                //BBS: project embedded presets
                else if (!dont_load_config && boost::algorithm::istarts_with(name, PROJECT_EMBEDDED_PRINT_PRESETS_FILE)) {
                    // extract slic3r layer config ranges file
                    _extract_project_embedded_presets_from_archive(archive, stat, project_presets, model, Preset::TYPE_PRINT, false);
                }
                else if (!dont_load_config && boost::algorithm::istarts_with(name, PROJECT_EMBEDDED_SLICE_PRESETS_FILE)) {
                    // extract slic3r layer config ranges file
                    _extract_project_embedded_presets_from_archive(archive, stat, project_presets, model, Preset::TYPE_PRINT);
                }
                else if (!dont_load_config && boost::algorithm::istarts_with(name, PROJECT_EMBEDDED_FILAMENT_PRESETS_FILE)) {
                    // extract slic3r layer config ranges file
                    _extract_project_embedded_presets_from_archive(archive, stat, project_presets, model, Preset::TYPE_FILAMENT);
                }
                else if (!dont_load_config && boost::algorithm::istarts_with(name, PROJECT_EMBEDDED_PRINTER_PRESETS_FILE)) {
                    // extract slic3r layer config ranges file
                    _extract_project_embedded_presets_from_archive(archive, stat, project_presets, model, Preset::TYPE_PRINTER);
                }
                else if (!dont_load_config && boost::algorithm::iequals(name, CUSTOM_GCODE_PER_PRINT_Z_FILE)) {
                    // extract slic3r layer config ranges file
                    _extract_custom_gcode_per_print_z_from_archive(archive, stat);
                }
                else if (boost::algorithm::iequals(name, BBS_MODEL_CONFIG_FILE)) {
                    // extract slic3r model config file
                    if (!_extract_xml_from_archive(archive, stat, _handle_start_config_xml_element, _handle_end_config_xml_element)) {
                        if (m_is_bbl_3mf) {
                            add_error("Archive does not contain a valid model config");
                            return false;
                        }
                    }
                }
                else if (_is_svg_shape_file(name)) {
                    _extract_embossed_svg_shape_file(name, archive, stat);
                }
                else if (!dont_load_config && boost::algorithm::iequals(name, SLICE_INFO_CONFIG_FILE)) {
                    m_parsing_slice_info = true;
                    //extract slice info from archive
                    _extract_xml_from_archive(archive, stat, _handle_start_config_xml_element, _handle_end_config_xml_element);
                    m_parsing_slice_info = false;
                }
                else if (boost::algorithm::istarts_with(name, AUXILIARY_DIR)) {
                    // extract auxiliary directory to temp directory, do nothing for restore
                    if (m_load_aux && !m_load_restore)
                        _extract_auxiliary_file_from_archive(archive, stat, model);
                }
                else if (!dont_load_config && boost::algorithm::istarts_with(name, METADATA_DIR) && boost::algorithm::iends_with(name, GCODE_EXTENSION)) {
                    //load gcode files
                    _extract_file_from_archive(archive, stat);
                }
                else if (!dont_load_config && boost::algorithm::istarts_with(name, METADATA_DIR) && boost::algorithm::iends_with(name, THUMBNAIL_EXTENSION)) {
                    //BBS parsing pattern thumbnail and plate thumbnails
                    _extract_file_from_archive(archive, stat);
                }
                else if (!dont_load_config && boost::algorithm::istarts_with(name, METADATA_DIR) && boost::algorithm::iends_with(name, CALIBRATION_INFO_EXTENSION)) {
                    //BBS parsing pattern config files
                    _extract_file_from_archive(archive, stat);
                }
                else {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", %1% skipped, already parsed or a directory or not supported\n")%name;
                }
            }
        }

        lock.close();

        if (!m_is_bbl_3mf) {
            // if the 3mf was not produced by OrcaSlicer and there is more than one instance,
            // split the object in as many objects as instances
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", found 3mf from other vendor, split as instance");
            for (const IdToModelObjectMap::value_type& object : m_objects) {
                if (object.second >= int(m_model->objects.size())) {
                    add_error("3rd 3mf, invalid object, id: "+std::to_string(object.first.second));
                    return false;
                }
                ModelObject* model_object = m_model->objects[object.second];
                if (model_object->instances.size() > 1) {
                    IdToCurrentObjectMap::const_iterator current_object = m_current_objects.find(object.first);
                    if (current_object == m_current_objects.end()) {
                        add_error("3rd 3mf, can not find object, id " + std::to_string(object.first.second));
                        return false;
                    }
                    std::vector<Component> object_id_list;
                    _generate_current_object_list(object_id_list, object.first, m_current_objects);

                    ObjectMetadata::VolumeMetadataList volumes;
                    ObjectMetadata::VolumeMetadataList* volumes_ptr = nullptr;

                    for (int k = 0; k < object_id_list.size(); k++)
                    {
                        Id object_id = object_id_list[k].object_id;
                        volumes.emplace_back(object_id.second);
                    }

                    // select as volumes
                    volumes_ptr = &volumes;

                    // for each instance after the 1st, create a new model object containing only that instance
                    // and copy into it the geometry
                    while (model_object->instances.size() > 1) {
                        ModelObject* new_model_object = m_model->add_object(*model_object);
                        new_model_object->clear_instances();
                        new_model_object->add_instance(*model_object->instances.back());
                        model_object->delete_last_instance();
                        if (!_generate_volumes_new(*new_model_object, object_id_list, *volumes_ptr, config_substitutions))
                            return false;
                    }
                }
            }
        }

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", process group colors, size %1%\n")%m_group_id_to_color.size();
        std::map<int, int> color_group_id_to_extruder_id_map;
        std::map<std::string, int> color_to_extruder_id_map;
        int extruder_id = 0;
        for (auto group_iter = m_group_id_to_color.begin(); group_iter != m_group_id_to_color.end(); ++group_iter) {
            auto color_iter = color_to_extruder_id_map.find(group_iter->second);
            if (color_iter == color_to_extruder_id_map.end()) {
                ++extruder_id;
                color_to_extruder_id_map[group_iter->second] = extruder_id;
                color_group_id_to_extruder_id_map[group_iter->first] = extruder_id;
            } else {
                color_group_id_to_extruder_id_map[group_iter->first] = color_iter->second;
            }
        }

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", begin to assemble objects, size %1%\n")%m_objects.size();
        //only load objects in plate_id
        PlateData* current_plate_data = nullptr;
        if ((plate_id > 0) && (plate_id <= m_plater_data.size())) {
            std::map<int, PlateData*>::iterator it =m_plater_data.find(plate_id);
            if (it != m_plater_data.end()) {
                current_plate_data = it->second;
            }
        }
        for (const IdToModelObjectMap::value_type& object : m_objects) {
            if (object.second >= int(m_model->objects.size())) {
                add_error("invalid object, id: "+std::to_string(object.first.second));
                return false;
            }
            if (current_plate_data) {
                std::map<int, std::pair<int, int>>::iterator it = current_plate_data->obj_inst_map.find(object.first.second);
                if (it == current_plate_data->obj_inst_map.end()) {
                    //not in current plate, skip
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", could not find object %1% in plate %2%, skip it\n")%object.first.second %plate_id;
                    continue;
                }
            }
            ModelObject* model_object = m_model->objects[object.second];
            /*IdToGeometryMap::const_iterator obj_geometry = m_geometries.find(object.first);
            if (obj_geometry == m_geometries.end()) {
                add_error("Unable to find object geometry");
                return false;
            }*/

            // m_layer_heights_profiles are indexed by a 1 based model object index.
            IdToLayerHeightsProfileMap::iterator obj_layer_heights_profile = m_layer_heights_profiles.find(object.second + 1);
            if (obj_layer_heights_profile != m_layer_heights_profiles.end())
                model_object->layer_height_profile.set(std::move(obj_layer_heights_profile->second));

            // m_layer_config_ranges are indexed by a 1 based model object index.
            IdToLayerConfigRangesMap::iterator obj_layer_config_ranges = m_layer_config_ranges.find(object.second + 1);
            if (obj_layer_config_ranges != m_layer_config_ranges.end())
                model_object->layer_config_ranges = std::move(obj_layer_config_ranges->second);

            IdToBrimPointsMap::iterator obj_brim_points = m_brim_ear_points.find(object.second + 1);
            if (obj_brim_points != m_brim_ear_points.end())
                model_object->brim_points = std::move(obj_brim_points->second);

            // m_sla_support_points are indexed by a 1 based model object index.
            /*IdToSlaSupportPointsMap::iterator obj_sla_support_points = m_sla_support_points.find(object.second + 1);
            if (obj_sla_support_points != m_sla_support_points.end() && !obj_sla_support_points->second.empty()) {
                model_object->sla_support_points = std::move(obj_sla_support_points->second);
                model_object->sla_points_status = sla::PointsStatus::UserModified;
            }

            IdToSlaDrainHolesMap::iterator obj_drain_holes = m_sla_drain_holes.find(object.second + 1);
            if (obj_drain_holes != m_sla_drain_holes.end() && !obj_drain_holes->second.empty()) {
                model_object->sla_drain_holes = std::move(obj_drain_holes->second);
            }*/

            std::vector<Component> object_id_list;
            _generate_current_object_list(object_id_list, object.first, m_current_objects);

            ObjectMetadata::VolumeMetadataList volumes;
            ObjectMetadata::VolumeMetadataList* volumes_ptr = nullptr;

            IdToMetadataMap::iterator obj_metadata = m_objects_metadata.find(object.first.second);
            if (obj_metadata != m_objects_metadata.end()) {
                // config data has been found, this model was saved using slic3r pe

                // apply object's name and config data
                for (const Metadata& metadata : obj_metadata->second.metadata) {
                    if (metadata.key == "name")
                        model_object->name = metadata.value;
                    //BBS: add module name
                    else if (metadata.key == "module")
                        model_object->module_name = metadata.value;
                    else
                        model_object->config.set_deserialize(metadata.key, metadata.value, config_substitutions);
                }

                // select object's detected volumes
                volumes_ptr = &obj_metadata->second.volumes;
            }
            else {
                // config data not found, this model was not saved using slic3r pe

                // add the entire geometry as the single volume to generate
                //volumes.emplace_back(0, (int)obj_geometry->second.triangles.size() - 1);
                for (int k = 0; k < object_id_list.size(); k++)
                {
                    Id object_id = object_id_list[k].object_id;
                    volumes.emplace_back(object_id.second);
                }

                IdToCurrentObjectMap::const_iterator current_object = m_current_objects.find(object.first);
                if (current_object != m_current_objects.end()) {
                    // get name
                    if (!current_object->second.name.empty())
                        model_object->name = current_object->second.name;
                    else
                        model_object->name = "Object_"+std::to_string(object.second+1);

                    // get color
                    auto extruder_itor = color_group_id_to_extruder_id_map.find(current_object->second.pid);
                    if (extruder_itor != color_group_id_to_extruder_id_map.end()) {
                        model_object->config.set_key_value("extruder", new ConfigOptionInt(extruder_itor->second));
                    }
                }

                // select as volumes
                volumes_ptr = &volumes;
            }

            if (!_generate_volumes_new(*model_object, object_id_list, *volumes_ptr, config_substitutions))
                return false;

            // Apply cut information for object if any was loaded
            // m_cut_object_ids are indexed by a 1 based model object index.
            IdToCutObjectInfoMap::iterator cut_object_info = m_cut_object_infos.find(object.second + 1);
            if (cut_object_info != m_cut_object_infos.end()) {
                model_object->cut_id = cut_object_info->second.id;
                int vol_cnt = int(model_object->volumes.size());
                for (auto connector : cut_object_info->second.connectors) {
                    if (connector.volume_id < 0 || connector.volume_id >= vol_cnt) {
                        add_error("Invalid connector is found");
                        continue;
                    }
                    model_object->volumes[connector.volume_id]->cut_info = 
                        ModelVolume::CutInfo(CutConnectorType(connector.type), connector.r_tolerance, connector.h_tolerance, true);
                }
            }
        }

        // If instances contain a single volume, the volume offset should be 0,0,0
        // This equals to say that instance world position and volume world position should match
        // Correct all instances/volumes for which this does not hold
        for (int obj_id = 0; obj_id < int(model.objects.size()); ++obj_id) {
            ModelObject *o = model.objects[obj_id];
            if (o->volumes.size() == 1) {
                ModelVolume *                           v                 = o->volumes.front();
                const Slic3r::Geometry::Transformation &first_inst_trafo  = o->instances.front()->get_transformation();
                const Vec3d                             world_vol_offset  = (first_inst_trafo * v->get_transformation()).get_offset();
                const Vec3d                             world_inst_offset = first_inst_trafo.get_offset();

                if (!world_vol_offset.isApprox(world_inst_offset)) {
                    const Slic3r::Geometry::Transformation &vol_trafo = v->get_transformation();
                    for (int inst_id = 0; inst_id < int(o->instances.size()); ++inst_id) {
                        ModelInstance *                         i          = o->instances[inst_id];
                        const Slic3r::Geometry::Transformation &inst_trafo = i->get_transformation();
                        i->set_offset((inst_trafo * vol_trafo).get_offset());
                    }
                    v->set_offset(Vec3d::Zero());
                }
            }
        }

        int object_idx = 0;
        for (ModelObject* o : model.objects) {
            int volume_idx = 0;
            for (ModelVolume* v : o->volumes) {
                if (v->source.input_file.empty() && v->type() == ModelVolumeType::MODEL_PART) {
                    v->source.input_file = filename;
                    if (v->source.volume_idx == -1)
                        v->source.volume_idx = volume_idx;
                    if (v->source.object_idx == -1)
                        v->source.object_idx = object_idx;
                }
                ++volume_idx;
            }
            ++object_idx;
        }

        const ConfigOptionStrings* filament_ids_opt = config.option<ConfigOptionStrings>("filament_settings_id");
        int max_filament_id = filament_ids_opt ? filament_ids_opt->size() : std::numeric_limits<int>::max();
        for (ModelObject* mo : m_model->objects) {
            const ConfigOptionInt* extruder_opt = dynamic_cast<const ConfigOptionInt*>(mo->config.option("extruder"));
            int extruder_id = 0;
            if (extruder_opt != nullptr)
                extruder_id = extruder_opt->getInt();

            if (extruder_id == 0 || extruder_id > max_filament_id)
                mo->config.set_key_value("extruder", new ConfigOptionInt(1));

            if (mo->volumes.size() == 1) {
                mo->volumes[0]->config.erase("extruder");
            }
            else {
                for (ModelVolume* mv : mo->volumes) {
                    const ConfigOptionInt* vol_extruder_opt = dynamic_cast<const ConfigOptionInt*>(mv->config.option("extruder"));
                    if (vol_extruder_opt == nullptr)
                        continue;

                    if (vol_extruder_opt->getInt() == 0)
                        mv->config.erase("extruder");
                    else if (vol_extruder_opt->getInt() > max_filament_id)
                        mv->config.set_key_value("extruder", new ConfigOptionInt(1));
                }
            }
        }

//        // fixes the min z of the model if negative
//        model.adjust_min_z();

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("import 3mf IMPORT_STAGE_LOADING_PLATES, m_plater_data size %1%, m_backup_path %2%\n")%m_plater_data.size() %m_backup_path;
        if (proFn) {
            proFn(IMPORT_STAGE_LOADING_PLATES, 0, 1, cb_cancel);
            if (cb_cancel)
                return false;
        }

        //BBS: load the plate info into plate_data_list
        std::map<int, PlateData*>::iterator it = m_plater_data.begin();
        plate_data_list.clear();
        plate_data_list.reserve(m_plater_data.size());
        for (unsigned int i = 0; i < m_plater_data.size(); i++)
        {
            PlateData* plate = new PlateData();
            plate_data_list.push_back(plate);
        }
        while (it != m_plater_data.end())
        {
            if (it->first > m_plater_data.size())
            {
                add_error("invalid plate index");
                return false;
            }
            plate_data_list[it->first-1]->locked = it->second->locked;
            plate_data_list[it->first-1]->plate_index = it->second->plate_index-1;
            plate_data_list[it->first-1]->plate_name = it->second->plate_name;
            plate_data_list[it->first-1]->obj_inst_map = it->second->obj_inst_map;
            plate_data_list[it->first-1]->gcode_file = (m_load_restore || it->second->gcode_file.empty()) ? it->second->gcode_file : m_backup_path + "/" + it->second->gcode_file;
            plate_data_list[it->first-1]->gcode_prediction = it->second->gcode_prediction;
            plate_data_list[it->first-1]->gcode_weight = it->second->gcode_weight;
            plate_data_list[it->first-1]->toolpath_outside = it->second->toolpath_outside;
            plate_data_list[it->first-1]->is_support_used = it->second->is_support_used;
            plate_data_list[it->first-1]->is_label_object_enabled = it->second->is_label_object_enabled;
            plate_data_list[it->first-1]->slice_filaments_info = it->second->slice_filaments_info;
            plate_data_list[it->first-1]->skipped_objects = it->second->skipped_objects;
            plate_data_list[it->first-1]->warnings = it->second->warnings;
            plate_data_list[it->first-1]->thumbnail_file = (m_load_restore || it->second->thumbnail_file.empty()) ? it->second->thumbnail_file : m_backup_path + "/" + it->second->thumbnail_file;
            //plate_data_list[it->first-1]->pattern_file = (m_load_restore || it->second->pattern_file.empty()) ? it->second->pattern_file : m_backup_path + "/" + it->second->pattern_file;
            plate_data_list[it->first-1]->no_light_thumbnail_file = (m_load_restore || it->second->no_light_thumbnail_file.empty()) ? it->second->no_light_thumbnail_file : m_backup_path + "/" + it->second->no_light_thumbnail_file;
            plate_data_list[it->first-1]->top_file = (m_load_restore || it->second->top_file.empty()) ? it->second->top_file : m_backup_path + "/" + it->second->top_file;
            plate_data_list[it->first-1]->pick_file = (m_load_restore || it->second->pick_file.empty()) ? it->second->pick_file : m_backup_path + "/" + it->second->pick_file;
            plate_data_list[it->first-1]->pattern_bbox_file = (m_load_restore || it->second->pattern_bbox_file.empty()) ? it->second->pattern_bbox_file : m_backup_path + "/" + it->second->pattern_bbox_file;
            plate_data_list[it->first-1]->config = it->second->config;

            current_plate_data = plate_data_list[it->first - 1];
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", plate %1%, thumbnail_file=%2%, no_light_thumbnail_file=%3%")%it->first %plate_data_list[it->first-1]->thumbnail_file %plate_data_list[it->first-1]->no_light_thumbnail_file;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", top_thumbnail_file=%1%, pick_thumbnail_file=%2%")%plate_data_list[it->first-1]->top_file %plate_data_list[it->first-1]->pick_file;
            it++;

            //update the arrange order
            std::map<int, std::pair<int, int>>::iterator map_it = current_plate_data->obj_inst_map.begin();
            while (map_it != current_plate_data->obj_inst_map.end()) {
                int obj_index, obj_id = map_it->first, inst_index = map_it->second.first;
                IndexToPathMap::iterator index_iter = m_index_paths.find(obj_id);
                if (index_iter == m_index_paths.end()) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ":" << __LINE__
                        << boost::format(", can not find object from plate's obj_map, id=%1%, skip this object")%obj_id;
                    map_it++;
                    continue;
                }
                Id temp_id = std::make_pair(index_iter->second, index_iter->first);
                IdToModelObjectMap::iterator object_item = m_objects.find(temp_id);
                if (object_item == m_objects.end()) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ":" << __LINE__
                        << boost::format(", can not find object from plate's obj_map, ID <%1%, %2%>, skip this object")%index_iter->second %index_iter->first;
                    map_it++;
                    continue;
                }
                obj_index = object_item->second;

                if (obj_index >= m_model->objects.size()) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ":" << __LINE__ << boost::format("invalid object id %1%\n")%obj_index;
                    map_it++;
                    continue;
                }
                ModelObject* obj =  m_model->objects[obj_index];
                if (inst_index >= obj->instances.size()) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ":" << __LINE__ << boost::format("invalid instance id %1%\n")%inst_index;
                    map_it++;
                    continue;
                }
                ModelInstance* inst =  obj->instances[inst_index];
                inst->loaded_id = map_it->second.second;
                map_it++;
            }
        }

        if ((plate_id > 0) && (plate_id <= m_plater_data.size())) {
            //remove the no need objects
            std::vector<size_t> delete_ids;
            for (int index = 0; index < m_model->objects.size(); index++) {
                ModelObject* obj =  m_model->objects[index];
                if (obj->volumes.size() == 0) {
                    //remove this model objects
                    delete_ids.push_back(index);
                }
            }

            for (int index = delete_ids.size() - 1; index >= 0; index--)
                m_model->delete_object(delete_ids[index]);
        }

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("import 3mf IMPORT_STAGE_FINISH\n");
        if (proFn) {
            proFn(IMPORT_STAGE_FINISH, 0, 1, cb_cancel);
            if (cb_cancel)
                return false;
        }

        return true;
    }

    bool _BBS_3MF_Importer::_is_svg_shape_file(const std::string &name) const { 
        return boost::starts_with(name, MODEL_FOLDER) && boost::ends_with(name, ".svg");
    }

    bool _BBS_3MF_Importer::_extract_from_archive(mz_zip_archive& archive, std::string const & path, std::function<bool (mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)> extract, bool restore)
    {
        mz_uint num_entries = mz_zip_reader_get_num_files(&archive);
        mz_zip_archive_file_stat stat;
        std::string path2 = path;
        if (path2.front() == '/') path2 = path2.substr(1);
        // try utf8 encoding
        int index = mz_zip_reader_locate_file(&archive, path2.c_str(), nullptr, 0);
        if (index < 0) {
            // try native encoding
            std::string native_path = encode_path(path2.c_str());
            index = mz_zip_reader_locate_file(&archive, native_path.c_str(), nullptr, 0);
        }
        if (index < 0) {
            // try unicode path extra
            std::string extra(1024, 0);
            for (mz_uint i = 0; i < archive.m_total_files; ++i) {
                size_t n = mz_zip_reader_get_extra(&archive, i, extra.data(), extra.size());
                if (n > 0 && path2 == ZipUnicodePathExtraField::decode(extra.substr(0, n))) {
                    index = i;
                    break;
                }
            }
        }
        if (index < 0 || !mz_zip_reader_file_stat(&archive, index, &stat)) {
            if (restore) {
                std::vector<std::string> paths = {m_backup_path + path};
                if (!m_origin_file.empty()) paths.push_back(m_origin_file);
                for (auto & path2 : paths) {
                    bool result = false;
                    if (boost::filesystem::exists(path2)) {
                        mz_zip_archive archive;
                        mz_zip_zero_struct(&archive);
                        if (open_zip_reader(&archive, path2)) {
                            result = _extract_from_archive(archive, path, extract);
                            close_zip_reader(&archive);
                        }
                    }
                    if (result) return result;
                }
            }
            char error_buf[1024];
            ::snprintf(error_buf, 1024, "File %s not found from archive", path.c_str());
            add_error(error_buf);
            return false;
        }
        try
        {
            if (!extract(archive, stat)) {
                return false;
            }
        }
        catch (const std::exception& e)
        {
            // ensure the zip archive is closed and rethrow the exception
            add_error(e.what());
            return false;
        }
        return true;
    }

    bool _BBS_3MF_Importer::_extract_xml_from_archive(mz_zip_archive& archive, const std::string & path, XML_StartElementHandler start_handler, XML_EndElementHandler end_handler)
    {
        return _extract_from_archive(archive, path, [this, start_handler, end_handler](mz_zip_archive& archive, const mz_zip_archive_file_stat& stat) {
            return _extract_xml_from_archive(archive, stat, start_handler, end_handler);
        });
    }

    bool _BBS_3MF_Importer::_extract_xml_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, XML_StartElementHandler start_handler, XML_EndElementHandler end_handler)
    {
        if (stat.m_uncomp_size == 0) {
            add_error("Found invalid size");
            return false;
        }

        _destroy_xml_parser();

        m_xml_parser = XML_ParserCreate(nullptr);
        if (m_xml_parser == nullptr) {
            add_error("Unable to create parser");
            return false;
        }

        XML_SetUserData(m_xml_parser, (void*)this);
        XML_SetElementHandler(m_xml_parser, start_handler, end_handler);
        XML_SetCharacterDataHandler(m_xml_parser, _BBS_3MF_Importer::_handle_xml_characters);
        XML_SetEntityDeclHandler(m_xml_parser, nullptr);
        XML_SetExternalEntityRefHandler(m_xml_parser, nullptr);

        void* parser_buffer = XML_GetBuffer(m_xml_parser, (int)stat.m_uncomp_size);
        if (parser_buffer == nullptr) {
            add_error("Unable to create buffer");
            return false;
        }

        mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, parser_buffer, (size_t)stat.m_uncomp_size, 0);
        if (res == 0) {
            add_error("Error while reading config data to buffer");
            return false;
        }

        if (!XML_ParseBuffer(m_xml_parser, (int)stat.m_uncomp_size, 1)) {
            char error_buf[1024];
            ::snprintf(error_buf, 1024, "Error (%s) while parsing xml file at line %d", XML_ErrorString(XML_GetErrorCode(m_xml_parser)), (int)XML_GetCurrentLineNumber(m_xml_parser));
            add_error(error_buf);
            return false;
        }

        return true;
    }

    bool _BBS_3MF_Importer::_extract_model_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
    {
        if (stat.m_uncomp_size == 0) {
            add_error("Found invalid size");
            return false;
        }

        _destroy_xml_parser();

        m_xml_parser = XML_ParserCreate(nullptr);
        if (m_xml_parser == nullptr) {
            add_error("Unable to create parser");
            return false;
        }

        XML_SetUserData(m_xml_parser, (void*)this);
        XML_SetElementHandler(m_xml_parser, _BBS_3MF_Importer::_handle_start_model_xml_element, _BBS_3MF_Importer::_handle_end_model_xml_element);
        XML_SetCharacterDataHandler(m_xml_parser, _BBS_3MF_Importer::_handle_xml_characters);
        XML_SetEntityDeclHandler(m_xml_parser, nullptr);
        XML_SetExternalEntityRefHandler(m_xml_parser, nullptr);

        struct CallbackData
        {
            XML_Parser& parser;
            _BBS_3MF_Importer& importer;
            const mz_zip_archive_file_stat& stat;

            CallbackData(XML_Parser& parser, _BBS_3MF_Importer& importer, const mz_zip_archive_file_stat& stat) : parser(parser), importer(importer), stat(stat) {}
        };

        CallbackData data(m_xml_parser, *this, stat);

        mz_bool res = 0;

        try
        {
            mz_file_write_func callback = [](void* pOpaque, mz_uint64 file_ofs, const void* pBuf, size_t n)->size_t {
                CallbackData* data = (CallbackData*)pOpaque;
                if (!XML_Parse(data->parser, (const char*)pBuf, (int)n, (file_ofs + n == data->stat.m_uncomp_size) ? 1 : 0) || data->importer.parse_error()) {
                    char error_buf[1024];
                    ::snprintf(error_buf, 1024, "Error (%s) while parsing '%s' at line %d", data->importer.parse_error_message(), data->stat.m_filename, (int)XML_GetCurrentLineNumber(data->parser));
                    throw Slic3r::FileIOError(error_buf);
                }
                return n;
            };
            void* opaque = &data;
            res = mz_zip_reader_extract_to_callback(&archive, stat.m_file_index, callback, opaque, 0);
        }
        catch (const version_error& e)
        {
            // rethrow the exception
            throw Slic3r::FileIOError(e.what());
        }
        catch (std::exception& e)
        {
            add_error(e.what());
            return false;
        }

        if (res == 0) {
            add_error("Error while extracting model data from zip archive");
            return false;
        }

        return true;
    }

    void _BBS_3MF_Importer::_extract_cut_information_from_archive(mz_zip_archive &archive, const mz_zip_archive_file_stat &stat, ConfigSubstitutionContext &config_substitutions)
    {
        if (stat.m_uncomp_size > 0) {
            std::string buffer((size_t)stat.m_uncomp_size, 0);
            mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, (void *) buffer.data(), (size_t) stat.m_uncomp_size, 0);
            if (res == 0) {
                add_error("Error while reading cut information data to buffer");
                return;
            }

            std::istringstream iss(buffer); // wrap returned xml to istringstream
            pt::ptree objects_tree;
            pt::read_xml(iss, objects_tree);

            for (const auto& object : objects_tree.get_child("objects")) {
                pt::ptree object_tree = object.second;
                int obj_idx = object_tree.get<int>("<xmlattr>.id", -1);
                if (obj_idx <= 0) {
                    add_error("Found invalid object id");
                    continue;
                }

                IdToCutObjectInfoMap::iterator object_item = m_cut_object_infos.find(obj_idx);
                if (object_item != m_cut_object_infos.end()) {
                    add_error("Found duplicated cut_object_id");
                    continue;
                }

                CutObjectBase cut_id;
                std::vector<CutObjectInfo::Connector>  connectors;

                for (const auto& obj_cut_info : object_tree) {
                    if (obj_cut_info.first == "cut_id") {
                        pt::ptree cut_id_tree = obj_cut_info.second;
                        cut_id = CutObjectBase(ObjectID( cut_id_tree.get<size_t>("<xmlattr>.id")),
                                                         cut_id_tree.get<size_t>("<xmlattr>.check_sum"),
                                                         cut_id_tree.get<size_t>("<xmlattr>.connectors_cnt"));
                    }
                    if (obj_cut_info.first == "connectors") {
                        pt::ptree cut_connectors_tree = obj_cut_info.second;
                        for (const auto& cut_connector : cut_connectors_tree) {
                            if (cut_connector.first != "connector")
                                continue;
                            pt::ptree connector_tree = cut_connector.second;
                            CutObjectInfo::Connector connector = {connector_tree.get<int>("<xmlattr>.volume_id"),
                                                                  connector_tree.get<int>("<xmlattr>.type"),
                                                                  connector_tree.get<float>("<xmlattr>.r_tolerance"),
                                                                  connector_tree.get<float>("<xmlattr>.h_tolerance")};
                            connectors.emplace_back(connector);
                        }
                    }
                }

                CutObjectInfo cut_info {cut_id, connectors};
                m_cut_object_infos.insert({ obj_idx, cut_info });
            }
        }
    }

    void _BBS_3MF_Importer::_extract_print_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, DynamicPrintConfig& config, ConfigSubstitutionContext& config_substitutions, const std::string& archive_filename)
    {
        if (stat.m_uncomp_size > 0) {
            std::string buffer((size_t)stat.m_uncomp_size, 0);
            mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
            if (res == 0) {
                add_error("Error while reading config data to buffer");
                return;
            }
            ConfigBase::load_from_gcode_string_legacy(config, buffer.data(), config_substitutions);
        }
    }

    //BBS: extract project config from json files
    void _BBS_3MF_Importer::_extract_project_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, DynamicPrintConfig& config, ConfigSubstitutionContext& config_substitutions, Model& model)
    {
        if (stat.m_uncomp_size > 0) {
            const std::string& temp_path = model.get_backup_path();

            std::string dest_file = temp_path + std::string("/") + "_temp_3.config";;
            std::string dest_zip_file = encode_path(dest_file.c_str());
            mz_bool res = mz_zip_reader_extract_to_file(&archive, stat.m_file_index, dest_zip_file.c_str(), 0);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", extract  %1% from 3mf %2%, ret %3%\n") % dest_file % stat.m_filename % res;
            if (res == 0) {
                add_error("Error while extract project config file to file");
                return;
            }
            std::map<std::string, std::string> key_values;
            std::string reason;
            int ret = config.load_from_json(dest_file, config_substitutions, true, key_values, reason);
            if (ret) {
                add_error("Error load config from json:"+reason);
                return;
            }
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", load project config file successfully from %1%\n") %dest_file;
        }
    }

    //BBS: extract project embedded presets
    void _BBS_3MF_Importer::_extract_project_embedded_presets_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, std::vector<Preset*>&project_presets, Model& model, Preset::Type type, bool use_json)
    {
        if (stat.m_uncomp_size > 0) {
            /*std::string src_file = decode_path(stat.m_filename);
            std::size_t found = src_file.find(METADATA_DIR);
            if (found != std::string::npos)
                src_file = src_file.substr(found + METADATA_STR_LEN);
            else
                return;*/
            std::string dest_file = m_backup_path + std::string("/") + "_temp_2.config";;
            std::string dest_zip_file = encode_path(dest_file.c_str());
            mz_bool res = mz_zip_reader_extract_to_file(&archive, stat.m_file_index, dest_zip_file.c_str(), 0);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", extract  %1% from 3mf %2%, ret %3%\n") % dest_file % stat.m_filename % res;
            if (res == 0) {
                add_error("Error while extract auxiliary file to file");
                return;
            }
            //load presets
            DynamicPrintConfig config;
            //ConfigSubstitutions config_substitutions = config.load_from_ini(dest_file, Enable);
            std::map<std::string, std::string> key_values;
            std::string reason;
            ConfigSubstitutions config_substitutions = use_json? config.load_from_json(dest_file, Enable, key_values, reason) : config.load_from_ini(dest_file, Enable);
            if (!reason.empty()) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", load project embedded config from  %1% failed\n") % dest_file;
                //skip this file
                return;
            }
            ConfigOptionString* print_name;
            ConfigOptionStrings* filament_names;
            std::string preset_name;
            if (type == Preset::TYPE_PRINT) {
                print_name = dynamic_cast < ConfigOptionString* > (config.option("print_settings_id"));
                if (!print_name) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", can not found print_settings_id from  %1%\n") % dest_file;
                    //skip this file
                    return;
                }
                preset_name = print_name->value;
            }
            else if (type == Preset::TYPE_FILAMENT) {
                filament_names = dynamic_cast < ConfigOptionStrings* > (config.option("filament_settings_id"));
                if (!filament_names) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", can not found filament_settings_id from  %1%\n") % dest_file;
                    //skip this file
                    return;
                }
                preset_name = filament_names->values[0];
            }
            else if (type == Preset::TYPE_PRINTER) {
                print_name = dynamic_cast < ConfigOptionString* > (config.option("printer_settings_id"));
                if (!print_name) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", can not found printer_settings_id from  %1%\n") % dest_file;
                    //skip this file
                    return;
                }
                preset_name = print_name->value;
            }
            else {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(", invalid type  %1% from file %2%\n")% Preset::get_type_string(type) % dest_file;
                //skip this file
                return;
            }

            Preset *preset = new Preset(type, preset_name, false);
            preset->file = dest_file;
            preset->config = std::move(config);
            preset->loaded = true;
            preset->is_project_embedded = true;
            preset->is_external = true;
            preset->is_dirty = false;

            std::string version_str = key_values[BBL_JSON_KEY_VERSION];
            boost::optional<Semver> version = Semver::parse(version_str);
            if (version) {
                preset->version = *version;
            }
            else
                preset->version = this->m_bambuslicer_generator_version?*this->m_bambuslicer_generator_version: Semver();
            /*for (int i = 0; i < config_substitutions.size(); i++)
            {
                //ConfigSubstitution config_substitution;
                //config_substitution.opt_def   = optdef;
                //config_substitution.old_value = value;
                //config_substitution.new_value = ConfigOptionUniquePtr(opt->clone());
                preset->loading_substitutions.emplace_back(std::move(config_substitutions[i]));
            }*/
            if (!config_substitutions.empty()) {
                preset->loading_substitutions = new ConfigSubstitutions();
                *(preset->loading_substitutions) = std::move(config_substitutions);
            }

            project_presets.push_back(preset);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", create one project embedded preset: %1% from %2%, type %3%\n") % preset_name % dest_file %Preset::get_type_string(type);
        }
    }

    void _BBS_3MF_Importer::_extract_auxiliary_file_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, Model& model)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", stat.m_uncomp_size is %1%")%stat.m_uncomp_size;
        if (stat.m_uncomp_size > 0) {
            std::string dest_file;
            if (stat.m_is_utf8) {
                dest_file = stat.m_filename;
            } else {
                std::string extra(1024, 0);
                size_t n = mz_zip_reader_get_extra(&archive, stat.m_file_index, extra.data(), extra.size());
                dest_file = ZipUnicodePathExtraField::decode(extra.substr(0, n), stat.m_filename);
            }
            std::string temp_path = model.get_auxiliary_file_temp_path();
            //aux directory from model
            boost::filesystem::path dir = boost::filesystem::path(temp_path);
            std::size_t found = dest_file.find(AUXILIARY_DIR);
            if (found != std::string::npos)
                dest_file = dest_file.substr(found + AUXILIARY_STR_LEN);
            else
                return;

            if (dest_file.find('/') != std::string::npos) {
                boost::filesystem::path src_path = boost::filesystem::path(dest_file);
                boost::filesystem::path parent_path = src_path.parent_path();
                std::string temp_path = dir.string() + std::string("/") + parent_path.string();
                boost::filesystem::path parent_full_path =  boost::filesystem::path(temp_path);
                if (!boost::filesystem::exists(parent_full_path))
                    boost::filesystem::create_directories(parent_full_path);
            }
            dest_file = dir.string() + std::string("/") + dest_file;
            std::string dest_zip_file = encode_path(dest_file.c_str());
            mz_bool res = mz_zip_reader_extract_to_file(&archive, stat.m_file_index, dest_zip_file.c_str(), 0);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", extract  %1% from 3mf %2%, ret %3%\n") % dest_file % stat.m_filename % res;
            if (res == 0) {
                add_error("Error while extract auxiliary file to file");
                return;
            }
        }
    }

    void _BBS_3MF_Importer::_extract_file_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
    {
        if (stat.m_uncomp_size > 0) {
            std::string src_file = decode_path(stat.m_filename);
            // BBS: use backup path
            //aux directory from model
            boost::filesystem::path dest_path = boost::filesystem::path(m_backup_path + "/" + src_file);
            std::string dest_zip_file = encode_path(dest_path.string().c_str());
            mz_bool res = mz_zip_reader_extract_to_file(&archive, stat.m_file_index, dest_zip_file.c_str(), 0);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", extract  %1% from 3mf %2%, ret %3%\n") % dest_path % stat.m_filename % res;
            if (res == 0) {
                add_error("Error while extract file to temp directory");
                return;
            }
        }
        return;
    }

    void _BBS_3MF_Importer::_extract_layer_heights_profile_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
    {
        if (stat.m_uncomp_size > 0) {
            std::string buffer((size_t)stat.m_uncomp_size, 0);
            mz_bool res = mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
            if (res == 0) {
                add_error("Error while reading layer heights profile data to buffer");
                return;
            }

            if (buffer.back() == '\n')
                buffer.pop_back();

            std::vector<std::string> objects;
            boost::split(objects, buffer, boost::is_any_of("\n"), boost::token_compress_off);

            for (const std::string& object : objects)             {
                std::vector<std::string> object_data;
                boost::split(object_data, object, boost::is_any_of("|"), boost::token_compress_off);
                if (object_data.size() != 2) {
                    add_error("Error while reading object data");
                    continue;
                }

                std::vector<std::string> object_data_id;
                boost::split(object_data_id, object_data[0], boost::is_any_of("="), boost::token_compress_off);
                if (object_data_id.size() != 2) {
                    add_error("Error while reading object id");
                    continue;
                }

                int object_id = std::atoi(object_data_id[1].c_str());
                if (object_id == 0) {
                    add_error("Found invalid object id");
                    continue;
                }

                IdToLayerHeightsProfileMap::iterator object_item = m_layer_heights_profiles.find(object_id);
                if (object_item != m_layer_heights_profiles.end()) {
                    add_error("Found duplicated layer heights profile");
                    continue;
                }

                std::vector<std::string> object_data_profile;
                boost::split(object_data_profile, object_data[1], boost::is_any_of(";"), boost::token_compress_off);
                if (object_data_profile.size() <= 4 || object_data_profile.size() % 2 != 0) {
                    add_error("Found invalid layer heights profile");
                    continue;
                }

                std::vector<coordf_t> profile;
                profile.reserve(object_data_profile.size());

                for (const std::string& value : object_data_profile) {
                    profile.push_back((coordf_t)std::atof(value.c_str()));
                }

                m_layer_heights_profiles.insert({ object_id, profile });
            }
        }
    }
    
    void _BBS_3MF_Importer::_extract_layer_config_ranges_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, ConfigSubstitutionContext& config_substitutions)
    {
        if (stat.m_uncomp_size > 0) {
            std::string buffer((size_t)stat.m_uncomp_size, 0);
            mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
            if (res == 0) {
                add_error("Error while reading layer config ranges data to buffer");
                return;
            }

            std::istringstream iss(buffer); // wrap returned xml to istringstream
            pt::ptree objects_tree;
            pt::read_xml(iss, objects_tree);

            for (const auto& object : objects_tree.get_child("objects")) {
                pt::ptree object_tree = object.second;
                int obj_idx = object_tree.get<int>("<xmlattr>.id", -1);
                if (obj_idx <= 0) {
                    add_error("Found invalid object id");
                    continue;
                }

                IdToLayerConfigRangesMap::iterator object_item = m_layer_config_ranges.find(obj_idx);
                if (object_item != m_layer_config_ranges.end()) {
                    add_error("Found duplicated layer config range");
                    continue;
                }

                t_layer_config_ranges config_ranges;

                for (const auto& range : object_tree) {
                    if (range.first != "range")
                        continue;
                    pt::ptree range_tree = range.second;
                    double min_z = range_tree.get<double>("<xmlattr>.min_z");
                    double max_z = range_tree.get<double>("<xmlattr>.max_z");

                    // get Z range information
                    DynamicPrintConfig config;

                    for (const auto& option : range_tree) {
                        if (option.first != "option")
                            continue;
                        std::string opt_key = option.second.get<std::string>("<xmlattr>.opt_key");
                        std::string value = option.second.data();

                        config.set_deserialize(opt_key, value, config_substitutions);
                    }

                    config_ranges[{ min_z, max_z }].assign_config(std::move(config));
                }

                if (!config_ranges.empty())
                    m_layer_config_ranges.insert({ obj_idx, std::move(config_ranges) });
            }
        }
    }

    void _BBS_3MF_Importer::_extract_brim_ear_points_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
    {
        if (stat.m_uncomp_size > 0) {
            std::string buffer((size_t)stat.m_uncomp_size, 0);
            mz_bool res = mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
            if (res == 0) {
                add_error("Error while reading brim ear points data to buffer");
                return;
            }

            if (buffer.back() == '\n')
                buffer.pop_back();

            std::vector<std::string> objects;
            boost::split(objects, buffer, boost::is_any_of("\n"), boost::token_compress_off);

            // Info on format versioning - see bbs_3mf.hpp
            int version = 0;
            std::string key("brim_points_format_version=");
            if (!objects.empty() && objects[0].find(key) != std::string::npos) {
                objects[0].erase(objects[0].begin(), objects[0].begin() + long(key.size())); // removes the string
                version = std::stoi(objects[0]);
                objects.erase(objects.begin()); // pop the header
            }

            for (const std::string& object : objects) {
                std::vector<std::string> object_data;
                boost::split(object_data, object, boost::is_any_of("|"), boost::token_compress_off);

                if (object_data.size() != 2) {
                    add_error("Error while reading object data");
                    continue;
                }

                std::vector<std::string> object_data_id;
                boost::split(object_data_id, object_data[0], boost::is_any_of("="), boost::token_compress_off);
                if (object_data_id.size() != 2) {
                    add_error("Error while reading object id");
                    continue;
                }

                int object_id = std::atoi(object_data_id[1].c_str());
                if (object_id == 0) {
                    add_error("Found invalid object id");
                    continue;
                }

                IdToBrimPointsMap::iterator object_item = m_brim_ear_points.find(object_id);
                if (object_item != m_brim_ear_points.end()) {
                    add_error("Found duplicated brim ear points");
                    continue;
                }

                std::vector<std::string> object_data_points;
                boost::split(object_data_points, object_data[1], boost::is_any_of(" "), boost::token_compress_off);

                std::vector<BrimPoint> brim_ear_points;
                if (version == 0) {
                    for (unsigned int i=0; i<object_data_points.size(); i+=4)
                    brim_ear_points.emplace_back(float(std::atof(object_data_points[i+0].c_str())),
                                                    float(std::atof(object_data_points[i+1].c_str())),
                                                    float(std::atof(object_data_points[i+2].c_str())),
                                                    float(std::atof(object_data_points[i+3].c_str())));
                }

                if (!brim_ear_points.empty())
                    m_brim_ear_points.insert({ object_id, brim_ear_points });
            }
        }
    }
    /*
    void _BBS_3MF_Importer::_extract_sla_support_points_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
    {
        if (stat.m_uncomp_size > 0) {
            std::string buffer((size_t)stat.m_uncomp_size, 0);
            mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
            if (res == 0) {
                add_error("Error while reading sla support points data to buffer");
                return;
            }

            if (buffer.back() == '\n')
                buffer.pop_back();

            std::vector<std::string> objects;
            boost::split(objects, buffer, boost::is_any_of("\n"), boost::token_compress_off);

            // Info on format versioning - see 3mf.hpp
            int version = 0;
            std::string key("support_points_format_version=");
            if (!objects.empty() && objects[0].find(key) != std::string::npos) {
                objects[0].erase(objects[0].begin(), objects[0].begin() + long(key.size())); // removes the string
                version = std::stoi(objects[0]);
                objects.erase(objects.begin()); // pop the header
            }

            for (const std::string& object : objects) {
                std::vector<std::string> object_data;
                boost::split(object_data, object, boost::is_any_of("|"), boost::token_compress_off);

                if (object_data.size() != 2) {
                    add_error("Error while reading object data");
                    continue;
                }

                std::vector<std::string> object_data_id;
                boost::split(object_data_id, object_data[0], boost::is_any_of("="), boost::token_compress_off);
                if (object_data_id.size() != 2) {
                    add_error("Error while reading object id");
                    continue;
                }

                int object_id = std::atoi(object_data_id[1].c_str());
                if (object_id == 0) {
                    add_error("Found invalid object id");
                    continue;
                }

                IdToSlaSupportPointsMap::iterator object_item = m_sla_support_points.find(object_id);
                if (object_item != m_sla_support_points.end()) {
                    add_error("Found duplicated SLA support points");
                    continue;
                }

                std::vector<std::string> object_data_points;
                boost::split(object_data_points, object_data[1], boost::is_any_of(" "), boost::token_compress_off);

                std::vector<sla::SupportPoint> sla_support_points;

                if (version == 0) {
                    for (unsigned int i=0; i<object_data_points.size(); i+=3)
                    sla_support_points.emplace_back(float(std::atof(object_data_points[i+0].c_str())),
                                                    float(std::atof(object_data_points[i+1].c_str())),
													float(std::atof(object_data_points[i+2].c_str())),
                                                    0.4f,
                                                    false);
                }
                if (version == 1) {
                    for (unsigned int i=0; i<object_data_points.size(); i+=5)
                    sla_support_points.emplace_back(float(std::atof(object_data_points[i+0].c_str())),
                                                    float(std::atof(object_data_points[i+1].c_str())),
                                                    float(std::atof(object_data_points[i+2].c_str())),
                                                    float(std::atof(object_data_points[i+3].c_str())),
													//FIXME storing boolean as 0 / 1 and importing it as float.
                                                    std::abs(std::atof(object_data_points[i+4].c_str()) - 1.) < EPSILON);
                }

                if (!sla_support_points.empty())
                    m_sla_support_points.insert({ object_id, sla_support_points });
            }
        }
    }

    void _BBS_3MF_Importer::_extract_sla_drain_holes_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
    {
        if (stat.m_uncomp_size > 0) {
            std::string buffer(size_t(stat.m_uncomp_size), 0);
            mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
            if (res == 0) {
                add_error("Error while reading sla support points data to buffer");
                return;
            }

            if (buffer.back() == '\n')
                buffer.pop_back();

            std::vector<std::string> objects;
            boost::split(objects, buffer, boost::is_any_of("\n"), boost::token_compress_off);

            // Info on format versioning - see 3mf.hpp
            int version = 0;
            std::string key("drain_holes_format_version=");
            if (!objects.empty() && objects[0].find(key) != std::string::npos) {
                objects[0].erase(objects[0].begin(), objects[0].begin() + long(key.size())); // removes the string
                version = std::stoi(objects[0]);
                objects.erase(objects.begin()); // pop the header
            }

            for (const std::string& object : objects) {
                std::vector<std::string> object_data;
                boost::split(object_data, object, boost::is_any_of("|"), boost::token_compress_off);

                if (object_data.size() != 2) {
                    add_error("Error while reading object data");
                    continue;
                }

                std::vector<std::string> object_data_id;
                boost::split(object_data_id, object_data[0], boost::is_any_of("="), boost::token_compress_off);
                if (object_data_id.size() != 2) {
                    add_error("Error while reading object id");
                    continue;
                }

                int object_id = std::atoi(object_data_id[1].c_str());
                if (object_id == 0) {
                    add_error("Found invalid object id");
                    continue;
                }

                IdToSlaDrainHolesMap::iterator object_item = m_sla_drain_holes.find(object_id);
                if (object_item != m_sla_drain_holes.end()) {
                    add_error("Found duplicated SLA drain holes");
                    continue;
                }

                std::vector<std::string> object_data_points;
                boost::split(object_data_points, object_data[1], boost::is_any_of(" "), boost::token_compress_off);

                sla::DrainHoles sla_drain_holes;

                if (version == 1) {
                    for (unsigned int i=0; i<object_data_points.size(); i+=8)
                        sla_drain_holes.emplace_back(Vec3f{float(std::atof(object_data_points[i+0].c_str())),
                                                      float(std::atof(object_data_points[i+1].c_str())),
                                                      float(std::atof(object_data_points[i+2].c_str()))},
                                                     Vec3f{float(std::atof(object_data_points[i+3].c_str())),
                                                      float(std::atof(object_data_points[i+4].c_str())),
                                                      float(std::atof(object_data_points[i+5].c_str()))},
                                                      float(std::atof(object_data_points[i+6].c_str())),
                                                      float(std::atof(object_data_points[i+7].c_str())));
                }

                // The holes are saved elevated above the mesh and deeper (bad idea indeed).
                // This is retained for compatibility.
                // Place the hole to the mesh and make it shallower to compensate.
                // The offset is 1 mm above the mesh.
                for (sla::DrainHole& hole : sla_drain_holes) {
                    hole.pos += hole.normal.normalized();
                    hole.height -= 1.f;
                }

                if (!sla_drain_holes.empty())
                    m_sla_drain_holes.insert({ object_id, sla_drain_holes });
            }
        }
    }*/

    void _BBS_3MF_Importer::_extract_embossed_svg_shape_file(const std::string &filename, mz_zip_archive &archive, const mz_zip_archive_file_stat &stat){
        assert(m_path_to_emboss_shape_files.find(filename) == m_path_to_emboss_shape_files.end());
        auto file = std::make_unique<std::string>(stat.m_uncomp_size, '\0');
        mz_bool res  = mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, (void *) file->data(), stat.m_uncomp_size, 0);
        if (res == 0) {
            add_error("Error while reading svg shape for emboss");
            return;
        }
        
        // store for case svg is loaded before volume
        m_path_to_emboss_shape_files[filename] = std::move(file);
        
        // find embossed volume, for case svg is loaded after volume
        for (const ModelObject* object : m_model->objects)
        for (ModelVolume *volume : object->volumes) {
            std::optional<EmbossShape> &es = volume->emboss_shape;
            if (!es.has_value())
                continue;
            std::optional<EmbossShape::SvgFile> &svg = es->svg_file;
            if (!svg.has_value())
                continue;
            if (filename.compare(svg->path_in_3mf) == 0)
                svg->file_data = m_path_to_emboss_shape_files[filename];
        }
    }

    void _BBS_3MF_Importer::_extract_custom_gcode_per_print_z_from_archive(::mz_zip_archive &archive, const mz_zip_archive_file_stat &stat)
    {
        //BBS: add plate tree related logic
        if (stat.m_uncomp_size > 0) {
            std::string buffer((size_t)stat.m_uncomp_size, 0);
            mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
            if (res == 0) {
                add_error("Error while reading custom Gcodes per height data to buffer");
                return;
            }

            std::istringstream iss(buffer); // wrap returned xml to istringstream
            pt::ptree main_tree;
            pt::read_xml(iss, main_tree);

            if (main_tree.front().first != "custom_gcodes_per_layer")
                return;

            auto extract_code = [this](int plate_id, pt::ptree code_tree) {
                for (const auto& code : code_tree) {
                    if (code.first == "mode") {
                        pt::ptree tree = code.second;
                        std::string mode = tree.get<std::string>("<xmlattr>.value");
                        m_model->plates_custom_gcodes[plate_id - 1].mode = mode == CustomGCode::SingleExtruderMode ? CustomGCode::Mode::SingleExtruder :
                            mode == CustomGCode::MultiAsSingleMode ? CustomGCode::Mode::MultiAsSingle :
                            CustomGCode::Mode::MultiExtruder;
                    }
                    if (code.first == "layer") {
                        pt::ptree tree = code.second;
                        double print_z = tree.get<double>("<xmlattr>.top_z");
                        int extruder = tree.get<int>("<xmlattr>.extruder");
                        std::string color = tree.get<std::string>("<xmlattr>.color");

                        CustomGCode::Type   type;
                        std::string         extra;
                        pt::ptree attr_tree = tree.find("<xmlattr>")->second;
                        if (attr_tree.find("type") == attr_tree.not_found()) {
                            // It means that data was saved in old version (2.2.0 and older) of PrusaSlicer
                            // read old data ...
                            std::string gcode = tree.get<std::string>("<xmlattr>.gcode");
                            // ... and interpret them to the new data
                            type = gcode == "M600" ? CustomGCode::ColorChange :
                                gcode == "M601" ? CustomGCode::PausePrint :
                                gcode == "tool_change" ? CustomGCode::ToolChange : CustomGCode::Custom;
                            extra = type == CustomGCode::PausePrint ? color :
                                type == CustomGCode::Custom ? gcode : "";
                        }
                        else {
                            type = static_cast<CustomGCode::Type>(tree.get<int>("<xmlattr>.type"));
                            extra = tree.get<std::string>("<xmlattr>.extra");
                        }
                        m_model->plates_custom_gcodes[plate_id - 1].gcodes.push_back(CustomGCode::Item{ print_z, type, extruder, color, extra });
                    }
                }
            };

            m_model->plates_custom_gcodes.clear();

            bool has_plate_info = false;
            for (const auto& element : main_tree.front().second) {
                if (element.first == "plate") {
                    has_plate_info = true;

                    int plate_id = -1;
                    pt::ptree code_tree = element.second;
                    for (const auto& code : code_tree) {
                        if (code.first == "plate_info") {
                            plate_id = code.second.get<int>("<xmlattr>.id");
                        }

                    }
                    if (plate_id == -1)
                        continue;

                    extract_code(plate_id, code_tree);
                }
            }

            if (!has_plate_info) {
                int plate_id = 1;
                pt::ptree code_tree = main_tree.front().second;
                extract_code(plate_id, code_tree);
            }
        }
    }

    void _BBS_3MF_Importer::_handle_start_model_xml_element(const char* name, const char** attributes)
    {
        if (m_xml_parser == nullptr)
            return;

        bool res = true;
        unsigned int num_attributes = (unsigned int)XML_GetSpecifiedAttributeCount(m_xml_parser);

        if (::strcmp(MODEL_TAG, name) == 0)
            res = _handle_start_model(attributes, num_attributes);
        else if (::strcmp(RESOURCES_TAG, name) == 0)
            res = _handle_start_resources(attributes, num_attributes);
        else if (::strcmp(OBJECT_TAG, name) == 0)
            res = _handle_start_object(attributes, num_attributes);
        else if (::strcmp(COLOR_GROUP_TAG, name) == 0)
            res = _handle_start_color_group(attributes, num_attributes);
        else if (::strcmp(COLOR_TAG, name) == 0)
            res = _handle_start_color(attributes, num_attributes);
        else if (::strcmp(MESH_TAG, name) == 0)
            res = _handle_start_mesh(attributes, num_attributes);
        else if (::strcmp(VERTICES_TAG, name) == 0)
            res = _handle_start_vertices(attributes, num_attributes);
        else if (::strcmp(VERTEX_TAG, name) == 0)
            res = _handle_start_vertex(attributes, num_attributes);
        else if (::strcmp(TRIANGLES_TAG, name) == 0)
            res = _handle_start_triangles(attributes, num_attributes);
        else if (::strcmp(TRIANGLE_TAG, name) == 0)
            res = _handle_start_triangle(attributes, num_attributes);
        else if (::strcmp(COMPONENTS_TAG, name) == 0)
            res = _handle_start_components(attributes, num_attributes);
        else if (::strcmp(COMPONENT_TAG, name) == 0)
            res = _handle_start_component(attributes, num_attributes);
        else if (::strcmp(BUILD_TAG, name) == 0)
            res = _handle_start_build(attributes, num_attributes);
        else if (::strcmp(ITEM_TAG, name) == 0)
            res = _handle_start_item(attributes, num_attributes);
        else if (::strcmp(METADATA_TAG, name) == 0)
            res = _handle_start_metadata(attributes, num_attributes);

        if (!res)
            _stop_xml_parser();
    }

    void _BBS_3MF_Importer::_handle_end_model_xml_element(const char* name)
    {
        if (m_xml_parser == nullptr)
            return;

        bool res = true;

        if (::strcmp(MODEL_TAG, name) == 0)
            res = _handle_end_model();
        else if (::strcmp(RESOURCES_TAG, name) == 0)
            res = _handle_end_resources();
        else if (::strcmp(OBJECT_TAG, name) == 0)
            res = _handle_end_object();
        else if (::strcmp(COLOR_GROUP_TAG, name) == 0)
            res = _handle_end_color_group();
        else if (::strcmp(COLOR_TAG, name) == 0)
            res = _handle_end_color();
        else if (::strcmp(MESH_TAG, name) == 0)
            res = _handle_end_mesh();
        else if (::strcmp(VERTICES_TAG, name) == 0)
            res = _handle_end_vertices();
        else if (::strcmp(VERTEX_TAG, name) == 0)
            res = _handle_end_vertex();
        else if (::strcmp(TRIANGLES_TAG, name) == 0)
            res = _handle_end_triangles();
        else if (::strcmp(TRIANGLE_TAG, name) == 0)
            res = _handle_end_triangle();
        else if (::strcmp(COMPONENTS_TAG, name) == 0)
            res = _handle_end_components();
        else if (::strcmp(COMPONENT_TAG, name) == 0)
            res = _handle_end_component();
        else if (::strcmp(BUILD_TAG, name) == 0)
            res = _handle_end_build();
        else if (::strcmp(ITEM_TAG, name) == 0)
            res = _handle_end_item();
        else if (::strcmp(METADATA_TAG, name) == 0)
            res = _handle_end_metadata();

        if (!res)
            _stop_xml_parser();
    }

    void _BBS_3MF_Importer::_handle_xml_characters(const XML_Char* s, int len)
    {
        m_curr_characters.append(s, len);
    }

    void _BBS_3MF_Importer::_handle_start_config_xml_element(const char* name, const char** attributes)
    {
        if (m_xml_parser == nullptr)
            return;

        bool res = true;
        unsigned int num_attributes = (unsigned int)XML_GetSpecifiedAttributeCount(m_xml_parser);

        if (::strcmp(CONFIG_TAG, name) == 0)
            res = _handle_start_config(attributes, num_attributes);
        else if (::strcmp(OBJECT_TAG, name) == 0)
            res = _handle_start_config_object(attributes, num_attributes);
        else if (::strcmp(VOLUME_TAG, name) == 0)
            res = _handle_start_config_volume(attributes, num_attributes);
        else if (::strcmp(PART_TAG, name) == 0)
            res = _handle_start_config_volume(attributes, num_attributes);
        else if (::strcmp(MESH_STAT_TAG, name) == 0)
            res = _handle_start_config_volume_mesh(attributes, num_attributes);
        else if (::strcmp(METADATA_TAG, name) == 0)
            res = _handle_start_config_metadata(attributes, num_attributes);
        else if (::strcmp(SHAPE_TAG, name) == 0)
            res = _handle_start_shape_configuration(attributes, num_attributes);
        else if (::strcmp(TEXT_TAG, name) == 0)
            res = _handle_start_text_configuration(attributes, num_attributes);
        else if (::strcmp(PLATE_TAG, name) == 0)
            res = _handle_start_config_plater(attributes, num_attributes);
        else if (::strcmp(INSTANCE_TAG, name) == 0)
            res = _handle_start_config_plater_instance(attributes, num_attributes);
        else if (::strcmp(FILAMENT_TAG, name) == 0)
            res = _handle_start_config_filament(attributes, num_attributes);
        else if (::strcmp(SLICE_WARNING_TAG, name) == 0)
            res = _handle_start_config_warning(attributes, num_attributes);
        else if (::strcmp(ASSEMBLE_TAG, name) == 0)
            res = _handle_start_assemble(attributes, num_attributes);
        else if (::strcmp(ASSEMBLE_ITEM_TAG, name) == 0)
            res = _handle_start_assemble_item(attributes, num_attributes);
        else if (::strcmp(TEXT_INFO_TAG, name) == 0)
            res = _handle_start_text_info_item(attributes, num_attributes);

        if (!res)
            _stop_xml_parser();
    }

    void _BBS_3MF_Importer::_handle_end_config_xml_element(const char* name)
    {
        if (m_xml_parser == nullptr)
            return;

        bool res = true;

        if (::strcmp(CONFIG_TAG, name) == 0)
            res = _handle_end_config();
        else if (::strcmp(OBJECT_TAG, name) == 0)
            res = _handle_end_config_object();
        else if (::strcmp(VOLUME_TAG, name) == 0)
            res = _handle_end_config_volume();
        else if (::strcmp(PART_TAG, name) == 0)
            res = _handle_end_config_volume();
        else if (::strcmp(MESH_STAT_TAG, name) == 0)
            res = _handle_end_config_volume_mesh();
        else if (::strcmp(METADATA_TAG, name) == 0)
            res = _handle_end_config_metadata();
        else if (::strcmp(PLATE_TAG, name) == 0)
            res = _handle_end_config_plater();
        else if (::strcmp(FILAMENT_TAG, name) == 0)
            res = _handle_end_config_filament();
        else if (::strcmp(INSTANCE_TAG, name) == 0)
            res = _handle_end_config_plater_instance();
        else if (::strcmp(ASSEMBLE_TAG, name) == 0)
            res = _handle_end_assemble();
        else if (::strcmp(ASSEMBLE_ITEM_TAG, name) == 0)
            res = _handle_end_assemble_item();

        if (!res)
            _stop_xml_parser();
    }

    bool _BBS_3MF_Importer::_handle_start_model(const char** attributes, unsigned int num_attributes)
    {
        m_unit_factor = bbs_get_unit_factor(bbs_get_attribute_value_string(attributes, num_attributes, UNIT_ATTR));

        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_model()
    {
        // BBS: Production Extension
        if (!m_sub_model_path.empty())
            return true;

        // deletes all non-built or non-instanced objects
        for (const IdToModelObjectMap::value_type& object : m_objects) {
            if (object.second >= int(m_model->objects.size())) {
                add_error("Unable to find object");
                return false;
            }
            ModelObject *model_object = m_model->objects[object.second];
            if (model_object != nullptr && model_object->instances.size() == 0)
                m_model->delete_object(model_object);
        }

        //construct the index maps
        for (const IdToCurrentObjectMap::value_type& object : m_current_objects) {
            m_index_paths.insert({ object.first.second, object.first.first});
        }

        if (!m_is_bbl_3mf) {
            // if the 3mf was not produced by OrcaSlicer and there is only one object,
            // set the object name to match the filename
            if (m_model->objects.size() == 1)
                m_model->objects.front()->name = m_name;
        }

        // applies instances' matrices
        for (Instance& instance : m_instances) {
            if (instance.instance != nullptr && instance.instance->get_object() != nullptr)
                // apply the transform to the instance
                _apply_transform(*instance.instance, instance.transform);
        }

        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_resources(const char** attributes, unsigned int num_attributes)
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_resources()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_object(const char** attributes, unsigned int num_attributes)
    {
        // reset current object data
        if (m_curr_object) {
            delete m_curr_object;
            m_curr_object = nullptr;
        }

        std::string object_type = bbs_get_attribute_value_string(attributes, num_attributes, TYPE_ATTR);

        if (bbs_is_valid_object_type(object_type)) {
            if (!m_curr_object) {
                m_curr_object = new CurrentObject();
                // create new object (it may be removed later if no instances are generated from it)
                /*m_curr_object->model_object_idx = (int)m_model->objects.size();
                m_curr_object.object = m_model->add_object();
                if (m_curr_object.object == nullptr) {
                    add_error("Unable to create object");
                    return false;
                }*/
            }

            m_curr_object->id = bbs_get_attribute_value_int(attributes, num_attributes, ID_ATTR);
            m_curr_object->name = bbs_get_attribute_value_string(attributes, num_attributes, NAME_ATTR);

            m_curr_object->uuid = bbs_get_attribute_value_string(attributes, num_attributes, PUUID_ATTR);
            if (m_curr_object->uuid.empty()) {
                m_curr_object->uuid = bbs_get_attribute_value_string(attributes, num_attributes, PUUID_LOWER_ATTR);
            }
            m_curr_object->pid = bbs_get_attribute_value_int(attributes, num_attributes, PID_ATTR);
        }

        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_object()
    {
        if (!m_load_model) {
            delete m_curr_object;
            m_curr_object = nullptr;
            return true;
        }
        if (!m_curr_object || (m_curr_object->id == -1)) {
            add_error("Found invalid object");
            return false;
        }
        else {
            if (m_is_bbl_3mf && boost::ends_with(m_curr_object->uuid, OBJECT_UUID_SUFFIX) && m_load_restore) {
                // Adjust backup object/volume id
                std::istringstream iss(m_curr_object->uuid);
                int backup_id;
                bool need_replace = false;
                if (iss >> std::hex >> backup_id) {
                    need_replace = (m_curr_object->id != backup_id);
                    m_curr_object->id = backup_id;
                }
                if (!m_curr_object->components.empty())
                {
                    Id first_id = m_curr_object->components.front().object_id;
                    first_id.second = 0;
                    IdToCurrentObjectMap::iterator current_object = m_current_objects.lower_bound(first_id);
                    IdToCurrentObjectMap new_map;
                    for (int index = 0; index < m_curr_object->components.size(); index++)
                    {
                        Component& component = m_curr_object->components[index];
                        Id new_id = component.object_id;
                        new_id.second = (index + 1) << 16 | backup_id;
                        if (current_object != m_current_objects.end()
                                && (new_id.first.empty() || new_id.first == current_object->first.first)) {
                            current_object->second.id   = new_id.second;
                            new_map.insert({new_id, std::move(current_object->second)});
                            current_object = m_current_objects.erase(current_object);
                        }
                        else {
                            add_error("can not find object for component, id=" + std::to_string(component.object_id.second));
                            delete m_curr_object;
                            m_curr_object = nullptr;
                            return false;
                        }
                        component.object_id.second = new_id.second;
                    }
                    for (auto & obj : new_map)
                        m_current_objects.insert({obj.first, std::move(obj.second)});
                }
            }

            Id id = std::make_pair(m_sub_model_path, m_curr_object->id);
            if (m_current_objects.find(id) == m_current_objects.end()) {
                m_current_objects.insert({ id, std::move(*m_curr_object) });
                delete m_curr_object;
                m_curr_object = nullptr;
            }
            else {
                add_error("Found object with duplicate id");
                delete m_curr_object;
                m_curr_object = nullptr;
                return false;
            }
        }

        /*if (m_curr_object.object != nullptr) {
            if (m_curr_object.id != -1) {
                if (m_curr_object.geometry.empty()) {
                    // no geometry defined
                    // remove the object from the model
                    m_model->delete_object(m_curr_object.object);

                    if (m_curr_object.components.empty()) {
                        // no components defined -> invalid object, delete it
                        IdToModelObjectMap::iterator object_item = m_objects.find(id);
                        if (object_item != m_objects.end())
                            m_objects.erase(object_item);

                        IdToAliasesMap::iterator alias_item = m_objects_aliases.find(id);
                        if (alias_item != m_objects_aliases.end())
                            m_objects_aliases.erase(alias_item);
                    }
                    else
                        // adds components to aliases
                        m_objects_aliases.insert({ id, m_curr_object.components });
                }
                else {
                    // geometry defined, store it for later use
                    m_geometries.insert({ id, std::move(m_curr_object.geometry) });

                    // stores the object for later use
                    if (m_objects.find(id) == m_objects.end()) {
                        m_objects.insert({ id, m_curr_object.model_object_idx });
                        m_objects_aliases.insert({ id, { 1, Component(m_curr_object.id) } }); // aliases itself
                    }
                    else {
                        add_error("Found object with duplicate id");
                        return false;
                    }
                }
            }
            else {
                //sub objects
            }
        }*/

        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_color_group(const char **attributes, unsigned int num_attributes)
    {
        m_current_color_group = bbs_get_attribute_value_int(attributes, num_attributes, ID_ATTR);
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_color_group()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_color(const char **attributes, unsigned int num_attributes)
    {
        std::string color = bbs_get_attribute_value_string(attributes, num_attributes, COLOR_ATTR);
        m_group_id_to_color[m_current_color_group] = color;
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_color()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_mesh(const char** attributes, unsigned int num_attributes)
    {
        // reset current geometry
        if (m_curr_object)
            m_curr_object->geometry.reset();
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_mesh()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_vertices(const char** attributes, unsigned int num_attributes)
    {
        // reset current vertices
        if (m_curr_object)
            m_curr_object->geometry.vertices.clear();
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_vertices()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_vertex(const char** attributes, unsigned int num_attributes)
    {
        // appends the vertex coordinates
        // missing values are set equal to ZERO
        if (m_curr_object)
            m_curr_object->geometry.vertices.emplace_back(
                m_unit_factor * bbs_get_attribute_value_float(attributes, num_attributes, X_ATTR),
                m_unit_factor * bbs_get_attribute_value_float(attributes, num_attributes, Y_ATTR),
                m_unit_factor * bbs_get_attribute_value_float(attributes, num_attributes, Z_ATTR));
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_vertex()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_triangles(const char** attributes, unsigned int num_attributes)
    {
        // reset current triangles
        if (m_curr_object)
            m_curr_object->geometry.triangles.clear();
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_triangles()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_triangle(const char** attributes, unsigned int num_attributes)
    {
        // we are ignoring the following attributes:
        // p1
        // p2
        // p3
        // pid
        // see specifications

        // appends the triangle's vertices indices
        // missing values are set equal to ZERO
        if (m_curr_object) {
            m_curr_object->geometry.triangles.emplace_back(
                bbs_get_attribute_value_int(attributes, num_attributes, V1_ATTR),
                bbs_get_attribute_value_int(attributes, num_attributes, V2_ATTR),
                bbs_get_attribute_value_int(attributes, num_attributes, V3_ATTR));

            m_curr_object->geometry.custom_supports.push_back(bbs_get_attribute_value_string(attributes, num_attributes, CUSTOM_SUPPORTS_ATTR));
            m_curr_object->geometry.custom_seam.push_back(bbs_get_attribute_value_string(attributes, num_attributes, CUSTOM_SEAM_ATTR));
            m_curr_object->geometry.mmu_segmentation.push_back(bbs_get_attribute_value_string(attributes, num_attributes, MMU_SEGMENTATION_ATTR));
            m_curr_object->geometry.fuzzy_skin.push_back(bbs_get_attribute_value_string(attributes, num_attributes, CUSTOM_FUZZY_SKIN_ATTR));
            // BBS
            m_curr_object->geometry.face_properties.push_back(bbs_get_attribute_value_string(attributes, num_attributes, FACE_PROPERTY_ATTR));
        }
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_triangle()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_components(const char** attributes, unsigned int num_attributes)
    {
        // reset current components
        if (m_curr_object)
            m_curr_object->components.clear();
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_components()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_component(const char** attributes, unsigned int num_attributes)
    {
        std::string path      = xml_unescape(bbs_get_attribute_value_string(attributes, num_attributes, PPATH_ATTR));
        int         object_id = bbs_get_attribute_value_int(attributes, num_attributes, OBJECTID_ATTR);
        Transform3d transform = bbs_get_transform_from_3mf_specs_string(bbs_get_attribute_value_string(attributes, num_attributes, TRANSFORM_ATTR));

        /*Id id = std::make_pair(m_sub_model_path, object_id);
        IdToModelObjectMap::iterator object_item = m_objects.find(id);
        if (object_item == m_objects.end()) {
            IdToAliasesMap::iterator alias_item = m_objects_aliases.find(id);
            if (alias_item == m_objects_aliases.end()) {
                add_error("Found component with invalid object id");
                return false;
            }
        }*/

        if (m_curr_object) {
            Id id = std::make_pair(m_sub_model_path.empty() ? path : m_sub_model_path, object_id);
            m_curr_object->components.emplace_back(id, transform);
        }

        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_component()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_build(const char** attributes, unsigned int num_attributes)
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_build()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_item(const char** attributes, unsigned int num_attributes)
    {
        // we are ignoring the following attributes
        // thumbnail
        // partnumber
        // pid
        // pindex
        // see specifications

        int object_id = bbs_get_attribute_value_int(attributes, num_attributes, OBJECTID_ATTR);
        std::string path = bbs_get_attribute_value_string(attributes, num_attributes, PPATH_ATTR);
        Transform3d transform = bbs_get_transform_from_3mf_specs_string(bbs_get_attribute_value_string(attributes, num_attributes, TRANSFORM_ATTR));
        int printable = bbs_get_attribute_value_bool(attributes, num_attributes, PRINTABLE_ATTR);

        return !m_load_model || _create_object_instance(path, object_id, transform, printable, 1);
    }

    bool _BBS_3MF_Importer::_handle_end_item()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_metadata(const char** attributes, unsigned int num_attributes)
    {
        m_curr_characters.clear();

        std::string name = bbs_get_attribute_value_string(attributes, num_attributes, NAME_ATTR);
        if (!name.empty()) {
            m_curr_metadata_name = name;
        }

        return true;
    }

    inline static void check_painting_version(unsigned int loaded_version, unsigned int highest_supported_version, const std::string &error_msg)
    {
        if (loaded_version > highest_supported_version)
            throw version_error(error_msg);
    }

    bool _BBS_3MF_Importer::_handle_end_metadata()
    {
        if ((m_curr_metadata_name == BBS_3MF_VERSION)||(m_curr_metadata_name == BBS_3MF_VERSION1)) {
            //m_is_bbl_3mf = true;
            m_version = (unsigned int)atoi(m_curr_characters.c_str());
            /*if (m_check_version && (m_version > VERSION_BBS_3MF_COMPATIBLE)) {
                // std::string msg = _(L("The selected 3mf file has been saved with a newer version of " + std::string(SLIC3R_APP_NAME) + " and is not compatible."));
                // throw version_error(msg.c_str());
                const std::string msg = (boost::format(_(L("The selected 3mf file has been saved with a newer version of %1% and is not compatible."))) % std::string(SLIC3R_APP_NAME)).str();
                throw version_error(msg);
            }*/
        } else if (m_curr_metadata_name == BBL_APPLICATION_TAG) {
            // Generator application of the 3MF.
            // SLIC3R_APP_KEY - SoftFever_VERSION
            if (boost::starts_with(m_curr_characters, "BambuStudio-")) {
                m_is_bbl_3mf = true;
                m_bambuslicer_generator_version = Semver::parse(m_curr_characters.substr(12));
            }
            else if (boost::starts_with(m_curr_characters, "OrcaSlicer-")) {
                m_is_bbl_3mf = true;
                m_bambuslicer_generator_version = Semver::parse(m_curr_characters.substr(11));
            }
        //TODO: currently use version 0, no need to load&&save this string
        /*} else if (m_curr_metadata_name == BBS_FDM_SUPPORTS_PAINTING_VERSION) {
            m_fdm_supports_painting_version = (unsigned int) atoi(m_curr_characters.c_str());
            check_painting_version(m_fdm_supports_painting_version, FDM_SUPPORTS_PAINTING_VERSION,
                _(L("The selected 3MF contains FDM supports painted object using a newer version of OrcaSlicer and is not compatible.")));
        } else if (m_curr_metadata_name == BBS_SEAM_PAINTING_VERSION) {
            m_seam_painting_version = (unsigned int) atoi(m_curr_characters.c_str());
            check_painting_version(m_seam_painting_version, SEAM_PAINTING_VERSION,
                _(L("The selected 3MF contains seam painted object using a newer version of OrcaSlicer and is not compatible.")));
        } else if (m_curr_metadata_name == BBS_MM_PAINTING_VERSION) {
            m_mm_painting_version = (unsigned int) atoi(m_curr_characters.c_str());
            check_painting_version(m_mm_painting_version, MM_PAINTING_VERSION,
                _(L("The selected 3MF contains multi-material painted object using a newer version of OrcaSlicer and is not compatible.")));*/
        } else if (m_curr_metadata_name == BBL_MODEL_ID_TAG) {
            m_model_id = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_MODEL_NAME_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found model name = " << m_curr_characters;
            model_info.model_name = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_ORIGIN_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found model name = " << m_curr_characters;
            model_info.origin = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_DESIGNER_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found designer = " << m_curr_characters;
            m_designer = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_DESIGNER_USER_ID_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found designer_user_id = " << m_curr_characters;
            m_designer_user_id = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_DESIGNER_COVER_FILE_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found designer_cover = " << m_curr_characters;
            model_info.cover_file = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_DESCRIPTION_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found description = " << m_curr_characters;
            model_info.description = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_LICENSE_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found license = " << m_curr_characters;
            model_info.license = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_COPYRIGHT_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found CopyRight = " << m_curr_characters;
            model_info.copyright = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_COPYRIGHT_NORMATIVE_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found Copyright = " << m_curr_characters;
            model_info.copyright = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_REGION_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found region = " << m_curr_characters;
            m_contry_code = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_PROFILE_TITLE_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found profile_title = " << m_curr_characters;
            m_profile_title = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_PROFILE_COVER_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found profile_cover = " << m_curr_characters;
            m_profile_cover = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_PROFILE_DESCRIPTION_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found profile_description = " << m_curr_characters;
            m_Profile_description = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_PROFILE_USER_ID_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found profile_user_id = " << m_curr_characters;
            m_profile_user_id = xml_unescape(m_curr_characters);
        }else if (m_curr_metadata_name == BBL_PROFILE_USER_NAME_TAG) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, load_3mf found profile_user_name = " << m_curr_characters;
            m_profile_user_name = xml_unescape(m_curr_characters);
        } else if (m_curr_metadata_name == BBL_CREATION_DATE_TAG) {
            ;
        } else if (m_curr_metadata_name == BBL_MODIFICATION_TAG) {
            ;
        } else {
            ;
        }
        if (!m_curr_metadata_name.empty()) {
            BOOST_LOG_TRIVIAL(info) << "load_3mf found metadata key = " << m_curr_metadata_name << ", value = " << xml_unescape(m_curr_characters);
            model_info.metadata_items[m_curr_metadata_name] = xml_unescape(m_curr_characters);
        }

        return true;
    }

    struct TextConfigurationSerialization
    {
    public:
        TextConfigurationSerialization() = delete;
                
        using TypeToName = boost::bimap<EmbossStyle::Type, std::string_view>;
        static const TypeToName type_to_name;

        using HorizontalAlignToName = boost::bimap<FontProp::HorizontalAlign, std::string_view>;
        static const HorizontalAlignToName horizontal_align_to_name;

        using VerticalAlignToName = boost::bimap<FontProp::VerticalAlign, std::string_view>;
        static const VerticalAlignToName vertical_align_to_name;
        
        static EmbossStyle::Type get_type(std::string_view type) {
            const auto& to_type = TextConfigurationSerialization::type_to_name.right;
            auto type_item = to_type.find(type);
            assert(type_item != to_type.end());
            if (type_item == to_type.end()) return EmbossStyle::Type::undefined;
            return type_item->second;        
        }

        static std::string_view get_name(EmbossStyle::Type type) {
            const auto& to_name = TextConfigurationSerialization::type_to_name.left;
            auto type_name = to_name.find(type);
            assert(type_name != to_name.end());
            if (type_name == to_name.end()) return "unknown type";
            return type_name->second;
        }

        static void to_xml(std::stringstream &stream, const TextConfiguration &tc);
        static std::optional<TextConfiguration> read(const char **attributes, unsigned int num_attributes);
        static EmbossShape read_old(const char **attributes, unsigned int num_attributes);
    };

    bool _BBS_3MF_Importer::_handle_start_text_configuration(const char **attributes, unsigned int num_attributes)
    {
        IdToMetadataMap::iterator object = m_objects_metadata.find(m_curr_config.object_id);
        if (object == m_objects_metadata.end()) {
            add_error("Can not assign volume mesh to a valid object");
            return false;
        }
        if (object->second.volumes.empty()) {
            add_error("Can not assign mesh to a valid volume");
            return false;
        }
        ObjectMetadata::VolumeMetadata& volume = object->second.volumes.back();
        volume.text_configuration = TextConfigurationSerialization::read(attributes, num_attributes);
        if (!volume.text_configuration.has_value())
            return false;

        // Is 3mf version with shapes?
        if (volume.shape_configuration.has_value())
            return true;

        // Back compatibility for 3mf version without shapes
        volume.shape_configuration = TextConfigurationSerialization::read_old(attributes, num_attributes);
        return true;
    }

    // Definition of read/write method for EmbossShape
    static void                       to_xml(std::stringstream &stream, const EmbossShape &es, const ModelVolume &volume, mz_zip_archive &archive,bool export_full_path);
    static std::optional<EmbossShape> read_emboss_shape(const char **attributes, unsigned int num_attributes);

    bool _BBS_3MF_Importer::_handle_start_shape_configuration(const char **attributes, unsigned int num_attributes)
    {
        IdToMetadataMap::iterator object = m_objects_metadata.find(m_curr_config.object_id);
        if (object == m_objects_metadata.end()) {
            add_error("Can not assign volume mesh to a valid object");
            return false;
        }
        auto &volumes = object->second.volumes;
        if (volumes.empty()) {
            add_error("Can not assign mesh to a valid volume");
            return false;
        }
        ObjectMetadata::VolumeMetadata &volume = volumes.back();
        volume.shape_configuration = read_emboss_shape(attributes, num_attributes);
        if (!volume.shape_configuration.has_value())
            return false;

        // Fill svg file content into shape_configuration
        std::optional<EmbossShape::SvgFile> &svg = volume.shape_configuration->svg_file;
        if (!svg.has_value())
            return true; // do not contain svg file

        const std::string &path = svg->path_in_3mf;
        if (path.empty()) 
            return true; // do not contain svg file

        auto it = m_path_to_emboss_shape_files.find(path);
        if (it == m_path_to_emboss_shape_files.end())
            return true; // svg file is not loaded yet

        svg->file_data = it->second;
        return true;
    }

    bool _BBS_3MF_Importer::_create_object_instance(std::string const & path, int object_id, const Transform3d& transform, const bool printable, unsigned int recur_counter)
    {
        static const unsigned int MAX_RECURSIONS = 10;

        // escape from circular aliasing
        if (recur_counter > MAX_RECURSIONS) {
            add_error("Too many recursions");
            return false;
        }

        Id id{path, object_id};
        IdToCurrentObjectMap::iterator it = m_current_objects.find(id);
        if (it == m_current_objects.end()) {
            add_error("can not find object id " + std::to_string(object_id) + " to builditem");
            return false;
        }

        IdToModelObjectMap::iterator object_item = m_objects.find(id);
        if (object_item == m_objects.end()) {
            //add object
            CurrentObject& current_object = it->second;
            int object_index =  (int)m_model->objects.size();
            ModelObject* model_object = m_model->add_object();
            if (model_object == nullptr) {
                add_error("Unable to create object for builditem, id " + std::to_string(object_id));
                return false;
            }
            m_objects.insert({ id, object_index });
            current_object.model_object_idx = object_index;
            current_object.object = model_object;

            ModelInstance* instance = m_model->objects[object_index]->add_instance();
            if (instance == nullptr) {
                add_error("error when add object instance for id " + std::to_string(object_id));
                return false;
            }
            instance->printable = printable;

            m_instances.emplace_back(instance, transform);

            if (m_is_bbl_3mf && boost::ends_with(current_object.uuid, OBJECT_UUID_SUFFIX)) {
                std::istringstream iss(current_object.uuid);
                int backup_id;
                if (iss >> std::hex >> backup_id) {
                    m_model->set_object_backup_id(*model_object, backup_id);
                }
            }
            /*if (!current_object.geometry.empty()) {
            }
            else if (!current_object.components.empty()) {
                 // recursively process nested components
                for (const Component& component : it->second) {
                    if (!_create_object_instance(path, component.object_id, transform * component.transform, printable, recur_counter + 1))
                        return false;
                }
            }
            else {
                add_error("can not construct build items with invalid object, id " + std::to_string(object_id));
                return false;
            }*/
        }
        else {
            //add instance
            ModelInstance* instance = m_model->objects[object_item->second]->add_instance();
            if (instance == nullptr) {
                add_error("error when add object instance for id " + std::to_string(object_id));
                return false;
            }
            instance->printable = printable;

            m_instances.emplace_back(instance, transform);
        }

        /*if (it->second.size() == 1 && it->second[0].object_id == object_id) {
            // aliasing to itself

            IdToModelObjectMap::iterator object_item = m_objects.find(id);
            if (object_item == m_objects.end() || object_item->second == -1) {
                add_error("Found invalid object");
                return false;
            }
            else {
                ModelInstance* instance = m_model->objects[object_item->second]->add_instance();
                if (instance == nullptr) {
                    add_error("Unable to add object instance");
                    return false;
                }
                instance->printable = printable;

                m_instances.emplace_back(instance, transform);
            }
        }
        else {
            // recursively process nested components
            for (const Component& component : it->second) {
                if (!_create_object_instance(path, component.object_id, transform * component.transform, printable, recur_counter + 1))
                    return false;
            }
        }*/

        return true;
    }

    void _BBS_3MF_Importer::_apply_transform(ModelInstance& instance, const Transform3d& transform)
    {
        Slic3r::Geometry::Transformation t(transform);
        // invalid scale value, return
        if (!t.get_scaling_factor().all())
            return;

        instance.set_transformation(t);
    }

    bool _BBS_3MF_Importer::_handle_start_config(const char** attributes, unsigned int num_attributes)
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_config()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_config_object(const char** attributes, unsigned int num_attributes)
    {
        if (m_parsing_slice_info)
            return true;
        int object_id = bbs_get_attribute_value_int(attributes, num_attributes, ID_ATTR);
        IdToMetadataMap::iterator object_item = m_objects_metadata.find(object_id);
        if (object_item != m_objects_metadata.end()) {
            add_error("Duplicated object id: " + std::to_string(object_id) + " in model_settings.config");
            return false;
        }

        // Added because of github #3435, currently not used by PrusaSlicer
        // int instances_count_id = bbs_get_attribute_value_int(attributes, num_attributes, INSTANCESCOUNT_ATTR);

        m_objects_metadata.insert({ object_id, ObjectMetadata() });
        m_curr_config.object_id = object_id;
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_config_object()
    {
        m_curr_config.object_id = -1;
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_config_volume(const char** attributes, unsigned int num_attributes)
    {
        IdToMetadataMap::iterator object = m_objects_metadata.find(m_curr_config.object_id);
        if (object == m_objects_metadata.end()) {
            add_error("can not find object for part, id " + std::to_string(m_curr_config.object_id) );
            return false;
        }

        m_curr_config.volume_id = (int)object->second.volumes.size();

        unsigned int first_triangle_id = (unsigned int)bbs_get_attribute_value_int(attributes, num_attributes, FIRST_TRIANGLE_ID_ATTR);
        unsigned int last_triangle_id = (unsigned int)bbs_get_attribute_value_int(attributes, num_attributes, LAST_TRIANGLE_ID_ATTR);

        //BBS: refine the part type logic
        std::string subtype_str = bbs_get_attribute_value_string(attributes, num_attributes, SUBTYPE_ATTR);
        ModelVolumeType type = ModelVolume::type_from_string(subtype_str);

        int subbject_id = bbs_get_attribute_value_int(attributes, num_attributes, ID_ATTR);

        if (last_triangle_id > 0)
            object->second.volumes.emplace_back(first_triangle_id, last_triangle_id, type);
        else
            object->second.volumes.emplace_back(subbject_id, type);
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_config_volume_mesh(const char** attributes, unsigned int num_attributes)
    {
        IdToMetadataMap::iterator object = m_objects_metadata.find(m_curr_config.object_id);
        if (object == m_objects_metadata.end()) {
            add_error("can not find object for mesh_stats, id " + std::to_string(m_curr_config.object_id) );
            return false;
        }
        if ((m_curr_config.volume_id == -1) || ((object->second.volumes.size() - 1) < m_curr_config.volume_id)) {
            add_error("can not find part for mesh_stats");
            return false;
        }

        ObjectMetadata::VolumeMetadata& volume = object->second.volumes[m_curr_config.volume_id];

        int edges_fixed         = bbs_get_attribute_value_int(attributes, num_attributes, MESH_STAT_EDGES_FIXED       );
        int degenerate_facets   = bbs_get_attribute_value_int(attributes, num_attributes, MESH_STAT_DEGENERATED_FACETS);
        int facets_removed      = bbs_get_attribute_value_int(attributes, num_attributes, MESH_STAT_FACETS_REMOVED    );
        int facets_reversed     = bbs_get_attribute_value_int(attributes, num_attributes, MESH_STAT_FACETS_RESERVED   );
        int backwards_edges     = bbs_get_attribute_value_int(attributes, num_attributes, MESH_STAT_BACKWARDS_EDGES   );

        volume.mesh_stats = { edges_fixed, degenerate_facets, facets_removed, facets_reversed, backwards_edges };

        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_config_volume()
    {
        m_curr_config.volume_id = -1;
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_config_volume_mesh()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_config_metadata(const char** attributes, unsigned int num_attributes)
    {
        //std::string type = bbs_get_attribute_value_string(attributes, num_attributes, TYPE_ATTR);
        std::string key = bbs_get_attribute_value_string(attributes, num_attributes, KEY_ATTR);
        std::string value = bbs_get_attribute_value_string(attributes, num_attributes, VALUE_ATTR);
        if (key.empty())
            return true;

        auto get_vector_from_string = [](const std::string& str) -> std::vector<int> {
            std::stringstream stream(str);
            int value;
            std::vector<int>  results;
            while (stream >> value) {
                results.push_back(value);
            }
            return results;
        };

        auto get_vector_array_from_string = [get_vector_from_string](const std::string &str) -> std::vector<std::vector<int>> {
            std::vector<std::string> sub_strs;
            size_t pos = 0;
            size_t found = 0;
            while ((found = str.find('#', pos)) != std::string::npos) {
                std::string sub_str = str.substr(pos, found - pos);
                sub_strs.push_back(sub_str);
                pos = found + 1;
            }

            std::vector<std::vector<int>> results;
            for (std::string sub_str : sub_strs) {
                results.emplace_back(get_vector_from_string(sub_str));
            }

            return results;
        };

        if ((m_curr_plater == nullptr)&&!m_parsing_slice_info)
        {
            IdToMetadataMap::iterator object = m_objects_metadata.find(m_curr_config.object_id);
            if (object == m_objects_metadata.end()) {
                add_error("Cannot find object for metadata, id " + std::to_string(m_curr_config.object_id));
                return false;
            }
            if (m_curr_config.volume_id == -1)
                object->second.metadata.emplace_back(key, value);
            else {
                if (size_t(m_curr_config.volume_id) < object->second.volumes.size())
                    object->second.volumes[m_curr_config.volume_id].metadata.emplace_back(key, value);
            }
        }
        else
        {
            //plater
            if (key == PLATERID_ATTR)
            {
                m_curr_plater->plate_index = atoi(value.c_str());
            }
            else if (key == PLATER_NAME_ATTR) {
                m_curr_plater->plate_name = xml_unescape(value.c_str());
            }
            else if (key == LOCK_ATTR)
            {
                std::istringstream(value) >> std::boolalpha >> m_curr_plater->locked;
            }
            else if (key == BED_TYPE_ATTR)
            {
                BedType bed_type = BedType::btPC;
                ConfigOptionEnum<BedType>::from_string(value, bed_type);
                m_curr_plater->config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(bed_type));
            }
            else if (key == PRINT_SEQUENCE_ATTR)
            {
                PrintSequence print_sequence = PrintSequence::ByLayer;
                ConfigOptionEnum<PrintSequence>::from_string(value, print_sequence);
                m_curr_plater->config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(print_sequence));
            }
            else if (key == FIRST_LAYER_PRINT_SEQUENCE_ATTR) {
                m_curr_plater->config.set_key_value("first_layer_print_sequence", new ConfigOptionInts(get_vector_from_string(value)));
            }
            else if (key == OTHER_LAYERS_PRINT_SEQUENCE_ATTR) {
                m_curr_plater->config.set_key_value("other_layers_print_sequence", new ConfigOptionInts(get_vector_from_string(value)));
            }
            else if (key == OTHER_LAYERS_PRINT_SEQUENCE_NUMS_ATTR) {
                m_curr_plater->config.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(stoi(value)));
            }
            else if (key == SPIRAL_VASE_MODE) {
                bool spiral_mode = false;
                std::istringstream(value) >> std::boolalpha >> spiral_mode;
                m_curr_plater->config.set_key_value("spiral_mode", new ConfigOptionBool(spiral_mode));
            }
            else if (key == FILAMENT_MAP_MODE_ATTR)
            {
                FilamentMapMode map_mode = FilamentMapMode::fmmAutoForFlush;
                // handle old versions, only load manual params
                if (value != "Auto") {
                    ConfigOptionEnum<FilamentMapMode>::from_string(value, map_mode);
                    m_curr_plater->config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(map_mode));
                }
            }
            else if (key == FILAMENT_MAP_ATTR) {
                if (m_curr_plater){
                    auto filament_map = get_vector_from_string(value);
                    for (size_t idx = 0; idx < filament_map.size(); ++idx) {
                        if (filament_map[idx] < 1) {
                            filament_map[idx] = 1;
                        }
                    }
                    m_curr_plater->config.set_key_value("filament_map", new ConfigOptionInts(filament_map));
                }
            }
            else if (key == GCODE_FILE_ATTR)
            {
                m_curr_plater->gcode_file = value;
            }
            else if (key == THUMBNAIL_FILE_ATTR)
            {
                m_curr_plater->thumbnail_file = value;
            }
            else if (key == NO_LIGHT_THUMBNAIL_FILE_ATTR)
            {
                m_curr_plater->no_light_thumbnail_file = value;
            }
            else if (key == TOP_FILE_ATTR)
            {
                m_curr_plater->top_file = value;
            }
            else if (key == PICK_FILE_ATTR)
            {
                m_curr_plater->pick_file = value;
            }
            //else if (key == PATTERN_FILE_ATTR)
            //{
            //    m_curr_plater->pattern_file = value;
            //}
            else if (key == PATTERN_BBOX_FILE_ATTR)
            {
                m_curr_plater->pattern_bbox_file = value;
            }
            else if (key == INSTANCEID_ATTR)
            {
                m_curr_instance.instance_id = atoi(value.c_str());
            }
            else if (key == IDENTIFYID_ATTR)
            {
                m_curr_instance.identify_id = atoi(value.c_str());
            }
            else if (key == OBJECT_ID_ATTR)
            {
                m_curr_instance.object_id = atoi(value.c_str());
                /*int obj_id = atoi(value.c_str());
                m_curr_instance.object_id = -1;
                IndexToPathMap::iterator index_iter = m_index_paths.find(obj_id);
                if (index_iter == m_index_paths.end()) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ":" << __LINE__
                        << boost::format(", can not find object for plate's item, id=%1%, skip this object")%obj_id;
                    return true;
                }
                Id temp_id = std::make_pair(index_iter->second, index_iter->first);
                IdToModelObjectMap::iterator object_item = m_objects.find(temp_id);
                if (object_item == m_objects.end()) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ":" << __LINE__
                        << boost::format(", can not find object for plate's item, ID <%1%, %2%>, skip this object")%index_iter->second %index_iter->first;
                    return true;
                }
                m_curr_instance.object_id = object_item->second;*/
            }
            else if (key == PLATE_IDX_ATTR)
            {
                int plate_index = atoi(value.c_str());
                std::map<int, PlateData*>::iterator it = m_plater_data.find(plate_index);
                if (it != m_plater_data.end())
                    m_curr_plater = it->second;
            }
            else if (key == SLICE_PREDICTION_ATTR)
            {
                if (m_curr_plater)
                    m_curr_plater->gcode_prediction = value;
            }
            else if (key == SLICE_WEIGHT_ATTR)
            {
                if (m_curr_plater)
                    m_curr_plater->gcode_weight = value;
            }
            else if (key == OUTSIDE_ATTR)
            {
                if (m_curr_plater)
                    std::istringstream(value) >> std::boolalpha >> m_curr_plater->toolpath_outside;
            }
            else if (key == SUPPORT_USED_ATTR)
            {
                if (m_curr_plater)
                    std::istringstream(value) >> std::boolalpha >> m_curr_plater->is_support_used;
            }
            else if (key == LABEL_OBJECT_ENABLED_ATTR)
            {
                if (m_curr_plater)
                    std::istringstream(value) >> std::boolalpha >> m_curr_plater->is_label_object_enabled;
            }
            else if (key == PRINTER_MODEL_ID_ATTR)
            {
                if (m_curr_plater)
                    m_curr_plater->printer_model_id = value;
            }
            else if (key == NOZZLE_DIAMETERS_ATTR)
            {
                if (m_curr_plater)
                    m_curr_plater->nozzle_diameters = value;
            }
        }

        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_config_metadata()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_config_filament(const char** attributes, unsigned int num_attributes)
    {
        if (m_curr_plater) {
            std::string id = bbs_get_attribute_value_string(attributes, num_attributes, FILAMENT_ID_TAG);
            std::string type = bbs_get_attribute_value_string(attributes, num_attributes, FILAMENT_TYPE_TAG);
            std::string color = bbs_get_attribute_value_string(attributes, num_attributes, FILAMENT_COLOR_TAG);
            std::string used_m = bbs_get_attribute_value_string(attributes, num_attributes, FILAMENT_USED_M_TAG);
            std::string used_g = bbs_get_attribute_value_string(attributes, num_attributes, FILAMENT_USED_G_TAG);
            std::string filament_id = bbs_get_attribute_value_string(attributes, num_attributes, FILAMENT_TRAY_INFO_ID_TAG);
            FilamentInfo filament_info;
            filament_info.id = atoi(id.c_str()) - 1;
            filament_info.type = type;
            filament_info.color = color;
            filament_info.used_m = atof(used_m.c_str());
            filament_info.used_g = atof(used_g.c_str());
            filament_info.filament_id = filament_id;
            m_curr_plater->slice_filaments_info.push_back(filament_info);
        }
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_config_filament()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_config_warning(const char** attributes, unsigned int num_attributes)
    {
        if (m_curr_plater) {
            std::string msg     = bbs_get_attribute_value_string(attributes, num_attributes, WARNING_MSG_TAG);
            std::string lvl_str = bbs_get_attribute_value_string(attributes, num_attributes, "level");
            GCodeProcessorResult::SliceWarning sw;
            sw.msg = msg;
            try {
                sw.level = atoi(lvl_str.c_str());
            }
            catch(...) {
            };

            m_curr_plater->warnings.push_back(sw);
        }
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_config_warning()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_config_plater(const char** attributes, unsigned int num_attributes)
    {
        if (!m_parsing_slice_info) {
            m_curr_plater = new PlateData();
        }

        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_config_plater()
    {
        if (!m_curr_plater)
        {
            add_error("_handle_end_config_plater: don't find plate created before");
            return false;
        }
        m_plater_data.emplace(m_curr_plater->plate_index, m_curr_plater);
        m_curr_plater = nullptr;
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_config_plater_instance(const char** attributes, unsigned int num_attributes)
    {
        if (!m_curr_plater)
        {
            add_error("_handle_start_config_plater_instance: don't find plate created before");
            return false;
        }

        //do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_config_plater_instance()
    {
        if (!m_curr_plater)
        {
            add_error("_handle_end_config_plater_instance: don't find plate created before");
            return false;
        }
        if ((m_curr_instance.object_id == -1) || (m_curr_instance.instance_id == -1))
        {
            //add_error("invalid object id/instance id");
            //skip this instance
            m_curr_instance.object_id = m_curr_instance.instance_id = -1;
            m_curr_instance.identify_id = 0;
            return true;
        }

        m_curr_plater->obj_inst_map.emplace(m_curr_instance.object_id, std::make_pair(m_curr_instance.instance_id, m_curr_instance.identify_id));
        m_curr_instance.object_id = m_curr_instance.instance_id = -1;
        m_curr_instance.identify_id = 0;
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_assemble(const char** attributes, unsigned int num_attributes)
    {
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_assemble()
    {
        //do nothing
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_assemble_item(const char** attributes, unsigned int num_attributes)
    {
        if (!m_load_model) return true;

        int object_id = bbs_get_attribute_value_int(attributes, num_attributes, OBJECT_ID_ATTR);
        int instance_id = bbs_get_attribute_value_int(attributes, num_attributes, INSTANCEID_ATTR);

        IndexToPathMap::iterator index_iter = m_index_paths.find(object_id);
        if (index_iter == m_index_paths.end()) {
            add_error("can not find object for assemble item, id= " + std::to_string(object_id));
            return false;
        }
        Id temp_id = std::make_pair(index_iter->second, index_iter->first);
        IdToModelObjectMap::iterator object_item = m_objects.find(temp_id);
        if (object_item == m_objects.end()) {
            add_error("can not find object for assemble item, id= " + std::to_string(object_id));
            return false;
        }
        object_id = object_item->second;

        Transform3d transform = bbs_get_transform_from_3mf_specs_string(bbs_get_attribute_value_string(attributes, num_attributes, TRANSFORM_ATTR));
        Vec3d ofs2ass = bbs_get_offset_from_3mf_specs_string(bbs_get_attribute_value_string(attributes, num_attributes, OFFSET_ATTR));
        if (object_id < m_model->objects.size()) {
            if (instance_id < m_model->objects[object_id]->instances.size()) {
                m_model->objects[object_id]->instances[instance_id]->set_assemble_from_transform(transform);
                m_model->objects[object_id]->instances[instance_id]->set_offset_to_assembly(ofs2ass);
            }
        }
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_assemble_item()
    {
        return true;
    }

    bool _BBS_3MF_Importer::_handle_start_text_info_item(const char **attributes, unsigned int num_attributes)
    {
        IdToMetadataMap::iterator object = m_objects_metadata.find(m_curr_config.object_id);
        if (object == m_objects_metadata.end()) {
            add_error("can not find object for text_info, id " + std::to_string(m_curr_config.object_id));
            return false;
        }
        if ((m_curr_config.volume_id == -1) || ((object->second.volumes.size() - 1) < m_curr_config.volume_id)) {
            add_error("can not find part for text_info");
            return false;
        }

        ObjectMetadata::VolumeMetadata &volume = object->second.volumes[m_curr_config.volume_id];

        if (volume.text_configuration.has_value()) {
            add_error("Both text_info and text_configuration found, ignore legacy text_info");
            return true;
        }

        // TODO: Orca: support legacy text info
        /*
        TextInfo text_info;
        text_info.m_text      = xml_unescape(bbs_get_attribute_value_string(attributes, num_attributes, TEXT_ATTR));
        text_info.m_font_name = bbs_get_attribute_value_string(attributes, num_attributes, FONT_NAME_ATTR);

        text_info.m_curr_font_idx = bbs_get_attribute_value_int(attributes, num_attributes, FONT_INDEX_ATTR);

        text_info.m_font_size = bbs_get_attribute_value_float(attributes, num_attributes, FONT_SIZE_ATTR);
        text_info.m_thickness = bbs_get_attribute_value_float(attributes, num_attributes, THICKNESS_ATTR);
        text_info.m_embeded_depth = bbs_get_attribute_value_float(attributes, num_attributes, EMBEDED_DEPTH_ATTR);
        text_info.m_rotate_angle  = bbs_get_attribute_value_float(attributes, num_attributes, ROTATE_ANGLE_ATTR);
        text_info.m_text_gap      = bbs_get_attribute_value_float(attributes, num_attributes, TEXT_GAP_ATTR);

        text_info.m_bold      = bbs_get_attribute_value_int(attributes, num_attributes, BOLD_ATTR);
        text_info.m_italic    = bbs_get_attribute_value_int(attributes, num_attributes, ITALIC_ATTR);
        text_info.m_is_surface_text = bbs_get_attribute_value_int(attributes, num_attributes, SURFACE_TEXT_ATTR);
        text_info.m_keep_horizontal = bbs_get_attribute_value_int(attributes, num_attributes, KEEP_HORIZONTAL_ATTR);

        text_info.m_rr.mesh_id = bbs_get_attribute_value_int(attributes, num_attributes, HIT_MESH_ATTR);

        std::string hit_pos = bbs_get_attribute_value_string(attributes, num_attributes, HIT_POSITION_ATTR);
        if (!hit_pos.empty())
            text_info.m_rr.hit = get_vec3_from_string(hit_pos);

        std::string hit_normal = bbs_get_attribute_value_string(attributes, num_attributes, HIT_NORMAL_ATTR);
        if (!hit_normal.empty())
            text_info.m_rr.normal = get_vec3_from_string(hit_normal);

        volume.text_info = text_info;*/
        return true;
    }

    bool _BBS_3MF_Importer::_handle_end_text_info_item()
    {
        return true;
    }

    void XMLCALL _BBS_3MF_Importer::_handle_start_relationships_element(void* userData, const char* name, const char** attributes)
    {
        _BBS_3MF_Importer* importer = (_BBS_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_start_relationships_element(name, attributes);
    }

    void XMLCALL _BBS_3MF_Importer::_handle_end_relationships_element(void* userData, const char* name)
    {
        _BBS_3MF_Importer* importer = (_BBS_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_end_relationships_element(name);
    }

    void _BBS_3MF_Importer::_handle_start_relationships_element(const char* name, const char** attributes)
    {
        if (m_xml_parser == nullptr)
            return;

        bool res = true;
        unsigned int num_attributes = (unsigned int)XML_GetSpecifiedAttributeCount(m_xml_parser);

        if (::strcmp(RELATIONSHIP_TAG, name) == 0)
            res = _handle_start_relationship(attributes, num_attributes);

        m_curr_characters.clear();
        if (!res)
            _stop_xml_parser();
    }

    void _BBS_3MF_Importer::_handle_end_relationships_element(const char* name)
    {
        if (m_xml_parser == nullptr)
            return;

        bool res = true;

        if (!res)
            _stop_xml_parser();
    }

    bool _BBS_3MF_Importer::_handle_start_relationship(const char** attributes, unsigned int num_attributes)
    {
        std::string path = bbs_get_attribute_value_string(attributes, num_attributes, TARGET_ATTR);
        std::string type = bbs_get_attribute_value_string(attributes, num_attributes, RELS_TYPE_ATTR);
        if (boost::starts_with(type, "http://schemas.microsoft.com/3dmanufacturing/") && boost::ends_with(type, "3dmodel")) {
            if (m_start_part_path.empty()) m_start_part_path = path;
            else m_sub_model_paths.push_back(path);
        } else if (boost::starts_with(type, "http://schemas.openxmlformats.org/") && boost::ends_with(type, "thumbnail")) {
            if (boost::algorithm::ends_with(path, ".png"))
                m_thumbnail_path = path;
        } else if (boost::starts_with(type, "http://schemas.bambulab.com/") && boost::ends_with(type, "cover-thumbnail-middle")) {
            m_thumbnail_middle = path;
        } else if (boost::starts_with(type, "http://schemas.bambulab.com/") && boost::ends_with(type, "cover-thumbnail-small")) {
            m_thumbnail_small = path;
        }
        return true;
    }

    void _BBS_3MF_Importer::_generate_current_object_list(std::vector<Component> &sub_objects, Id object_id, IdToCurrentObjectMap &current_objects)
    {
        std::list<std::pair<Component, Transform3d>> id_list;
        id_list.push_back(std::make_pair(Component(object_id, Transform3d::Identity()), Transform3d::Identity()));

        while (!id_list.empty())
        {
            auto current_item = id_list.front();
            Component current_id = current_item.first;
            id_list.pop_front();
            IdToCurrentObjectMap::iterator current_object = current_objects.find(current_id.object_id);
            if (current_object != current_objects.end()) {
                //found one
                if (!current_object->second.components.empty()) {
                    for (const Component &comp : current_object->second.components) {
                        id_list.push_back(std::pair(comp, current_item.second * comp.transform));
                    }
                }
                else if (!(current_object->second.geometry.empty())) {
                    //CurrentObject* ptr = &(current_objects[current_id]);
                    //CurrentObject* ptr2 = &(current_object->second);
                    sub_objects.push_back({ current_object->first, current_item.second});
                }
            }
        }
    }

    bool _BBS_3MF_Importer::_generate_volumes_new(ModelObject& object, const std::vector<Component> &sub_objects, const ObjectMetadata::VolumeMetadataList& volumes, ConfigSubstitutionContext& config_substitutions)
    {
        if (!object.volumes.empty()) {
            add_error("object already built with parts");
            return false;
        }

        //unsigned int geo_tri_count = (unsigned int)geometry.triangles.size();
        unsigned int renamed_volumes_count = 0;

        for (unsigned int index = 0; index < sub_objects.size(); index++)
        {
            //find the volume metadata firstly
            Component sub_comp = sub_objects[index];
            Id object_id = sub_comp.object_id;
            IdToCurrentObjectMap::iterator current_object = m_current_objects.find(object_id);
            if (current_object == m_current_objects.end()) {
                add_error("sub_objects can not be found, id=" + std::to_string(object_id.second));
                return false;
            }
            CurrentObject* sub_object = &(current_object->second);

            const ObjectMetadata::VolumeMetadata* volume_data = nullptr;
            ObjectMetadata::VolumeMetadata default_volume_data(sub_object->id);
            if (index < volumes.size() && volumes[index].subobject_id == sub_object->id)
                volume_data = &volumes[index];
            else for (const ObjectMetadata::VolumeMetadata& volume_iter : volumes) {
                if (volume_iter.subobject_id == sub_object->id) {
                    volume_data = &volume_iter;
                    break;
                }
            }

            Transform3d volume_matrix_to_object = Transform3d::Identity();
            bool        has_transform 		    = false;
            int         shared_mesh_id          = object_id.second;
            if (volume_data)
            {
                int found_count = 0;
                // extract the volume transformation from the volume's metadata, if present
                for (const Metadata& metadata : volume_data->metadata) {
                    if (metadata.key == MATRIX_KEY) {
                        volume_matrix_to_object = Slic3r::Geometry::transform3d_from_string(metadata.value);
                        has_transform 			= ! volume_matrix_to_object.isApprox(Transform3d::Identity(), 1e-10);
                        found_count++;
                    }
                    else if (metadata.key == MESH_SHARED_KEY){
                        //add the shared mesh logic
                        shared_mesh_id = ::atoi(metadata.value.c_str());
                        found_count++;
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": line %1%, shared_mesh_id %2%")%__LINE__%shared_mesh_id;
                    }

                    if (found_count >= 2)
                        break;
                }
            }
            else {
                //create a volume_data
                volume_data = &default_volume_data;
            }

            ModelVolume* volume = nullptr;
            ModelVolume *shared_volume = nullptr;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": line %1%, subobject_id %2%, shared_mesh_id %3%")%__LINE__ %sub_object->id %shared_mesh_id;
            if (shared_mesh_id != -1) {
                std::map<int, ModelVolume*>::iterator iter = m_shared_meshes.find(shared_mesh_id);
                if (iter != m_shared_meshes.end()) {
                    shared_volume = iter->second;
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": line %1%, found shared mesh, id %2%, mesh %3%")%__LINE__%shared_mesh_id%shared_volume;
                }
            }
            else {
                //for some cases, object point to this shared mesh already loaded, treat that one as the root
                std::map<int, ModelVolume*>::iterator iter = m_shared_meshes.find(sub_object->id);
                if (iter != m_shared_meshes.end()) {
                    shared_volume = iter->second;
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": line %1%, already loaded copy-share mesh before, id %2%, mesh %3%")%__LINE__%sub_object->id%shared_volume;
                }
            }

            const size_t triangles_count = sub_object->geometry.triangles.size();
            if (triangles_count == 0) {
                add_error("found no trianges in the object " + std::to_string(sub_object->id));
                return false;
            }
            if (!shared_volume){
                // splits volume out of imported geometry
                indexed_triangle_set its;
                its.indices.assign(sub_object->geometry.triangles.begin(), sub_object->geometry.triangles.end());
                //const size_t triangles_count = its.indices.size();
                //if (triangles_count == 0) {
                //    add_error("found no trianges in the object " + std::to_string(sub_object->id));
                //    return false;
                //}
                for (const Vec3i32& face : its.indices) {
                    for (const int tri_id : face) {
                        if (tri_id < 0 || tri_id >= int(sub_object->geometry.vertices.size())) {
                            add_error("invalid vertex id in object " + std::to_string(sub_object->id));
                            return false;
                        }
                    }
                }

                its.vertices.assign(sub_object->geometry.vertices.begin(), sub_object->geometry.vertices.end());

                // BBS
                for (const std::string& prop_str : sub_object->geometry.face_properties) {
                    FaceProperty face_prop;
                    face_prop.from_string(prop_str);
                    its.properties.push_back(face_prop);
                }

                TriangleMesh triangle_mesh(std::move(its), volume_data->mesh_stats);

                // BBS: no need to multiply the instance matrix into the volume
                //if (!m_is_bbl_3mf) {
                //    // if the 3mf was not produced by BambuStudio and there is only one instance,
                //    // bake the transformation into the geometry to allow the reload from disk command
                //    // to work properly
                //    if (object.instances.size() == 1) {
                //        triangle_mesh.transform(object.instances.front()->get_transformation().get_matrix(), false);
                //        object.instances.front()->set_transformation(Slic3r::Geometry::Transformation());
                //        //FIXME do the mesh fixing?
                //    }
                //}
                if (triangle_mesh.volume() < 0)
                    triangle_mesh.flip_triangles();

                volume = object.add_volume(std::move(triangle_mesh));

                if (shared_mesh_id != -1)
                    //for some cases the shared mesh is in other plate and not loaded in cli slicing
                    //we need to use the first one in the same plate instead
                    m_shared_meshes[shared_mesh_id] = volume;
                else
                    m_shared_meshes[sub_object->id] = volume;
            }
            else {
                //create volume to use shared mesh
                volume = object.add_volume_with_shared_mesh(*shared_volume);
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": line %1%, create volume using shared_mesh %2%")%__LINE__%shared_volume;
            }
            // stores the volume matrix taken from the metadata, if present
            if (has_transform)
                volume->source.transform = Slic3r::Geometry::Transformation(volume_matrix_to_object);

            volume->calculate_convex_hull();

            //set transform from 3mf
            Slic3r::Geometry::Transformation comp_transformatino(sub_comp.transform);
            volume->set_transformation(comp_transformatino * volume->get_transformation());
            if (shared_volume) {
                const TriangleMesh& trangle_mesh = volume->mesh();
                Vec3d shift = trangle_mesh.get_init_shift();
                if (!shift.isApprox(Vec3d::Zero()))
                    volume->translate(shift);
            }

            // recreate custom supports, seam and mmu segmentation from previously loaded attribute
            {
                volume->supported_facets.reserve(triangles_count);
                volume->seam_facets.reserve(triangles_count);
                volume->mmu_segmentation_facets.reserve(triangles_count);
                volume->fuzzy_skin_facets.reserve(triangles_count);
                for (size_t i=0; i<triangles_count; ++i) {
                    assert(i < sub_object->geometry.custom_supports.size());
                    assert(i < sub_object->geometry.custom_seam.size());
                    assert(i < sub_object->geometry.mmu_segmentation.size());
                    assert(i < sub_object->geometry.fuzzy_skin.size());
                    if (! sub_object->geometry.custom_supports[i].empty())
                        volume->supported_facets.set_triangle_from_string(i, sub_object->geometry.custom_supports[i]);
                    if (! sub_object->geometry.custom_seam[i].empty())
                        volume->seam_facets.set_triangle_from_string(i, sub_object->geometry.custom_seam[i]);
                    if (! sub_object->geometry.mmu_segmentation[i].empty())
                        volume->mmu_segmentation_facets.set_triangle_from_string(i, sub_object->geometry.mmu_segmentation[i]);
                    if (!sub_object->geometry.fuzzy_skin[i].empty())
                        volume->fuzzy_skin_facets.set_triangle_from_string(i, sub_object->geometry.fuzzy_skin[i]);
                }
                volume->supported_facets.shrink_to_fit();
                volume->seam_facets.shrink_to_fit();
                volume->mmu_segmentation_facets.shrink_to_fit();
                volume->mmu_segmentation_facets.touch();
                volume->fuzzy_skin_facets.shrink_to_fit();
                volume->fuzzy_skin_facets.touch();
            }

            volume->set_type(volume_data->part_type);
            
            if (auto &es = volume_data->shape_configuration; es.has_value())
                volume->emboss_shape = std::move(es);            
            if (auto &tc = volume_data->text_configuration; tc.has_value())
                volume->text_configuration = std::move(tc);

            // apply the remaining volume's metadata
            for (const Metadata& metadata : volume_data->metadata) {
                if (metadata.key == NAME_KEY)
                    volume->name = metadata.value;
                //else if ((metadata.key == MODIFIER_KEY) && (metadata.value == "1"))
				//	volume->set_type(ModelVolumeType::PARAMETER_MODIFIER);
				//for old format
                else if ((metadata.key == VOLUME_TYPE_KEY) || (metadata.key == PART_TYPE_KEY))
                    volume->set_type(ModelVolume::type_from_string(metadata.value));
                else if (metadata.key == SOURCE_FILE_KEY)
                    volume->source.input_file = metadata.value;
                else if (metadata.key == SOURCE_OBJECT_ID_KEY)
                    volume->source.object_idx = ::atoi(metadata.value.c_str());
                else if (metadata.key == SOURCE_VOLUME_ID_KEY)
                    volume->source.volume_idx = ::atoi(metadata.value.c_str());
                else if (metadata.key == SOURCE_OFFSET_X_KEY)
                    volume->source.mesh_offset(0) = ::atof(metadata.value.c_str());
                else if (metadata.key == SOURCE_OFFSET_Y_KEY)
                    volume->source.mesh_offset(1) = ::atof(metadata.value.c_str());
                else if (metadata.key == SOURCE_OFFSET_Z_KEY)
                    volume->source.mesh_offset(2) = ::atof(metadata.value.c_str());
                else if (metadata.key == SOURCE_IN_INCHES)
                    volume->source.is_converted_from_inches = metadata.value == "1";
                else if (metadata.key == SOURCE_IN_METERS)
                    volume->source.is_converted_from_meters = metadata.value == "1";
                else if ((metadata.key == MATRIX_KEY) || (metadata.key == MESH_SHARED_KEY))
                    continue;
                else
                    volume->config.set_deserialize(metadata.key, metadata.value, config_substitutions);
            }

            // this may happen for 3mf saved by 3rd part softwares
            if (volume->name.empty()) {
                volume->name = object.name;
                if (renamed_volumes_count > 0)
                    volume->name += "_" + std::to_string(renamed_volumes_count + 1);
                ++renamed_volumes_count;
            }
        }

        return true;
    }
    /*
    bool _BBS_3MF_Importer::_generate_volumes(ModelObject& object, const Geometry& geometry, const ObjectMetadata::VolumeMetadataList& volumes, ConfigSubstitutionContext& config_substitutions)
    {
        if (!object.volumes.empty()) {
            add_error("Found invalid volumes count");
            return false;
        }

        unsigned int geo_tri_count = (unsigned int)geometry.triangles.size();
        unsigned int renamed_volumes_count = 0;

        for (const ObjectMetadata::VolumeMetadata& volume_data : volumes) {
            if (geo_tri_count <= volume_data.first_triangle_id || geo_tri_count <= volume_data.last_triangle_id || volume_data.last_triangle_id < volume_data.first_triangle_id) {
                add_error("Found invalid triangle id");
                return false;
            }

            Transform3d volume_matrix_to_object = Transform3d::Identity();
            bool        has_transform 		    = false;
            // extract the volume transformation from the volume's metadata, if present
            for (const Metadata& metadata : volume_data.metadata) {
                if (metadata.key == MATRIX_KEY) {
                    volume_matrix_to_object = Slic3r::Geometry::transform3d_from_string(metadata.value);
                    has_transform 			= ! volume_matrix_to_object.isApprox(Transform3d::Identity(), 1e-10);
                    break;
                }
            }

            // splits volume out of imported geometry
            indexed_triangle_set its;
            its.indices.assign(geometry.triangles.begin() + volume_data.first_triangle_id, geometry.triangles.begin() + volume_data.last_triangle_id + 1);
            const size_t triangles_count = its.indices.size();
            if (triangles_count == 0) {
                add_error("An empty triangle mesh found");
                return false;
            }

            {
                int min_id = its.indices.front()[0];
                int max_id = min_id;
                for (const Vec3i32& face : its.indices) {
                    for (const int tri_id : face) {
                        if (tri_id < 0 || tri_id >= int(geometry.vertices.size())) {
                            add_error("Found invalid vertex id");
                            return false;
                        }
                        min_id = std::min(min_id, tri_id);
                        max_id = std::max(max_id, tri_id);
                    }
                }
                its.vertices.assign(geometry.vertices.begin() + min_id, geometry.vertices.begin() + max_id + 1);

                // BBS
                for (const std::string prop_str : geometry.face_properties) {
                    FaceProperty face_prop;
                    face_prop.from_string(prop_str);
                    its.properties.push_back(face_prop);
                }

                // rebase indices to the current vertices list
                for (Vec3i32& face : its.indices)
                    for (int& tri_id : face)
                        tri_id -= min_id;
            }

            TriangleMesh triangle_mesh(std::move(its), volume_data.mesh_stats);

            if (!m_is_bbl_3mf) {
                // if the 3mf was not produced by OrcaSlicer and there is only one instance,
                // bake the transformation into the geometry to allow the reload from disk command
                // to work properly
                if (object.instances.size() == 1) {
                    triangle_mesh.transform(object.instances.front()->get_transformation().get_matrix(), false);
                    object.instances.front()->set_transformation(Slic3r::Geometry::Transformation());
                    //FIXME do the mesh fixing?
                }
            }
            if (triangle_mesh.volume() < 0)
                triangle_mesh.flip_triangles();

			ModelVolume* volume = object.add_volume(std::move(triangle_mesh));
            // stores the volume matrix taken from the metadata, if present
            if (has_transform)
                volume->source.transform = Slic3r::Geometry::Transformation(volume_matrix_to_object);
            volume->calculate_convex_hull();

            // recreate custom supports, seam and mmu segmentation from previously loaded attribute
            volume->supported_facets.reserve(triangles_count);
            volume->seam_facets.reserve(triangles_count);
            volume->mmu_segmentation_facets.reserve(triangles_count);
            for (size_t i=0; i<triangles_count; ++i) {
                size_t index = volume_data.first_triangle_id + i;
                assert(index < geometry.custom_supports.size());
                assert(index < geometry.custom_seam.size());
                assert(index < geometry.mmu_segmentation.size());
                if (! geometry.custom_supports[index].empty())
                    volume->supported_facets.set_triangle_from_string(i, geometry.custom_supports[index]);
                if (! geometry.custom_seam[index].empty())
                    volume->seam_facets.set_triangle_from_string(i, geometry.custom_seam[index]);
                if (! geometry.mmu_segmentation[index].empty())
                    volume->mmu_segmentation_facets.set_triangle_from_string(i, geometry.mmu_segmentation[index]);
            }
            volume->supported_facets.shrink_to_fit();
            volume->seam_facets.shrink_to_fit();
            volume->mmu_segmentation_facets.shrink_to_fit();

            volume->set_type(volume_data.part_type);

            // apply the remaining volume's metadata
            for (const Metadata& metadata : volume_data.metadata) {
                if (metadata.key == NAME_KEY)
                    volume->name = metadata.value;
                //else if ((metadata.key == MODIFIER_KEY) && (metadata.value == "1"))
				//	volume->set_type(ModelVolumeType::PARAMETER_MODIFIER);
				//for old format
                else if ((metadata.key == VOLUME_TYPE_KEY) || (metadata.key == PART_TYPE_KEY))
                    volume->set_type(ModelVolume::type_from_string(metadata.value));
                else if (metadata.key == SOURCE_FILE_KEY)
                    volume->source.input_file = metadata.value;
                else if (metadata.key == SOURCE_OBJECT_ID_KEY)
                    volume->source.object_idx = ::atoi(metadata.value.c_str());
                else if (metadata.key == SOURCE_VOLUME_ID_KEY)
                    volume->source.volume_idx = ::atoi(metadata.value.c_str());
                else if (metadata.key == SOURCE_OFFSET_X_KEY)
                    volume->source.mesh_offset(0) = ::atof(metadata.value.c_str());
                else if (metadata.key == SOURCE_OFFSET_Y_KEY)
                    volume->source.mesh_offset(1) = ::atof(metadata.value.c_str());
                else if (metadata.key == SOURCE_OFFSET_Z_KEY)
                    volume->source.mesh_offset(2) = ::atof(metadata.value.c_str());
                else if (metadata.key == SOURCE_IN_INCHES)
                    volume->source.is_converted_from_inches = metadata.value == "1";
                else if (metadata.key == SOURCE_IN_METERS)
                    volume->source.is_converted_from_meters = metadata.value == "1";
                else
                    volume->config.set_deserialize(metadata.key, metadata.value, config_substitutions);
            }

            // this may happen for 3mf saved by 3rd part softwares
            if (volume->name.empty()) {
                volume->name = object.name;
                if (renamed_volumes_count > 0)
                    volume->name += "_" + std::to_string(renamed_volumes_count + 1);
                ++renamed_volumes_count;
            }
        }

        return true;
    }
    */
    void XMLCALL _BBS_3MF_Importer::_handle_start_model_xml_element(void* userData, const char* name, const char** attributes)
    {
        _BBS_3MF_Importer* importer = (_BBS_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_start_model_xml_element(name, attributes);
    }

    void XMLCALL _BBS_3MF_Importer::_handle_end_model_xml_element(void* userData, const char* name)
    {
        _BBS_3MF_Importer* importer = (_BBS_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_end_model_xml_element(name);
    }

    void XMLCALL _BBS_3MF_Importer::_handle_xml_characters(void* userData, const XML_Char* s, int len)
    {
        _BBS_3MF_Importer* importer = (_BBS_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_xml_characters(s, len);
    }

    void XMLCALL _BBS_3MF_Importer::_handle_start_config_xml_element(void* userData, const char* name, const char** attributes)
    {
        _BBS_3MF_Importer* importer = (_BBS_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_start_config_xml_element(name, attributes);
    }

    void XMLCALL _BBS_3MF_Importer::_handle_end_config_xml_element(void* userData, const char* name)
    {
        _BBS_3MF_Importer* importer = (_BBS_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_end_config_xml_element(name);
    }


    /* functions of ObjectImporter */
    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_model(const char** attributes, unsigned int num_attributes)
    {
        object_unit_factor = bbs_get_unit_factor(bbs_get_attribute_value_string(attributes, num_attributes, UNIT_ATTR));

        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_model()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_resources(const char** attributes, unsigned int num_attributes)
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_resources()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_object(const char** attributes, unsigned int num_attributes)
    {
        // reset current object data
        if (current_object) {
            delete current_object;
            current_object = nullptr;
        }

        std::string object_type = bbs_get_attribute_value_string(attributes, num_attributes, TYPE_ATTR);

        if (bbs_is_valid_object_type(object_type)) {
            if (!current_object) {
                current_object = new CurrentObject();
            }

            current_object->id = bbs_get_attribute_value_int(attributes, num_attributes, ID_ATTR);
            current_object->name = bbs_get_attribute_value_string(attributes, num_attributes, NAME_ATTR);

            current_object->uuid = bbs_get_attribute_value_string(attributes, num_attributes, PUUID_ATTR);
            if (current_object->uuid.empty()) {
                current_object->uuid = bbs_get_attribute_value_string(attributes, num_attributes, PUUID_LOWER_ATTR);
            }
            current_object->pid = bbs_get_attribute_value_int(attributes, num_attributes, PID_ATTR);
        }

        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_object()
    {
        if (!current_object || (current_object->id == -1)) {
            top_importer->add_error("Found invalid object for "+ object_path);
            return false;
        }
        else {
            if (is_bbl_3mf && boost::ends_with(current_object->uuid, OBJECT_UUID_SUFFIX) && top_importer->m_load_restore) {
                std::istringstream iss(current_object->uuid);
                int backup_id;
                bool need_replace = false;
                if (iss >> std::hex >> backup_id) {
                    need_replace = (current_object->id != backup_id);
                    current_object->id = backup_id;
                }
                //if (need_replace)
                {
                    for (int index = 0; index < current_object->components.size(); index++)
                    {
                        int temp_id = (index + 1) << 16 | backup_id;
                        Component& component = current_object->components[index];
                        std::string new_path = component.object_id.first;
                        Id new_id = std::make_pair(new_path, temp_id);
                        IdToCurrentObjectMap::iterator object_it = object_list.find(component.object_id);
                        if (object_it != object_list.end()) {
                            CurrentObject new_object;
                            new_object.geometry = std::move(object_it->second.geometry);
                            new_object.id = temp_id;
                            new_object.model_object_idx = object_it->second.model_object_idx;
                            new_object.name = object_it->second.name;
                            new_object.uuid = object_it->second.uuid;

                            object_list.erase(object_it);
                            object_list.insert({ new_id, std::move(new_object) });
                        }
                        else {
                            top_importer->add_error("can not find object for component, id=" + std::to_string(component.object_id.second));
                            delete current_object;
                            current_object = nullptr;
                            return false;
                        }

                        component.object_id.second = temp_id;
                    }
                }
            }
            Id id = std::make_pair(object_path, current_object->id);
            if (object_list.find(id) == object_list.end()) {
                object_list.insert({ id, std::move(*current_object) });
                delete current_object;
                current_object = nullptr;
            }
            else {
                top_importer->add_error("Found object with duplicate id for "+object_path);
                delete current_object;
                current_object = nullptr;
                return false;
            }
        }

        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_color_group(const char **attributes, unsigned int num_attributes)
    {
        object_current_color_group = bbs_get_attribute_value_int(attributes, num_attributes, ID_ATTR);
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_color_group()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_color(const char **attributes, unsigned int num_attributes)
    {
        std::string color = bbs_get_attribute_value_string(attributes, num_attributes, COLOR_ATTR);
        object_group_id_to_color[object_current_color_group] = color;
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_color()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_mesh(const char** attributes, unsigned int num_attributes)
    {
        // reset current geometry
        if (current_object)
            current_object->geometry.reset();
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_mesh()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_vertices(const char** attributes, unsigned int num_attributes)
    {
        // reset current vertices
        if (current_object)
            current_object->geometry.vertices.clear();
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_vertices()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_vertex(const char** attributes, unsigned int num_attributes)
    {
        // appends the vertex coordinates
        // missing values are set equal to ZERO
        if (current_object)
            current_object->geometry.vertices.emplace_back(
                object_unit_factor * bbs_get_attribute_value_float(attributes, num_attributes, X_ATTR),
                object_unit_factor * bbs_get_attribute_value_float(attributes, num_attributes, Y_ATTR),
                object_unit_factor * bbs_get_attribute_value_float(attributes, num_attributes, Z_ATTR));
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_vertex()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_triangles(const char** attributes, unsigned int num_attributes)
    {
        // reset current triangles
        if (current_object)
            current_object->geometry.triangles.clear();
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_triangles()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_triangle(const char** attributes, unsigned int num_attributes)
    {
        // we are ignoring the following attributes:
        // p1
        // p2
        // p3
        // pid
        // see specifications

        // appends the triangle's vertices indices
        // missing values are set equal to ZERO
        if (current_object) {
            current_object->geometry.triangles.emplace_back(
                bbs_get_attribute_value_int(attributes, num_attributes, V1_ATTR),
                bbs_get_attribute_value_int(attributes, num_attributes, V2_ATTR),
                bbs_get_attribute_value_int(attributes, num_attributes, V3_ATTR));

            current_object->geometry.custom_supports.push_back(bbs_get_attribute_value_string(attributes, num_attributes, CUSTOM_SUPPORTS_ATTR));
            current_object->geometry.custom_seam.push_back(bbs_get_attribute_value_string(attributes, num_attributes, CUSTOM_SEAM_ATTR));
            current_object->geometry.mmu_segmentation.push_back(bbs_get_attribute_value_string(attributes, num_attributes, MMU_SEGMENTATION_ATTR));
            current_object->geometry.fuzzy_skin.push_back(bbs_get_attribute_value_string(attributes, num_attributes, CUSTOM_FUZZY_SKIN_ATTR));
            // BBS
            current_object->geometry.face_properties.push_back(bbs_get_attribute_value_string(attributes, num_attributes, FACE_PROPERTY_ATTR));
        }
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_triangle()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_components(const char** attributes, unsigned int num_attributes)
    {
        // reset current components
        if (current_object)
            current_object->components.clear();
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_components()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_component(const char** attributes, unsigned int num_attributes)
    {
        int object_id = bbs_get_attribute_value_int(attributes, num_attributes, OBJECTID_ATTR);
        Transform3d transform = bbs_get_transform_from_3mf_specs_string(bbs_get_attribute_value_string(attributes, num_attributes, TRANSFORM_ATTR));

        /*Id id = std::make_pair(m_sub_model_path, object_id);
        IdToModelObjectMap::iterator object_item = m_objects.find(id);
        if (object_item == m_objects.end()) {
            IdToAliasesMap::iterator alias_item = m_objects_aliases.find(id);
            if (alias_item == m_objects_aliases.end()) {
                add_error("Found component with invalid object id");
                return false;
            }
        }*/

        if (current_object) {
            Id id = std::make_pair(object_path, object_id);
            current_object->components.emplace_back(id, transform);
        }

        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_component()
    {
        // do nothing
        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_start_metadata(const char** attributes, unsigned int num_attributes)
    {
        obj_curr_metadata_name.clear();

        std::string name = bbs_get_attribute_value_string(attributes, num_attributes, NAME_ATTR);
        if (!name.empty()) {
            obj_curr_metadata_name = name;
        }

        return true;
    }

    bool _BBS_3MF_Importer::ObjectImporter::_handle_object_end_metadata()
    {
        if ((obj_curr_metadata_name == BBS_3MF_VERSION)||(obj_curr_metadata_name == BBS_3MF_VERSION1)) {
            is_bbl_3mf = true;
        }
        return true;
    }
    void _BBS_3MF_Importer::ObjectImporter::_handle_object_start_model_xml_element(const char* name, const char** attributes)
    {
        if (object_xml_parser == nullptr)
            return;

        bool res = true;
        unsigned int num_attributes = (unsigned int)XML_GetSpecifiedAttributeCount(object_xml_parser);

        if (::strcmp(MODEL_TAG, name) == 0)
            res = _handle_object_start_model(attributes, num_attributes);
        else if (::strcmp(RESOURCES_TAG, name) == 0)
            res = _handle_object_start_resources(attributes, num_attributes);
        else if (::strcmp(OBJECT_TAG, name) == 0)
            res = _handle_object_start_object(attributes, num_attributes);
        else if (::strcmp(COLOR_GROUP_TAG, name) == 0)
            res = _handle_object_start_color_group(attributes, num_attributes);
        else if (::strcmp(COLOR_TAG, name) == 0)
            res = _handle_object_start_color(attributes, num_attributes);
        else if (::strcmp(MESH_TAG, name) == 0)
            res = _handle_object_start_mesh(attributes, num_attributes);
        else if (::strcmp(VERTICES_TAG, name) == 0)
            res = _handle_object_start_vertices(attributes, num_attributes);
        else if (::strcmp(VERTEX_TAG, name) == 0)
            res = _handle_object_start_vertex(attributes, num_attributes);
        else if (::strcmp(TRIANGLES_TAG, name) == 0)
            res = _handle_object_start_triangles(attributes, num_attributes);
        else if (::strcmp(TRIANGLE_TAG, name) == 0)
            res = _handle_object_start_triangle(attributes, num_attributes);
        else if (::strcmp(COMPONENTS_TAG, name) == 0)
            res = _handle_object_start_components(attributes, num_attributes);
        else if (::strcmp(COMPONENT_TAG, name) == 0)
            res = _handle_object_start_component(attributes, num_attributes);
        else if (::strcmp(METADATA_TAG, name) == 0)
            res = _handle_object_start_metadata(attributes, num_attributes);

        if (!res)
            _stop_object_xml_parser();
    }

    void _BBS_3MF_Importer::ObjectImporter::_handle_object_end_model_xml_element(const char* name)
    {
        if (object_xml_parser == nullptr)
            return;

        bool res = true;

        if (::strcmp(MODEL_TAG, name) == 0)
            res = _handle_object_end_model();
        else if (::strcmp(RESOURCES_TAG, name) == 0)
            res = _handle_object_end_resources();
        else if (::strcmp(OBJECT_TAG, name) == 0)
            res = _handle_object_end_object();
        else if (::strcmp(COLOR_GROUP_TAG, name) == 0)
            res = _handle_object_end_color_group();
        else if (::strcmp(COLOR_TAG, name) == 0)
            res = _handle_object_end_color();
        else if (::strcmp(MESH_TAG, name) == 0)
            res = _handle_object_end_mesh();
        else if (::strcmp(VERTICES_TAG, name) == 0)
            res = _handle_object_end_vertices();
        else if (::strcmp(VERTEX_TAG, name) == 0)
            res = _handle_object_end_vertex();
        else if (::strcmp(TRIANGLES_TAG, name) == 0)
            res = _handle_object_end_triangles();
        else if (::strcmp(TRIANGLE_TAG, name) == 0)
            res = _handle_object_end_triangle();
        else if (::strcmp(COMPONENTS_TAG, name) == 0)
            res = _handle_object_end_components();
        else if (::strcmp(COMPONENT_TAG, name) == 0)
            res = _handle_object_end_component();
        else if (::strcmp(METADATA_TAG, name) == 0)
            res = _handle_object_end_metadata();

        if (!res)
            _stop_object_xml_parser();
    }

    void _BBS_3MF_Importer::ObjectImporter::_handle_object_xml_characters(const XML_Char* s, int len)
    {
        obj_curr_characters.append(s, len);
    }

    void XMLCALL _BBS_3MF_Importer::ObjectImporter::_handle_object_start_model_xml_element(void* userData, const char* name, const char** attributes)
    {
        ObjectImporter* importer = (ObjectImporter*)userData;
        if (importer != nullptr)
            importer->_handle_object_start_model_xml_element(name, attributes);
    }

    void XMLCALL _BBS_3MF_Importer::ObjectImporter::_handle_object_end_model_xml_element(void* userData, const char* name)
    {
        ObjectImporter* importer = (ObjectImporter*)userData;
        if (importer != nullptr)
            importer->_handle_object_end_model_xml_element(name);
    }

    void XMLCALL _BBS_3MF_Importer::ObjectImporter::_handle_object_xml_characters(void* userData, const XML_Char* s, int len)
    {
        ObjectImporter* importer = (ObjectImporter*)userData;
        if (importer != nullptr)
            importer->_handle_object_xml_characters(s, len);
    }

    bool _BBS_3MF_Importer::ObjectImporter::_extract_object_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
    {
        if (stat.m_uncomp_size == 0) {
            top_importer->add_error("Found invalid size for "+object_path);
            return false;
        }

        object_xml_parser = XML_ParserCreate(nullptr);
        if (object_xml_parser == nullptr) {
            top_importer->add_error("Unable to create parser for "+object_path);
            return false;
        }

        XML_SetUserData(object_xml_parser, (void*)this);
        XML_SetElementHandler(object_xml_parser, _BBS_3MF_Importer::ObjectImporter::_handle_object_start_model_xml_element, _BBS_3MF_Importer::ObjectImporter::_handle_object_end_model_xml_element);
        XML_SetCharacterDataHandler(object_xml_parser, _BBS_3MF_Importer::ObjectImporter::_handle_object_xml_characters);
        XML_SetEntityDeclHandler(object_xml_parser, nullptr);
        XML_SetExternalEntityRefHandler(object_xml_parser, nullptr);

        struct CallbackData
        {
            XML_Parser& parser;
            _BBS_3MF_Importer::ObjectImporter& importer;
            const mz_zip_archive_file_stat& stat;

            CallbackData(XML_Parser& parser, _BBS_3MF_Importer::ObjectImporter& importer, const mz_zip_archive_file_stat& stat) : parser(parser), importer(importer), stat(stat) {}
        };

        CallbackData data(object_xml_parser, *this, stat);

        mz_bool res = 0;

        try
        {
            mz_file_write_func callback = [](void* pOpaque, mz_uint64 file_ofs, const void* pBuf, size_t n)->size_t {
                CallbackData* data = (CallbackData*)pOpaque;
                if (!XML_Parse(data->parser, (const char*)pBuf, (int)n, (file_ofs + n == data->stat.m_uncomp_size) ? 1 : 0) || data->importer.object_parse_error()) {
                    char error_buf[1024];
                    ::snprintf(error_buf, 1024, "Error (%s) while parsing '%s' at line %d", data->importer.object_parse_error_message(), data->stat.m_filename, (int)XML_GetCurrentLineNumber(data->parser));
                    throw Slic3r::FileIOError(error_buf);
                }
                return n;
            };
            void* opaque = &data;
            res = mz_zip_reader_extract_to_callback(&archive, stat.m_file_index, callback, opaque, 0);
        }
        catch (const version_error& e)
        {
            // rethrow the exception
            std::string error_message = std::string(e.what()) + " for " + object_path;
            throw Slic3r::FileIOError(error_message);
        }
        catch (std::exception& e)
        {
            std::string error_message = std::string(e.what()) + " for " + object_path;
            top_importer->add_error(error_message);
            return false;
        }

        if (res == 0) {
            top_importer->add_error("Error while extracting model data from zip archive for "+object_path);
            return false;
        }

        return true;
    }


    class _BBS_3MF_Exporter : public _BBS_3MF_Base
    {
        struct BuildItem
        {
            std::string path;
            unsigned int id;
            Transform3d transform;
            bool printable;

            BuildItem(std::string const & path, unsigned int id, const Transform3d& transform, const bool printable)
                : path(path)
                , id(id)
                , transform(transform)
                , printable(printable)
            {
            }
        };

        //BBS: change volume to seperate objects
        /*struct Offsets
        {
            unsigned int first_vertex_id;
            unsigned int first_triangle_id;
            unsigned int last_triangle_id;

            Offsets(unsigned int first_vertex_id)
                : first_vertex_id(first_vertex_id)
                , first_triangle_id(-1)
                , last_triangle_id(-1)
            {
            }
        };*/

        //typedef std::map<const ModelVolume*, Offsets> VolumeToOffsetsMap;
        typedef std::map<const ModelVolume*, int> VolumeToObjectIDMap;

        struct ObjectData
        {
            ModelObject const * object;
            int backup_id;
            int object_id = 0;
            std::string sub_path;
            bool share_mesh = false;
            VolumeToObjectIDMap volumes_objectID;
        };

        typedef std::vector<BuildItem> BuildItemsList;
        typedef std::map<ModelObject const *, ObjectData> ObjectToObjectDataMap;

        bool m_fullpath_sources{ false };
        bool m_zip64 { true };
        bool m_production_ext { false };    // save with Production Extention
        bool m_skip_static{ false };        // not save mesh and other big static contents
        bool m_from_backup_save{ false };   // the object save is from backup store
        bool m_split_model { false };       // save object per file with Production Extention
        bool m_save_gcode { false };        // whether to save gcode for normal save
        bool m_skip_model { false };        // skip model when exporting .gcode.3mf
        bool m_skip_auxiliary { false };    // skip normal axuiliary files
        bool m_use_loaded_id { false };        // whether to use loaded id for identify_id
        bool m_share_mesh { false };        // whether to share mesh between objects
        std::string m_thumbnail_middle = PRINTER_THUMBNAIL_MIDDLE_FILE;
        std::string m_thumbnail_small  = PRINTER_THUMBNAIL_SMALL_FILE;
        std::map<void const *, std::pair<ObjectData*, ModelVolume const *>> m_shared_meshes;
        std::map<ModelVolume const *, std::pair<std::string, int>> m_volume_paths;
    public:
        //BBS: add plate data related logic

        // add backup logic
        //bool save_model_to_file(const std::string& filename, Model& model, PlateDataPtrs& plate_data_list, std::vector<Preset*>& project_presets, const DynamicPrintConfig* config, bool fullpath_sources, const std::vector<ThumbnailData*>& thumbnail_data, bool zip64, bool skip_static, Export3mfProgressFn proFn = nullptr, bool silence = false);

        bool save_model_to_file(StoreParams& store_params);
        // add backup logic
        bool save_object_mesh(const std::string& temp_path, ModelObject const & object, int obj_id);
        static void add_transformation(std::stringstream &stream, const Transform3d &tr);

    private:
        //BBS: add plate data related logic
        bool _save_model_to_file(const std::string& filename,
            Model& model, PlateDataPtrs& plate_data_list,
            std::vector<Preset*>& project_presets,
            const DynamicPrintConfig* config,
            const std::vector<ThumbnailData*>& thumbnail_data,
            const std::vector<ThumbnailData *>& no_light_thumbnail_data,
            const std::vector<ThumbnailData*>& top_thumbnail_data,
            const std::vector<ThumbnailData*>& pick_thumbnail_data,
            Export3mfProgressFn proFn,
            const std::vector<ThumbnailData*>& calibration_data,
            const std::vector<PlateBBoxData*>& id_bboxes,
            BBLProject* project = nullptr,
            int export_plate_idx = -1);

        bool _add_file_to_archive(mz_zip_archive& archive, const std::string & path_in_zip, const std::string & file_path);

        bool _add_content_types_file_to_archive(mz_zip_archive& archive);

        bool _add_thumbnail_file_to_archive(mz_zip_archive& archive, const ThumbnailData& thumbnail_data, const char* local_path, int index, bool generate_small_thumbnail = false);
        bool _add_calibration_file_to_archive(mz_zip_archive& archive, const ThumbnailData& thumbnail_data, int index);
        bool _add_bbox_file_to_archive(mz_zip_archive& archive, const PlateBBoxData& id_bboxes, int index);
        bool _add_relationships_file_to_archive(mz_zip_archive &                archive,
                                                std::string const &             from    = {},
                                                std::vector<std::string> const &targets = {},
                                                std::vector<std::string> const &types   = {},
                                                PackingTemporaryData            data    = PackingTemporaryData(),
                                                int export_plate_idx = -1) const;
        bool _add_model_file_to_archive(const std::string& filename, mz_zip_archive& archive, const Model& model, ObjectToObjectDataMap& objects_data, Export3mfProgressFn proFn = nullptr, BBLProject* project = nullptr) const;
        bool _add_object_to_model_stream(mz_zip_writer_staged_context &context, ObjectData const &object_data) const;
        void _add_object_components_to_stream(std::stringstream &stream, ObjectData const &object_data) const;
        //BBS: change volume to seperate objects
        bool _add_mesh_to_object_stream(std::function<bool(std::string &, bool)> const &flush, ObjectData const &object_data) const;
        bool _add_build_to_model_stream(std::stringstream& stream, const BuildItemsList& build_items) const;
        bool _add_layer_height_profile_file_to_archive(mz_zip_archive& archive, Model& model);
        bool _add_layer_config_ranges_file_to_archive(mz_zip_archive& archive, Model& model);
        bool _add_brim_ear_points_file_to_archive(mz_zip_archive& archive, Model& model);
        bool _add_sla_support_points_file_to_archive(mz_zip_archive& archive, Model& model);
        bool _add_sla_drain_holes_file_to_archive(mz_zip_archive& archive, Model& model);
        bool _add_print_config_file_to_archive(mz_zip_archive& archive, const DynamicPrintConfig &config);
        //BBS: add project config file logic for json format
        bool _add_project_config_file_to_archive(mz_zip_archive& archive, const DynamicPrintConfig &config, Model& model);
        //BBS: add project embedded preset files
        bool _add_project_embedded_presets_to_archive(mz_zip_archive& archive, Model& model, std::vector<Preset*> project_presets);
        bool _add_model_config_file_to_archive(mz_zip_archive& archive, const Model& model, PlateDataPtrs& plate_data_list, const ObjectToObjectDataMap &objects_data, const DynamicPrintConfig& config, int export_plate_idx = -1, bool save_gcode = true, bool use_loaded_id = false);
        bool _add_cut_information_file_to_archive(mz_zip_archive &archive, Model &model);
        bool _add_slice_info_config_file_to_archive(mz_zip_archive &archive, const Model &model, PlateDataPtrs &plate_data_list, const ObjectToObjectDataMap &objects_data, const DynamicPrintConfig& config);
        bool _add_gcode_file_to_archive(mz_zip_archive& archive, const Model& model, PlateDataPtrs& plate_data_list, Export3mfProgressFn proFn = nullptr);
        bool _add_custom_gcode_per_print_z_file_to_archive(mz_zip_archive& archive, Model& model, const DynamicPrintConfig* config);
        bool _add_auxiliary_dir_to_archive(mz_zip_archive &archive, const std::string &aux_dir, PackingTemporaryData &data);

        static int convert_instance_id_to_resource_id(const Model& model, int obj_id, int instance_id)
        {
            int resource_id = 1;

            for (int i = 0; i < obj_id; ++i)
            {
                resource_id += model.objects[i]->volumes.size() + 1;
            }

            resource_id += model.objects[obj_id]->volumes.size();

            return resource_id;
        }
    };

    bool _BBS_3MF_Exporter::save_model_to_file(StoreParams& store_params)
    {
        clear_errors();
        m_fullpath_sources = store_params.strategy & SaveStrategy::FullPathSources;
        m_zip64 = store_params.strategy & SaveStrategy::Zip64;
        m_production_ext = store_params.strategy & SaveStrategy::ProductionExt;

        m_skip_static = store_params.strategy & SaveStrategy::SkipStatic;
        m_split_model = store_params.strategy & SaveStrategy::SplitModel;
        m_save_gcode = store_params.strategy & SaveStrategy::WithGcode;
        m_skip_model  = store_params.strategy & SaveStrategy::SkipModel;
        m_skip_auxiliary = store_params.strategy & SaveStrategy::SkipAuxiliary;
        m_share_mesh       = store_params.strategy & SaveStrategy::ShareMesh;
        m_from_backup_save = store_params.strategy & SaveStrategy::Backup;

        m_use_loaded_id = store_params.strategy & SaveStrategy::UseLoadedId;

        if (auto info = store_params.model->model_info) {
            if (auto iter = info->metadata_items.find("Thumbnail_Small"); iter != info->metadata_items.end())
                m_thumbnail_small = iter->second;
            if (auto iter = info->metadata_items.find("Thumbnail_Middle"); iter != info->metadata_items.end())
                m_thumbnail_middle = iter->second;
        }
        boost::system::error_code ec;
        std::string filename = std::string(store_params.path);
        boost::filesystem::remove(filename + ".tmp", ec);

        bool result = _save_model_to_file(filename + ".tmp", *store_params.model, store_params.plate_data_list, store_params.project_presets, store_params.config,
                                          store_params.thumbnail_data, store_params.no_light_thumbnail_data, store_params.top_thumbnail_data, store_params.pick_thumbnail_data,
                                          store_params.proFn,
            store_params.calibration_thumbnail_data, store_params.id_bboxes, store_params.project, store_params.export_plate_idx);
        if (result) {
            boost::filesystem::rename(filename + ".tmp", filename, ec);
            if (ec) {
                add_error("Failed to rename file: " + ec.message());
                boost::filesystem::remove(filename + ".tmp", ec);
                return false;
            }
            if (!(store_params.strategy & SaveStrategy::Silence))
                save_string_file(store_params.model->get_backup_path() + "/origin.txt", filename);
        }
        return result;
    }

    // backup mesh-only
    bool _BBS_3MF_Exporter::save_object_mesh(const std::string& temp_path, ModelObject const & object, int obj_id)
    {
        m_production_ext = true;
        m_from_backup_save = true;
        Model const & model = *object.get_model();

        mz_zip_archive archive;
        mz_zip_zero_struct(&archive);

        auto filename = boost::format("3D/Objects/%s_%d.model") % object.name % obj_id;
        std::string filepath = temp_path + "/" + filename.str();
        std::string filepath_tmp = filepath + ".tmp";
        boost::system::error_code ec;
        boost::filesystem::remove(filepath_tmp, ec);
        if (!open_zip_writer(&archive, filepath_tmp)) {
            add_error("Unable to open the file"+filepath_tmp);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to open the file\n");
            return false;
        }

        struct close_lock
        {
            mz_zip_archive & archive;
            std::string const * filename;
            void close() {
                close_zip_writer(&archive);
                filename = nullptr;
            }
            ~close_lock() {
                if (filename) {
                    close_zip_writer(&archive);
                    boost::system::error_code ec;
                    boost::filesystem::remove(*filename, ec);
                }
            }
        } lock{archive, &filepath_tmp};

        ObjectToObjectDataMap objects_data;
        auto & volumes_objectID = objects_data.insert({&object, {&object, obj_id}}).first->second.volumes_objectID;
        unsigned int volume_count = 0;
        for (ModelVolume *volume : object.volumes) {
            if (volume == nullptr) continue;
            volumes_objectID.insert({volume, (++volume_count << 16 | obj_id)});
        }

        _add_model_file_to_archive(filename.str(), archive, model, objects_data);

        mz_zip_writer_finalize_archive(&archive);
        lock.close();
        boost::filesystem::rename(filepath_tmp, filepath, ec);
        return true;
    }

    //BBS: add plate data related logic
    bool _BBS_3MF_Exporter::_save_model_to_file(const std::string& filename,
        Model& model,
        PlateDataPtrs& plate_data_list,
        std::vector<Preset*>& project_presets,
        const DynamicPrintConfig* config,
        const std::vector<ThumbnailData*>& thumbnail_data,
        const std::vector<ThumbnailData*>& no_light_thumbnail_data,
        const std::vector<ThumbnailData*>& top_thumbnail_data,
        const std::vector<ThumbnailData*>& pick_thumbnail_data,
        Export3mfProgressFn proFn,
        const std::vector<ThumbnailData*>& calibration_data,
        const std::vector<PlateBBoxData*>& id_bboxes,
        BBLProject* project,
        int export_plate_idx)
    {
        PackingTemporaryData temp_data;

        mz_zip_archive archive;
        mz_zip_zero_struct(&archive);

        bool cb_cancel = false;

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ <<
                boost::format(",before open zip writer, m_skip_static %1%, m_save_gcode %2%, m_use_loaded_id %3%")%m_skip_static %m_save_gcode %m_use_loaded_id;
        if (proFn) {
            proFn(EXPORT_STAGE_OPEN_3MF, 0, 1, cb_cancel);
            if (cb_cancel)
                return false;
        }

        if (!open_zip_writer(&archive, filename)) {
            add_error("Unable to open the file"+filename);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to open the file\n");
            return false;
        }

        struct close_lock
        {
            mz_zip_archive & archive;
            std::string const * filename;
            ~close_lock() {
                close_zip_writer(&archive);
                if (filename) {
                    boost::system::error_code ec;
                    boost::filesystem::remove(*filename, ec);
                }
            }
        } lock{ archive, &filename};

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", before add _add_content_types_file_to_archive\n");
        if (proFn) {
            proFn(EXPORT_STAGE_CONTENT_TYPES, 0, 1, cb_cancel);
            if (cb_cancel)
                return false;
        }

        // Adds content types file ("[Content_Types].xml";).
        // The content of this file is the same for each OrcaSlicer 3mf.
        if (!_add_content_types_file_to_archive(archive)) {
            return false;
        }

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(",before add thumbnails, count %1%") % thumbnail_data.size();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(",top&&pick thumbnails, count %1%")%top_thumbnail_data.size();

        //BBS: add thumbnail for each plate
        if (!m_skip_static) {
            std::vector<bool> thumbnail_status(plate_data_list.size(), false);
            std::vector<bool> no_light_thumbnail_status(plate_data_list.size(), false);
            std::vector<bool> top_thumbnail_status(plate_data_list.size(), false);
            std::vector<bool> pick_thumbnail_status(plate_data_list.size(), false);

            if ((thumbnail_data.size() > 0)&&(thumbnail_data.size() > plate_data_list.size())) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", thumbnail_data size %1% > plate count %2%")
                    % thumbnail_data.size() %plate_data_list.size();
                return false;
            }
            if ((no_light_thumbnail_data.size() > 0) && (no_light_thumbnail_data.size() > plate_data_list.size())) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", no_light_thumbnail_data size %1% > plate count %2%") %
                    no_light_thumbnail_data.size() % plate_data_list.size();
                return false;
            }
            if ((top_thumbnail_data.size() > 0)&&(top_thumbnail_data.size() > plate_data_list.size())) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", top_thumbnail_data size %1% > plate count %2%")
                    % top_thumbnail_data.size() %plate_data_list.size();
                return false;
            }
            if ((pick_thumbnail_data.size() > 0)&&(pick_thumbnail_data.size() > plate_data_list.size())) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", pick_thumbnail_data size %1% > plate count %2%")
                    % pick_thumbnail_data.size() %plate_data_list.size();
                return false;
            }
            if (top_thumbnail_data.size() != pick_thumbnail_data.size()) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", top_thumbnail_data size %1% != pick_thumbnail_data size %2%")
                    % top_thumbnail_data.size() %pick_thumbnail_data.size();
                return false;
            }

            if (proFn) {
                proFn(EXPORT_STAGE_ADD_THUMBNAILS, 0, plate_data_list.size(), cb_cancel);
                if (cb_cancel)
                    return false;
            }

            for (unsigned int index = 0; index < thumbnail_data.size(); index++)
            {
                if (thumbnail_data[index]->is_valid())
                {
                    if (!_add_thumbnail_file_to_archive(archive, *thumbnail_data[index], "Metadata/plate", index, true)) {
                        return false;
                    }

                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(",add thumbnail %1%'s data into 3mf")%(index+1);
                    thumbnail_status[index] = true;
                }
            }

            for (unsigned int index = 0; index < no_light_thumbnail_data.size(); index++) {
                if (no_light_thumbnail_data[index]->is_valid()) {
                    if (!_add_thumbnail_file_to_archive(archive, *no_light_thumbnail_data[index], "Metadata/plate_no_light", index)) {
                        return false;
                    }

                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(",add no light thumbnail %1%'s data into 3mf") % (index + 1);
                    thumbnail_status[index] = true;
                }
            }
            // Adds the file Metadata/top_i.png and Metadata/pick_i.png
            for (unsigned int index = 0; index < top_thumbnail_data.size(); index++)
            {
                if (top_thumbnail_data[index]->is_valid())
                {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(",add top thumbnail %1%'s data into 3mf")%(index+1);
                    if (!_add_thumbnail_file_to_archive(archive, *top_thumbnail_data[index], "Metadata/top", index)) {
                        return false;
                    }
                    top_thumbnail_status[index] = true;
                }

                if (pick_thumbnail_data[index]->is_valid())
                {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(",add pick thumbnail %1%'s data into 3mf")%(index+1);
                    if (!_add_thumbnail_file_to_archive(archive, *pick_thumbnail_data[index], "Metadata/pick", index)) {
                        return false;
                    }
                    pick_thumbnail_status[index] = true;
                }
            }

            for (int i = 0; i < plate_data_list.size(); i++) {
                PlateData *plate_data = plate_data_list[i];

                if (!thumbnail_status[i] && !plate_data->thumbnail_file.empty() && (boost::filesystem::exists(plate_data->thumbnail_file))){
                    std::string dst_in_3mf = (boost::format("Metadata/plate_%1%.png") % (i + 1)).str();
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", add thumbnail %1% from file %2%") % (i+1) %plate_data->thumbnail_file;

                    if (!_add_file_to_archive(archive, dst_in_3mf, plate_data->thumbnail_file)) {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", add thumbnail %1% from file %2% failed\n") % (i+1) %plate_data->thumbnail_file;
                        return false;
                    }
                }

                if (!no_light_thumbnail_status[i] && !plate_data->no_light_thumbnail_file.empty() && (boost::filesystem::exists(plate_data->no_light_thumbnail_file))){
                    std::string dst_in_3mf = (boost::format("Metadata/plate_no_light_%1%.png") % (i + 1)).str();
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", add no light thumbnail %1% from file %2%") % (i+1) %plate_data->no_light_thumbnail_file;

                    if (!_add_file_to_archive(archive, dst_in_3mf, plate_data->no_light_thumbnail_file)) {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", add no light thumbnail %1% from file %2% failed\n") % (i+1) %plate_data->no_light_thumbnail_file;
                        return false;
                    }
                }

                if (!top_thumbnail_status[i] && !plate_data->top_file.empty() && (boost::filesystem::exists(plate_data->top_file))){
                    std::string dst_in_3mf = (boost::format("Metadata/top_%1%.png") % (i + 1)).str();

                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", add top thumbnail %1% from file %2%") % (i+1) %plate_data->top_file;
                    if (!_add_file_to_archive(archive, dst_in_3mf, plate_data->top_file)) {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", add top thumbnail %1% failed") % (i+1);
                        return false;
                    }
                    top_thumbnail_status[i] = true;
                }

                if (!pick_thumbnail_status[i] && !plate_data->pick_file.empty() && (boost::filesystem::exists(plate_data->pick_file))){
                    std::string dst_in_3mf = (boost::format("Metadata/pick_%1%.png") % (i + 1)).str();

                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", add pick thumbnail %1% from file %2%") % (i+1) %plate_data->pick_file;
                    if (!_add_file_to_archive(archive, dst_in_3mf, plate_data->pick_file)) {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", add pick thumbnail %1% failed") % (i+1);
                        return false;
                    }
                    pick_thumbnail_status[i] = true;
                }
            }
            if (proFn) {
                proFn(EXPORT_STAGE_ADD_THUMBNAILS, plate_data_list.size(), plate_data_list.size(), cb_cancel);
                if (cb_cancel)
                    return false;
            }
        }

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(",before add calibration thumbnails, count %1%\n")%calibration_data.size();
        //BBS add calibration thumbnail for each plate
        if (!m_skip_static && calibration_data.size() > 0) {
            // Adds the file Metadata/calibration_p[X].png.
            for (unsigned int index = 0; index < calibration_data.size(); index++)
            {
                if (proFn) {
                    proFn(EXPORT_STAGE_ADD_THUMBNAILS, index, calibration_data.size(), cb_cancel);
                    if (cb_cancel)
                        return false;
                }

                if (calibration_data[index]->is_valid())
                {
                    if (!_add_calibration_file_to_archive(archive, *calibration_data[index], index)) {
                        close_zip_writer(&archive);
                        return false;
                    }
                }
            }
        }

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(",before add calibration boundingbox, count %1%\n")%id_bboxes.size();
        if (!m_skip_static && id_bboxes.size() > 0) {
            // Adds the file Metadata/calibration_p[X].png.
            for (unsigned int index = 0; index < id_bboxes.size(); index++)
            {
                // BBS: save bounding box to json
                if (id_bboxes[index]->is_valid()) {
                    if (!_add_bbox_file_to_archive(archive, *id_bboxes[index], index)) {
                        close_zip_writer(&archive);
                        return false;
                    }
                }
            }
        }

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", before add models\n");
        if (proFn) {
            proFn(EXPORT_STAGE_ADD_MODELS, 0, 1, cb_cancel);
            if (cb_cancel)
                return false;
        }

        // Adds model file ("3D/3dmodel.model").
        // This is the one and only file that contains all the geometry (vertices and triangles) of all ModelVolumes.
        ObjectToObjectDataMap objects_data;
        //if (!m_skip_model)
        {
            if (!_add_model_file_to_archive(filename, archive, model, objects_data, proFn, project)) { return false; }

            // Adds layer height profile file ("Metadata/Slic3r_PE_layer_heights_profile.txt").
            // All layer height profiles of all ModelObjects are stored here, indexed by 1 based index of the ModelObject in Model.
            // The index differes from the index of an object ID of an object instance of a 3MF file!
            if (!_add_layer_height_profile_file_to_archive(archive, model)) {
                close_zip_writer(&archive);
                return false;
            }

            // BBS progress point
            /*BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format("export 3mf EXPORT_STAGE_ADD_LAYER_RANGE\n");
            if (proFn) {
                proFn(EXPORT_STAGE_ADD_LAYER_RANGE, 0, 1, cb_cancel);
                if (cb_cancel)
                    return false;
            }*/

            // Adds layer config ranges file ("Metadata/Slic3r_PE_layer_config_ranges.txt").
            // All layer height profiles of all ModelObjects are stored here, indexed by 1 based index of the ModelObject in Model.
            // The index differes from the index of an object ID of an object instance of a 3MF file!
            if (!_add_layer_config_ranges_file_to_archive(archive, model)) {
                close_zip_writer(&archive);
                return false;
            }

            if (!_add_brim_ear_points_file_to_archive(archive, model)) {
                close_zip_writer(&archive);
                return false;
            }

            // BBS progress point
            /*BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format("export 3mf EXPORT_STAGE_ADD_SUPPORT\n");
            if (proFn) {
                proFn(EXPORT_STAGE_ADD_SUPPORT, 0, 1, cb_cancel);
                if (cb_cancel)
                    return false;
            }

            // Adds sla support points file ("Metadata/Slic3r_PE_sla_support_points.txt").
            // All  sla support points of all ModelObjects are stored here, indexed by 1 based index of the ModelObject in Model.
            // The index differes from the index of an object ID of an object instance of a 3MF file!
            if (!_add_sla_support_points_file_to_archive(archive, model)) {
                return false;
            }

            if (!_add_sla_drain_holes_file_to_archive(archive, model)) {
                return false;
            }*/

            // BBS progress point
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", before add custom gcodes\n");
            if (proFn) {
                proFn(EXPORT_STAGE_ADD_CUSTOM_GCODE, 0, 1, cb_cancel);
                if (cb_cancel) return false;
            }

            // Adds custom gcode per height file ("Metadata/Prusa_Slicer_custom_gcode_per_print_z.xml").
            // All custom gcode per height of whole Model are stored here
            if (!_add_custom_gcode_per_print_z_file_to_archive(archive, model, config)) { return false; }

            // BBS progress point
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", before add project_settings\n");
            if (proFn) {
                proFn(EXPORT_STAGE_ADD_PRINT_CONFIG, 0, 1, cb_cancel);
                if (cb_cancel) return false;
            }

            // Adds slic3r print config file ("Metadata/Slic3r_PE.config").
            // This file contains the content of FullPrintConfig / SLAFullPrintConfig.
            if (config != nullptr) {
                // BBS: change to json format
                // if (!_add_print_config_file_to_archive(archive, *config)) {
                if (!_add_project_config_file_to_archive(archive, *config, model)) { return false; }
            }

            // BBS progress point
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", before add project embedded settings\n");
            if (proFn) {
                proFn(EXPORT_STAGE_ADD_CONFIG_FILE, 0, 1, cb_cancel);
                if (cb_cancel) return false;
            }

            // BBS: add project config
            if (project_presets.size() > 0) {
                // BBS: add project embedded preset files
                _add_project_embedded_presets_to_archive(archive, model, project_presets);

                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", finished add project embedded settings, size %1%\n")%project_presets.size();
                if (proFn) {
                    proFn(EXPORT_STAGE_ADD_PROJECT_CONFIG, 0, 1, cb_cancel);
                    if (cb_cancel) return false;
                }
            }
        }

        // add plate_N.gcode.md5 to file
        if (!m_skip_static && m_save_gcode) {
            for (int i = 0; i < plate_data_list.size(); i++) {
                PlateData *plate_data = plate_data_list[i];
                if (!plate_data->gcode_file.empty() && plate_data->is_sliced_valid && boost::filesystem::exists(plate_data->gcode_file)) {
                    unsigned char digest[16];
                    MD5_CTX       ctx;
                    MD5_Init(&ctx);
                    auto                        src_gcode_file = plate_data->gcode_file;
                    boost::filesystem::ifstream ifs(src_gcode_file, std::ios::binary);
                    std::string                 buf(64 * 1024, 0);
                    const std::size_t &         size      = boost::filesystem::file_size(src_gcode_file);
                    std::size_t                 left_size = size;
                    while (ifs) {
                        ifs.read(buf.data(), buf.size());
                        int read_bytes = ifs.gcount();
                        MD5_Update(&ctx, (unsigned char *) buf.data(), read_bytes);
                    }
                    MD5_Final(digest, &ctx);
                    char md5_str[33];
                    for (int j = 0; j < 16; j++) { sprintf(&md5_str[j * 2], "%02X", (unsigned int) digest[j]); }
                    plate_data->gcode_file_md5 = std::string(md5_str);
                    std::string target_file    = (boost::format("Metadata/plate_%1%.gcode.md5") % (plate_data->plate_index + 1)).str();
                    if (!mz_zip_writer_add_mem(&archive, target_file.c_str(), (const void *) plate_data->gcode_file_md5.c_str(), plate_data->gcode_file_md5.length(),
                                               MZ_DEFAULT_COMPRESSION)) {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__
                                                 << boost::format(", store  gcode md5 to 3mf's %1%,  length %2%, failed\n") %target_file %plate_data->gcode_file_md5.length();
                        return false;
                    }
                }
            }
        }

        // Adds gcode files ("Metadata/plate_1.gcode, plate_2.gcode, ...)
        // Before _add_model_config_file_to_archive, because we modify plate_data
        //if (!m_skip_static && !_add_gcode_file_to_archive(archive, model, plate_data_list, proFn)) {
        if (!m_skip_static && m_save_gcode && !_add_gcode_file_to_archive(archive, model, plate_data_list, proFn)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", _add_gcode_file_to_archive failed\n");
            return false;
        }

        // Adds slic3r model config file ("Metadata/Slic3r_PE_model.config").
        // This file contains all the attributes of all ModelObjects and their ModelVolumes (names, parameter overrides).
        // As there is just a single Indexed Triangle Set data stored per ModelObject, offsets of volumes into their respective Indexed Triangle Set data
        // is stored here as well.
        if (!_add_model_config_file_to_archive(archive, model, plate_data_list, objects_data, *config, export_plate_idx, m_save_gcode, m_use_loaded_id)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", _add_model_config_file_to_archive failed\n");
            return false;
        }

        if (!_add_cut_information_file_to_archive(archive, model)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", _add_cut_information_file_to_archive failed\n");
            return false;
        }

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", before add sliced info to 3mf\n");
        if (proFn) {
            proFn(EXPORT_STAGE_ADD_SLICE_INFO, 0, 1, cb_cancel);
            if (cb_cancel)
                return false;
        }

        // Adds sliced info of plate file ("Metadata/slice_info.config")
        // This file contains all sliced info of all plates
        if (!_add_slice_info_config_file_to_archive(archive, model, plate_data_list, objects_data, *config)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", _add_slice_info_config_file_to_archive failed\n");
            return false;
        }

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", before add auxiliary dir to 3mf\n");
        if (proFn) {
            proFn(EXPORT_STAGE_ADD_AUXILIARIES, 0, 1, cb_cancel);
            if (cb_cancel)
                return false;
        }

        if (!m_skip_static && !_add_auxiliary_dir_to_archive(archive, model.get_auxiliary_file_temp_path(), temp_data)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", _add_auxiliary_dir_to_archive failed\n");
            return false;
        }

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", before add relation file to 3mf\n");
        if (proFn) {
            proFn(EXPORT_STAGE_ADD_RELATIONS, 0, 1, cb_cancel);
            if (cb_cancel)
                return false;
        }

        // Adds relationships file ("_rels/.rels").
        // The content of this file is the same for each OrcaSlicer 3mf.
        // The relationshis file contains a reference to the geometry file "3D/3dmodel.model", the name was chosen to be compatible with CURA.
        if (!_add_relationships_file_to_archive(archive, {}, {}, {}, temp_data, export_plate_idx)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", _add_relationships_file_to_archive failed\n");
            return false;
        }

        if (!mz_zip_writer_finalize_archive(&archive)) {
            add_error("Unable to finalize the archive");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to finalize the archive\n");
            return false;
        }

        //BBS progress point
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", finished exporting 3mf\n");
        if (proFn) {
            proFn(EXPORT_STAGE_FINISH, 0, 1, cb_cancel);
            if (cb_cancel)
                return false;
        }

        lock.filename = nullptr;

        return true;
    }

    bool _BBS_3MF_Exporter::_add_file_to_archive(mz_zip_archive& archive, const std::string& path_in_zip, const std::string& src_file_path)
    {
        static std::string const nocomp_exts[] = {".png", ".jpg", ".mp4", ".jpeg", ".zip", ".3mf"};
        auto end = nocomp_exts + sizeof(nocomp_exts) / sizeof(nocomp_exts[0]);
        bool nocomp = std::find_if(nocomp_exts, end, [&path_in_zip](auto & ext) { return boost::algorithm::ends_with(path_in_zip, ext); }) != end;
#if WRITE_ZIP_LANGUAGE_ENCODING
        bool result = mz_zip_writer_add_file(&archive, path_in_zip.c_str(), encode_path(src_file_path.c_str()).c_str(), NULL, 0, nocomp ? MZ_NO_COMPRESSION : MZ_DEFAULT_LEVEL);
#else
        std::string native_path = encode_path(path_in_zip.c_str());
        std::string extra = ZipUnicodePathExtraField::encode(path_in_zip, native_path);
        bool result = mz_zip_writer_add_file_ex(&archive, native_path.c_str(), encode_path(src_file_path.c_str()).c_str(), NULL, 0, nocomp ? MZ_ZIP_FLAG_ASCII_FILENAME : MZ_DEFAULT_COMPRESSION,
                extra.c_str(), extra.length(), extra.c_str(), extra.length());
#endif
        if (!result) {
            add_error("Unable to add file " + src_file_path + " to archive");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", Unable to add file %1% to archive %2%\n") % src_file_path % path_in_zip;
        } else {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", add file %1% to archive %2%\n") % src_file_path % path_in_zip;
        }
        return result;
    }

    bool _BBS_3MF_Exporter::_add_content_types_file_to_archive(mz_zip_archive& archive)
    {
        std::stringstream stream;
        stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        stream << "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n";
        stream << " <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n";
        stream << " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n";
        stream << " <Default Extension=\"png\" ContentType=\"image/png\"/>\n";
        stream << " <Default Extension=\"gcode\" ContentType=\"text/x.gcode\"/>\n";
        stream << "</Types>";

        std::string out = stream.str();

        if (!mz_zip_writer_add_mem(&archive, CONTENT_TYPES_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
            add_error("Unable to add content types file to archive");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add content types file to archive\n");
            return false;
        }

        return true;
    }

    bool _BBS_3MF_Exporter::_add_thumbnail_file_to_archive(mz_zip_archive& archive, const ThumbnailData& thumbnail_data, const char* local_path, int index, bool generate_small_thumbnail)
    {
        bool res = false;

        size_t png_size = 0;
        void* png_data = tdefl_write_image_to_png_file_in_memory_ex((const void*)thumbnail_data.pixels.data(), thumbnail_data.width, thumbnail_data.height, 4, &png_size, MZ_DEFAULT_COMPRESSION, 1);
        if (png_data != nullptr) {
            std::string thumbnail_name = (boost::format("%1%_%2%.png")%local_path % (index + 1)).str();
            res = mz_zip_writer_add_mem(&archive, thumbnail_name.c_str(), (const void*)png_data, png_size, MZ_NO_COMPRESSION);
            mz_free(png_data);
        }

        if (!res) {
            add_error("Unable to add thumbnail file to archive");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add thumbnail file to archive\n");
        }

        if (generate_small_thumbnail && thumbnail_data.is_valid()) {
            //generate small size of thumbnail
            std::vector<unsigned char> small_pixels;
            small_pixels.resize(PLATE_THUMBNAIL_SMALL_WIDTH * PLATE_THUMBNAIL_SMALL_HEIGHT * 4);
            /* step width and step height */
            int sw = thumbnail_data.width / PLATE_THUMBNAIL_SMALL_WIDTH;
            int sh = thumbnail_data.height / PLATE_THUMBNAIL_SMALL_HEIGHT;
            int clampped_width = sw * PLATE_THUMBNAIL_SMALL_WIDTH;
            int clampped_height = sh * PLATE_THUMBNAIL_SMALL_HEIGHT;

            for (int i = 0; i < clampped_height; i += sh) {
                for (int j = 0; j < clampped_width; j += sw) {
                    int r = 0, g = 0, b = 0, a = 0;
                    for (int m = 0; m < sh; m++) {
                        for (int n = 0; n < sw; n++) {
                            r += (int)thumbnail_data.pixels[4 * ((i + m) * thumbnail_data.width + j + n) + 0];
                            g += (int)thumbnail_data.pixels[4 * ((i + m) * thumbnail_data.width + j + n) + 1];
                            b += (int)thumbnail_data.pixels[4 * ((i + m) * thumbnail_data.width + j + n) + 2];
                            a += (int)thumbnail_data.pixels[4 * ((i + m) * thumbnail_data.width + j + n) + 3];
                        }
                    }
                    r = std::clamp(0, r / sw / sh, 255);
                    g = std::clamp(0, g / sw / sh, 255);
                    b = std::clamp(0, b / sw / sh, 255);
                    a = std::clamp(0, a / sw / sh, 255);
                    small_pixels[4 * (i / sw * PLATE_THUMBNAIL_SMALL_WIDTH + j / sh) + 0] = (unsigned char)r;
                    small_pixels[4 * (i / sw * PLATE_THUMBNAIL_SMALL_WIDTH + j / sh) + 1] = (unsigned char)g;
                    small_pixels[4 * (i / sw * PLATE_THUMBNAIL_SMALL_WIDTH + j / sh) + 2] = (unsigned char)b;
                    small_pixels[4 * (i / sw * PLATE_THUMBNAIL_SMALL_WIDTH + j / sh) + 3] = (unsigned char)a;
                    //memcpy((void*)&small_pixels[4*(i / sw * PLATE_THUMBNAIL_SMALL_WIDTH + j / sh)], thumbnail_data.pixels.data() + 4*(i * thumbnail_data.width + j), 4);
                }
            }
            size_t small_png_size = 0;
            void* small_png_data = tdefl_write_image_to_png_file_in_memory_ex((const void*)small_pixels.data(), PLATE_THUMBNAIL_SMALL_WIDTH, PLATE_THUMBNAIL_SMALL_HEIGHT, 4, &small_png_size, MZ_DEFAULT_COMPRESSION, 1);
            if (png_data != nullptr) {
                std::string thumbnail_name = (boost::format("%1%_%2%_small.png") % local_path % (index + 1)).str();
                res = mz_zip_writer_add_mem(&archive, thumbnail_name.c_str(), (const void*)small_png_data, small_png_size, MZ_NO_COMPRESSION);
                mz_free(small_png_data);
            }

            if (!res) {
                add_error("Unable to add small thumbnail file to archive");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add small thumbnail file to archive\n");
            }
        }

        return res;
    }

    bool _BBS_3MF_Exporter::_add_calibration_file_to_archive(mz_zip_archive& archive, const ThumbnailData& thumbnail_data, int index)
    {
        bool res = false;

        /*size_t png_size = 0;
        void* png_data = tdefl_write_image_to_png_file_in_memory_ex((const void*)thumbnail_data.pixels.data(), thumbnail_data.width, thumbnail_data.height, 4, &png_size, MZ_DEFAULT_COMPRESSION, 1);
        if (png_data != nullptr) {
            std::string thumbnail_name = (boost::format(PATTERN_FILE_FORMAT) % (index + 1)).str();
            res = mz_zip_writer_add_mem(&archive, thumbnail_name.c_str(), (const void*)png_data, png_size, MZ_NO_COMPRESSION);
            mz_free(png_data);
        }

        if (!res) {
            add_error("Unable to add thumbnail file to archive");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add thumbnail file to archive\n");
        }*/

        return res;
    }

    bool _BBS_3MF_Exporter::_add_bbox_file_to_archive(mz_zip_archive& archive, const PlateBBoxData& id_bboxes, int index)
    {
        bool res = false;
        nlohmann::json j;
        id_bboxes.to_json(j);
        std::string out = j.dump();

        std::string json_file_name = (boost::format(PATTERN_CONFIG_FILE_FORMAT) % (index + 1)).str();
        if (!mz_zip_writer_add_mem(&archive, json_file_name.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
            add_error("Unable to add json file to archive");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add json file to archive\n");
            return false;
        }

        return true;
    }

    bool _BBS_3MF_Exporter::_add_relationships_file_to_archive(
        mz_zip_archive &archive, std::string const &from, std::vector<std::string> const &targets, std::vector<std::string> const &types, PackingTemporaryData data, int export_plate_idx) const
    {
        std::stringstream stream;
        stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        stream << "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
        if (from.empty()) {
            stream << " <Relationship Target=\"/" << MODEL_FILE << "\" Id=\"rel-1\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n";

            if (export_plate_idx < 0) {
                //use cover image if have
                if (data._3mf_thumbnail.empty()) {
                    stream << " <Relationship Target=\"/Metadata/plate_1.png"
                               << "\" Id=\"rel-2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail\"/>\n";
                } else {
                    stream << " <Relationship Target=\"/" << xml_escape(data._3mf_thumbnail)
                           << "\" Id=\"rel-2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail\"/>\n";
                }

                if (data._3mf_printer_thumbnail_middle.empty()) {
                    stream << " <Relationship Target=\"/Metadata/plate_1.png"
                           << "\" Id=\"rel-4\" Type=\"http://schemas.bambulab.com/package/2021/cover-thumbnail-middle\"/>\n";
                } else {
                    stream << " <Relationship Target=\"/" << xml_escape(data._3mf_printer_thumbnail_middle)
                           << "\" Id=\"rel-4\" Type=\"http://schemas.bambulab.com/package/2021/cover-thumbnail-middle\"/>\n";
                }

                if (data._3mf_printer_thumbnail_small.empty()) {
                    stream << "<Relationship Target=\"/Metadata/plate_1_small.png"
                           << "\" Id=\"rel-5\" Type=\"http://schemas.bambulab.com/package/2021/cover-thumbnail-small\"/>\n";
                } else {
                    stream << " <Relationship Target=\"/" << xml_escape(data._3mf_printer_thumbnail_small)
                           << "\" Id=\"rel-5\" Type=\"http://schemas.bambulab.com/package/2021/cover-thumbnail-small\"/>\n";
                }
            }
            else {
                //always use plate thumbnails
                std::string thumbnail_file_str = (boost::format("Metadata/plate_%1%.png") % (export_plate_idx + 1)).str();
                stream << " <Relationship Target=\"/" << xml_escape(thumbnail_file_str)
                    << "\" Id=\"rel-2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail\"/>\n";

                thumbnail_file_str = (boost::format("Metadata/plate_%1%.png") % (export_plate_idx + 1)).str();
                stream << " <Relationship Target=\"/" << xml_escape(thumbnail_file_str)
                   << "\" Id=\"rel-4\" Type=\"http://schemas.bambulab.com/package/2021/cover-thumbnail-middle\"/>\n";

                thumbnail_file_str = (boost::format("Metadata/plate_%1%_small.png") % (export_plate_idx + 1)).str();
                stream << " <Relationship Target=\"/" << xml_escape(thumbnail_file_str)
                   << "\" Id=\"rel-5\" Type=\"http://schemas.bambulab.com/package/2021/cover-thumbnail-small\"/>\n";
            }
        }
        else if (targets.empty()) {
            return false;
        }
        else {
            int i = 0;
            for (auto & path : targets) {
                for (auto & type : types)
                    stream << " <Relationship Target=\"/" << xml_escape(path) << "\" Id=\"rel-" << std::to_string(++i) << "\" Type=\"" << type << "\"/>\n";
            }
        }
        stream << "</Relationships>";

        std::string out = stream.str();

        if (!mz_zip_writer_add_mem(&archive, from.empty() ? RELATIONSHIPS_FILE.c_str() : from.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
            add_error("Unable to add relationships file to archive");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add relationships file to archive\n");
            return false;
        }

        return true;
    }


    static void reset_stream(std::stringstream &stream)
    {
        stream.str("");
        stream.clear();
        // https://en.cppreference.com/w/cpp/types/numeric_limits/max_digits10
        // Conversion of a floating-point value to text and back is exact as long as at least max_digits10 were used (9 for float, 17 for double).
        // It is guaranteed to produce the same floating-point value, even though the intermediate text representation is not exact.
        // The default value of std::stream precision is 6 digits only!
        stream << std::setprecision(std::numeric_limits<float>::max_digits10);
    }

    /*
    * BBS: Production Extension (SplitModel)
    *   save sub model if objects_data is not empty
    *   not collect build items in sub model
    */
    bool _BBS_3MF_Exporter::_add_model_file_to_archive(const std::string& filename, mz_zip_archive& archive, const Model& model, ObjectToObjectDataMap& objects_data, Export3mfProgressFn proFn, BBLProject* project) const
    {
        bool sub_model = !objects_data.empty();
        bool write_object = sub_model || !m_split_model;

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", filename %1%, m_split_model %2%,  sub_model %3%")%filename % m_split_model % sub_model;

#if WRITE_ZIP_LANGUAGE_ENCODING
        auto & zip_filename = filename;
#else
        std::string zip_filename = encode_path(filename.c_str());
        std::string extra = sub_model ? ZipUnicodePathExtraField::encode(filename, zip_filename) : "";
#endif
        mz_zip_writer_staged_context context;
        if (!mz_zip_writer_add_staged_open(&archive, &context, sub_model ? zip_filename.c_str() : MODEL_FILE.c_str(),
            m_zip64 ?
                // Maximum expected and allowed 3MF file size is 16GiB.
                // This switches the ZIP file to a 64bit mode, which adds a tiny bit of overhead to file records.
                (uint64_t(1) << 30) * 16 :
                // Maximum expected 3MF file size is 4GB-1. This is a workaround for interoperability with Windows 10 3D model fixing API, see
                // GH issue #6193.
                (uint64_t(1) << 32) - 1,
#if WRITE_ZIP_LANGUAGE_ENCODING
            nullptr, nullptr, 0, MZ_DEFAULT_LEVEL, nullptr, 0, nullptr, 0)) {
#else
            nullptr, nullptr, 0, MZ_DEFAULT_COMPRESSION, extra.c_str(), extra.length(), extra.c_str(), extra.length())) {
#endif
            add_error("Unable to add model file to archive");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add model file to archive\n");
            return false;
        }


        {
            std::stringstream stream;
            reset_stream(stream);
            stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
            stream << "<" << MODEL_TAG << " unit=\"millimeter\" xml:lang=\"en-US\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\" xmlns:BambuStudio=\"http://schemas.bambulab.com/package/2021\"";
            if (m_production_ext)
                stream << " xmlns:p=\"http://schemas.microsoft.com/3dmanufacturing/production/2015/06\" requiredextensions=\"p\"";
            stream << ">\n";

            std::string origin;
            std::string name;
            std::string user_name;
            std::string user_id;
            std::string design_cover;
            std::string license;
            std::string description;
            std::string copyright;
            std::string rating;
            std::string model_id;
            std::string region_code;
            if (model.design_info) {
                 user_name = model.design_info->Designer;
                 user_id = model.design_info->DesignerUserId;
                 BOOST_LOG_TRIVIAL(trace) << "design_info, save_3mf found designer = " << user_name;
                 BOOST_LOG_TRIVIAL(trace) << "design_info, save_3mf found designer_user_id = " << user_id;
            }

            if (project) {
                model_id    = project->project_model_id;
                region_code = project->project_country_code;
            }

            if (model.model_info) {
                design_cover = model.model_info->cover_file;
                license      = model.model_info->license;
                description  = model.model_info->description;
                copyright    = model.model_info->copyright;
                name         = model.model_info->model_name;
                origin       = model.model_info->origin;
                BOOST_LOG_TRIVIAL(trace) << "design_info, save_3mf found designer_cover = " << design_cover;
            }
            // remember to use metadata_item_map to store metadata info
            std::map<std::string, std::string> metadata_item_map;
            if (!sub_model) {
                // update metadat_items
                if (model.model_info && model.model_info.get()) {
                    metadata_item_map = model.model_info.get()->metadata_items;
                }

                metadata_item_map[BBL_MODEL_NAME_TAG]           = xml_escape(name);
                metadata_item_map[BBL_ORIGIN_TAG]               = xml_escape(origin);
                metadata_item_map[BBL_DESIGNER_TAG]             = xml_escape(user_name);
                metadata_item_map[BBL_DESIGNER_USER_ID_TAG]     = ""; // Orca: PRIVACY: do not store BBL user id in 3mf
                metadata_item_map[BBL_DESIGNER_COVER_FILE_TAG]  = xml_escape(design_cover);
                metadata_item_map[BBL_DESCRIPTION_TAG]          = xml_escape(description);
                metadata_item_map[BBL_COPYRIGHT_NORMATIVE_TAG]  = xml_escape(copyright);
                metadata_item_map[BBL_LICENSE_TAG]              = xml_escape(license);

                /* save model info */
                if (!model_id.empty()) {
                    metadata_item_map[BBL_MODEL_ID_TAG] = model_id;
                    metadata_item_map[BBL_REGION_TAG]   = region_code;
                }

                // Orca: PRIVACY: do not store creation & modification date in 3mf
                metadata_item_map[BBL_CREATION_DATE_TAG] = "";
                metadata_item_map[BBL_MODIFICATION_TAG]  = "";
                //SoftFever: write BambuStudio tag to keep it compatible 
                metadata_item_map[BBL_APPLICATION_TAG] = (boost::format("%1%-%2%") % "BambuStudio" % SoftFever_VERSION).str();
            }
            metadata_item_map[BBS_3MF_VERSION] = std::to_string(VERSION_BBS_3MF);

            if (!model.mk_name.empty()) {
                metadata_item_map[BBL_MAKERLAB_TAG] = xml_escape(model.mk_name);
                BOOST_LOG_TRIVIAL(info) << "saved mk_name " << model.mk_name;
            }
            if (!model.mk_version.empty()) {
                metadata_item_map[BBL_MAKERLAB_VERSION_TAG] = xml_escape(model.mk_version);
                BOOST_LOG_TRIVIAL(info) << "saved mk_version " << model.mk_version;
            }
            if (!model.md_name.empty()) {
                for (unsigned int i = 0; i < model.md_name.size(); i++)
                {
                    BOOST_LOG_TRIVIAL(info) << boost::format("saved metadata_name %1%, metadata_value %2%") %model.md_name[i] %model.md_value[i];
                    metadata_item_map[model.md_name[i]] = xml_escape(model.md_value[i]);
                }
            }

            // store metadata info
            for (auto item : metadata_item_map) {
                BOOST_LOG_TRIVIAL(info) << "bbs_3mf: save key= " << item.first << ", value = " << item.second;
                stream << " <" << METADATA_TAG << " name=\"" << item.first << "\">"
                       << xml_escape(item.second) << "</" << METADATA_TAG << ">\n";
            }

            stream << " <" << RESOURCES_TAG << ">\n";
            std::string buf = stream.str();
            if (! buf.empty() && ! mz_zip_writer_add_staged_data(&context, buf.data(), buf.size())) {
                add_error("Unable to add model file to archive");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add model file to archive\n");
                return false;
            }
        }

        // Instance transformations, indexed by the 3MF object ID (which is a linear serialization of all instances of all ModelObjects).
        BuildItemsList build_items;

        // The object_id here is a one based identifier of the first instance of a ModelObject in the 3MF file, where
        // all the object instances of all ModelObjects are stored and indexed in a 1 based linear fashion.
        // Therefore the list of object_ids here may not be continuous.
        unsigned int object_id = 1;
        unsigned int object_index = 0;

        bool cb_cancel = false;
        std::vector<std::string> object_paths;
        // if (!m_skip_model) {
            for (ModelObject* obj : model.objects) {
                if (sub_model && obj != objects_data.begin()->second.object) continue;

                if (proFn) {
                    proFn(EXPORT_STAGE_ADD_MODELS, object_index++, model.objects.size(), cb_cancel);
                    if (cb_cancel)
                        return false;
                }

                if (obj == nullptr)
                    continue;

                // Index of an object in the 3MF file corresponding to the 1st instance of a ModelObject.
                ObjectToObjectDataMap::iterator object_it = objects_data.begin();

                if (!sub_model) {
                    // For backup, use backup id as object id
                    int backup_id = const_cast<Model&>(model).get_object_backup_id(*obj);
                    if (m_from_backup_save) object_id = backup_id;
                    object_it = objects_data.insert({obj, {obj, backup_id} }).first;
                    auto & object_data = object_it->second;

                    if (m_split_model) {
                        auto filename = boost::format("3D/Objects/%s_%d.model") % obj->name % backup_id;
                        object_data.sub_path = "/" + filename.str();
                        object_paths.push_back(filename.str());
                    }

                    auto &volumes_objectID = object_data.volumes_objectID;
                    unsigned int volume_id = object_id, volume_count = 0;
                    for (ModelVolume *volume : obj->volumes) {
                        if (volume == nullptr)
                            continue;
                        volume_count++;
                        if (m_share_mesh) {
                            auto iter = m_shared_meshes.find(volume->mesh_ptr().get());
                            if (iter != m_shared_meshes.end())
                            {
                                const ModelVolume* shared_volume = iter->second.second;
                                if ((shared_volume->supported_facets.equals(volume->supported_facets))
                                    && (shared_volume->seam_facets.equals(volume->seam_facets))
                                    && (shared_volume->mmu_segmentation_facets.equals(volume->mmu_segmentation_facets))
                                    && (shared_volume->fuzzy_skin_facets.equals(volume->fuzzy_skin_facets)))
                                {
                                    auto data = iter->second.first;
                                    const_cast<_BBS_3MF_Exporter *>(this)->m_volume_paths.insert({volume, {data->sub_path, data->volumes_objectID.find(iter->second.second)->second}});
                                    volumes_objectID.insert({volume, 0});
                                    object_data.share_mesh = true;
                                    continue;
                                }
                            }
                            const_cast<_BBS_3MF_Exporter *>(this)->m_shared_meshes.insert({volume->mesh_ptr().get(), {&object_data, volume}});
                        }
                        if (m_from_backup_save)
                            volume_id = (volume_count << 16 | backup_id);
                        volumes_objectID.insert({volume, volume_id});
                        volume_id++;
                    }

                    if (!m_from_backup_save) object_id = volume_id;
                    object_data.object_id = object_id;
                }

                if (m_skip_model) continue;

                if (write_object) {
                    // Store geometry of all ModelVolumes contained in a single ModelObject into a single 3MF indexed triangle set object.
                    // object_it->second.volumes_objectID will contain the offsets of the ModelVolumes in that single indexed triangle set.
                    // object_id will be increased to point to the 1st instance of the next ModelObject.
                    if (!_add_object_to_model_stream(context, object_it->second)) {
                        add_error("Unable to add object to archive");
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add object to archive\n");
                        return false;
                    }
                }

                if (sub_model) break;

                unsigned int count = 0;
                for (const ModelInstance* instance : obj->instances) {
                    Transform3d t = instance->get_matrix();
                    // instance_id is just a 1 indexed index in build_items.
                    //assert(m_skip_static || curr_id == build_items.size() + 1);
                    build_items.emplace_back("", object_it->second.object_id, t, instance->printable);
                    count++;
                }

                if (!m_from_backup_save) object_id++;
            }
        // }

        {
            std::stringstream stream;
            reset_stream(stream);

            if (!m_skip_model && !sub_model) {
                for (auto object : model.objects) {
                    auto &data = objects_data[object];
                    _add_object_components_to_stream(stream, data);
                }
            }

            stream << " </" << RESOURCES_TAG << ">\n";

            // Store the transformations of all the ModelInstances of all ModelObjects, indexed in a linear fashion.
            if (!_add_build_to_model_stream(stream, build_items)) {
                add_error("Unable to add build to archive");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add build to archive\n");
                return false;
            }

            stream << "</" << MODEL_TAG << ">\n";

            std::string buf = stream.str();

            if ((! buf.empty() && ! mz_zip_writer_add_staged_data(&context, buf.data(), buf.size())) ||
                ! mz_zip_writer_add_staged_finish(&context)) {
                add_error("Unable to add model file to archive");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add model file to archive\n");
                return false;
            }
        }

        if (m_skip_model || write_object) return true;

        // write model rels
        _add_relationships_file_to_archive(archive, MODEL_RELS_FILE, object_paths, {"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"});

        if (!m_from_backup_save) {
            boost::mutex mutex;
            tbb::parallel_for(tbb::blocked_range<size_t>(0, objects_data.size(), 1), [this, &mutex, &model, objects = model.objects, &objects_data, &object_paths, main = &archive, project](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i < range.end(); ++i) {
                    auto iter = objects_data.find(objects[i]);
                    ObjectToObjectDataMap objects_data2;
                    objects_data2.insert(*iter);
                    auto & object = *iter->second.object;
                    mz_zip_archive archive;
                    mz_zip_zero_struct(&archive);
                    mz_zip_writer_init_heap(&archive, 0, 1024 * 1024);
                    CNumericLocalesSetter locales_setter;
                    _add_model_file_to_archive(object_paths[i], archive, model, objects_data2, nullptr, project);
                    iter->second = objects_data2.begin()->second;
                    void *ppBuf; size_t pSize;
                    mz_zip_writer_finalize_heap_archive(&archive, &ppBuf, &pSize);
                    mz_zip_writer_end(&archive);
                    mz_zip_zero_struct(&archive);
                    mz_zip_reader_init_mem(&archive, ppBuf, pSize, 0);
                    {
                        boost::unique_lock l(mutex);
                        mz_zip_writer_add_from_zip_reader(main, &archive, 0);
                    }
                    mz_zip_reader_end(&archive);
                }
            });
        }

        return true;
    }

    bool _BBS_3MF_Exporter::_add_object_to_model_stream(mz_zip_writer_staged_context &context, ObjectData const &object_data) const
    {
        // backup: make _add_mesh_to_object_stream() reusable
        auto flush = [this, &context](std::string & buf, bool force = false) {
            if ((force && !buf.empty()) || buf.size() >= 65536 * 16) {
                if (!mz_zip_writer_add_staged_data(&context, buf.data(), buf.size())) {
                    add_error("Error during writing or compression");
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Error during writing or compression\n");
                    return false;
                }
                buf.clear();
            }
            return true;
        };
        if (!_add_mesh_to_object_stream(flush, object_data)) {
            add_error("Unable to add mesh to archive");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add mesh to archive\n");
            return false;
        }

        // Move all components to main model
        //_add_object_components_to_stream(stream, object_data);

        return true;
    }

    void _BBS_3MF_Exporter::_add_object_components_to_stream(std::stringstream &stream, ObjectData const &object_data) const
    {
        auto &       object = *object_data.object;

        stream << "  <" << OBJECT_TAG << " id=\"" << object_data.object_id;
        if (m_production_ext)
            stream << "\" " << PUUID_ATTR << "=\"" << hex_wrap<boost::uint32_t>{(boost::uint32_t)object_data.backup_id}
                    << (object_data.share_mesh ? OBJECT_UUID_SUFFIX2 : OBJECT_UUID_SUFFIX);
        stream << "\" type=\"model\">\n";

        stream << "   <" << COMPONENTS_TAG << ">\n";

        for (unsigned int index = 0; index < object.volumes.size(); index ++) {
            ModelVolume *volume    = object.volumes[index];
            unsigned int volume_id = object_data.volumes_objectID.find(volume)->second;
            auto * ppath = &object_data.sub_path;
            auto iter = m_volume_paths.find(volume);
            if (iter != m_volume_paths.end()) {
                ppath = &iter->second.first;
                volume_id = iter->second.second;
            }
            //add the transform of the volume
            if (ppath->empty())
                stream << "    <" << COMPONENT_TAG << " objectid=\"" << volume_id;
            else
                stream << "    <" << COMPONENT_TAG << " p:path=\"" << xml_escape(*ppath) << "\" objectid=\"" << volume_id; // << "\"/>\n";
            if (m_production_ext)
                stream << "\" " << PUUID_ATTR << "=\"" << hex_wrap<boost::uint32_t>{(boost::uint32_t) (index + (object_data.backup_id << 16))} << COMPONENT_UUID_SUFFIX;
            const Transform3d &transf = volume->get_matrix();
            stream << "\" " << TRANSFORM_ATTR << "=\"";
            for (unsigned c = 0; c < 4; ++c) {
                for (unsigned r = 0; r < 3; ++r) {
                    stream << transf(r, c);
                    if (r != 2 || c != 3)
                        stream << " ";
                }
            }
            stream << "\"/>\n";
        }

        stream << "   </" << COMPONENTS_TAG << ">\n";

        stream << "  </" << OBJECT_TAG << ">\n";
    }

#if EXPORT_3MF_USE_SPIRIT_KARMA_FP
    template <typename Num>
    struct coordinate_policy_fixed : boost::spirit::karma::real_policies<Num>
    {
        static int floatfield(Num n) { return fmtflags::fixed; }
        // Number of decimal digits to maintain float accuracy when storing into a text file and parsing back.
        static unsigned precision(Num /* n */) { return std::numeric_limits<Num>::max_digits10 + 1; }
        // No trailing zeros, thus for fmtflags::fixed usually much less than max_digits10 decimal numbers will be produced.
        static bool trailing_zeros(Num /* n */) { return false; }
    };
    template <typename Num>
    struct coordinate_policy_scientific : coordinate_policy_fixed<Num>
    {
        static int floatfield(Num n) { return fmtflags::scientific; }
    };
    // Define a new generator type based on the new coordinate policy.
    using coordinate_type_fixed      = boost::spirit::karma::real_generator<float, coordinate_policy_fixed<float>>;
    using coordinate_type_scientific = boost::spirit::karma::real_generator<float, coordinate_policy_scientific<float>>;
#endif // EXPORT_3MF_USE_SPIRIT_KARMA_FP

    //BBS: change volume to seperate objects
    bool _BBS_3MF_Exporter::_add_mesh_to_object_stream(std::function<bool(std::string &, bool)> const &flush, ObjectData const &object_data) const
    {
        std::string output_buffer;

#if 0
        auto flush = [this, &output_buffer, &context](bool force = false) {
            if ((force && ! output_buffer.empty()) || output_buffer.size() >= 65536 * 16) {
                if (! mz_zip_writer_add_staged_data(&context, output_buffer.data(), output_buffer.size())) {
                    add_error("Error during writing or compression");
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Error during writing or compression\n");
                    return false;
                }
                output_buffer.clear();
            }
            return true;
        };
#endif

        /*output_buffer += "   <";
        output_buffer += MESH_TAG;
        output_buffer += ">\n    <";
        output_buffer += VERTICES_TAG;
        output_buffer += ">\n";*/

        auto format_coordinate = [](float f, char *buf) -> char* {
            assert(is_decimal_separator_point());
#if EXPORT_3MF_USE_SPIRIT_KARMA_FP
            // Slightly faster than sprintf("%.9g"), but there is an issue with the karma floating point formatter,
            // https://github.com/boostorg/spirit/pull/586
            // where the exported string is one digit shorter than it should be to guarantee lossless round trip.
            // The code is left here for the ocasion boost guys improve.
            coordinate_type_fixed      const coordinate_fixed      = coordinate_type_fixed();
            coordinate_type_scientific const coordinate_scientific = coordinate_type_scientific();
            // Format "f" in a fixed format.
            char *ptr = buf;
            boost::spirit::karma::generate(ptr, coordinate_fixed, f);
            // Format "f" in a scientific format.
            char *ptr2 = ptr;
            boost::spirit::karma::generate(ptr2, coordinate_scientific, f);
            // Return end of the shorter string.
            auto len2 = ptr2 - ptr;
            if (ptr - buf > len2) {
                // Move the shorter scientific form to the front.
                memcpy(buf, ptr, len2);
                ptr = buf + len2;
            }
            // Return pointer to the end.
            return ptr;
#else
            // Round-trippable float, shortest possible.
            return buf + sprintf(buf, "%.9g", f);
#endif
        };

        auto const & object = *object_data.object;

        char buf[256];
        unsigned int vertices_count = 0;
        //unsigned int triangles_count = 0;
        for (unsigned int index = 0; index < object.volumes.size(); index++) {
            ModelVolume *volume = object.volumes[index];
            if (volume == nullptr)
                continue;

            int volume_id = object_data.volumes_objectID.find(volume)->second;
            if (m_share_mesh && volume_id == 0)
                continue;

			//if (!volume->mesh().stats().repaired())
			//	throw Slic3r::FileIOError("store_3mf() requires repair()");

            const indexed_triangle_set &its = volume->mesh().its;
            if (its.vertices.empty()) {
                add_error("Found invalid mesh");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Found invalid mesh\n");
                return false;
            }

            // Orca#7574: always use "model" type to follow the 3MF Core Specification:
            // https://github.com/3MFConsortium/spec_core/blob/20c079eef39e45ed223b8443dc9f34cbe32dc2c2/3MF%20Core%20Specification.md#3431-item-element
            // > Note: items MUST NOT reference objects of type "other", either directly or recursively.
            // This won't break anything because when loading the file Orca (and Bambu) simply does not care about the actual object type at all (as long as it's one of "model" & "other");
            // But PrusaSlicer requires the type to be "model".
            std::string type = "model";

            output_buffer += "  <";
            output_buffer += OBJECT_TAG;
            output_buffer += " id=\"";
            output_buffer += std::to_string(volume_id);
            if (m_production_ext) {
                std::stringstream stream;
                reset_stream(stream);
                stream << "\" " << PUUID_ATTR << "=\"" << hex_wrap<boost::uint32_t>{(boost::uint32_t) (index + (object_data.backup_id << 16))} << SUB_OBJECT_UUID_SUFFIX;
                //output_buffer += "\" ";
                //output_buffer += PUUID_ATTR;
                //output_buffer += "=\"";
                //output_buffer += std::to_string(hex_wrap<boost::uint32_t>{(boost::uint32_t)backup_id});
                //output_buffer += OBJECT_UUID_SUFFIX;
                output_buffer += stream.str();
            }
            output_buffer += "\" type=\"";
            output_buffer += type;
            output_buffer += "\">\n";
            output_buffer += "   <";
            output_buffer += MESH_TAG;
            output_buffer += ">\n    <";
            output_buffer += VERTICES_TAG;
            output_buffer += ">\n";

            vertices_count += (int)its.vertices.size();

            for (size_t i = 0; i < its.vertices.size(); ++i) {
                //don't save the volume's matrix into vertex data
                //add the shared mesh logic
                //Vec3f v = (matrix * its.vertices[i].cast<double>()).cast<float>();
                Vec3f v = its.vertices[i];
                char* ptr = buf;
                boost::spirit::karma::generate(ptr, boost::spirit::lit("     <") << VERTEX_TAG << " x=\"");
                ptr = format_coordinate(v.x(), ptr);
                boost::spirit::karma::generate(ptr, "\" y=\"");
                ptr = format_coordinate(v.y(), ptr);
                boost::spirit::karma::generate(ptr, "\" z=\"");
                ptr = format_coordinate(v.z(), ptr);
                boost::spirit::karma::generate(ptr, "\"/>\n");
                *ptr = '\0';
                output_buffer += buf;
                if (!flush(output_buffer, false))
                    return false;
            }
        //}

            output_buffer += "    </";
            output_buffer += VERTICES_TAG;
            output_buffer += ">\n    <";
            output_buffer += TRIANGLES_TAG;
            output_buffer += ">\n";

        //for (ModelVolume* volume : object.volumes) {
        //    if (volume == nullptr)
        //        continue;

            //BBS: as we stored matrix seperately, not multiplied into vertex
            //we don't need to consider this left hand case specially
            //bool is_left_handed = volume->is_left_handed();
            bool is_left_handed = false;
            //VolumeToOffsetsMap::iterator volume_it = volumes_objectID.find(volume);
            //assert(volume_it != volumes_objectID.end());

            //const indexed_triangle_set &its = volume->mesh().its;

            // updates triangle offsets
            //unsigned int first_triangle_id = triangles_count;
            //triangles_count += (int)its.indices.size();
            //unsigned int last_triangle_id = triangles_count - 1;

            for (int i = 0; i < int(its.indices.size()); ++ i) {
                {
                    const Vec3i32 &idx = its.indices[i];
                    char *ptr = buf;
                    boost::spirit::karma::generate(ptr, boost::spirit::lit("     <") << TRIANGLE_TAG <<
                        " v1=\"" << boost::spirit::int_ <<
                        "\" v2=\"" << boost::spirit::int_ <<
                        "\" v3=\"" << boost::spirit::int_ << "\"",
                        idx[is_left_handed ? 2 : 0],
                        idx[1],
                        idx[is_left_handed ? 0 : 2]);
                    *ptr = '\0';
                    output_buffer += buf;
                }

                std::string custom_supports_data_string = volume->supported_facets.get_triangle_as_string(i);
                if (! custom_supports_data_string.empty()) {
                    output_buffer += " ";
                    output_buffer += CUSTOM_SUPPORTS_ATTR;
                    output_buffer += "=\"";
                    output_buffer += custom_supports_data_string;
                    output_buffer += "\"";
                }

                std::string custom_seam_data_string = volume->seam_facets.get_triangle_as_string(i);
                if (! custom_seam_data_string.empty()) {
                    output_buffer += " ";
                    output_buffer += CUSTOM_SEAM_ATTR;
                    output_buffer += "=\"";
                    output_buffer += custom_seam_data_string;
                    output_buffer += "\"";
                }

                std::string mmu_painting_data_string = volume->mmu_segmentation_facets.get_triangle_as_string(i);
                if (! mmu_painting_data_string.empty()) {
                    output_buffer += " ";
                    output_buffer += MMU_SEGMENTATION_ATTR;
                    output_buffer += "=\"";
                    output_buffer += mmu_painting_data_string;
                    output_buffer += "\"";
                }

                std::string fuzzy_skin_painting_data_string = volume->fuzzy_skin_facets.get_triangle_as_string(i);
                if (!fuzzy_skin_painting_data_string.empty()) {
                    output_buffer += " ";
                    output_buffer += CUSTOM_FUZZY_SKIN_ATTR;
                    output_buffer += "=\"";
                    output_buffer += fuzzy_skin_painting_data_string;
                    output_buffer += "\"";
                }

                // BBS
                if (i < its.properties.size()) {
                    std::string prop_str = its.properties[i].to_string();
                    if (!prop_str.empty()) {
                        output_buffer += " ";
                        output_buffer += FACE_PROPERTY_ATTR;
                        output_buffer += "=\"";
                        output_buffer += prop_str;
                        output_buffer += "\"";
                    }
                }

                output_buffer += "/>\n";

                if (! flush(output_buffer, false))
                    return false;
            }
            output_buffer += "    </";
            output_buffer += TRIANGLES_TAG;
            output_buffer += ">\n   </";
            output_buffer += MESH_TAG;
            output_buffer += ">\n";
            output_buffer +=  "  </";
            output_buffer += OBJECT_TAG;
            output_buffer += ">\n";
        }

        // Force flush.
        return flush(output_buffer, true);
    }

    void _BBS_3MF_Exporter::add_transformation(std::stringstream &stream, const Transform3d &tr)
    {
        for (unsigned c = 0; c < 4; ++c) {
            for (unsigned r = 0; r < 3; ++r) {
                stream << tr(r, c);
                if (r != 2 || c != 3) stream << " ";
            }
        }
    }

    bool _BBS_3MF_Exporter::_add_build_to_model_stream(std::stringstream& stream, const BuildItemsList& build_items) const
    {
        // This happens for empty projects
        if (build_items.size() == 0) {
            stream << " <" << BUILD_TAG << "/>\n";
            return true;
        }

        stream << " <" << BUILD_TAG;
        if (m_production_ext)
            stream << " " << PUUID_ATTR << "=\"" << BUILD_UUID << "\"";
        stream << ">\n";

        for (const BuildItem& item : build_items) {
            stream << "  <" << ITEM_TAG << " " << OBJECTID_ATTR << "=\"" << item.id;
            if (m_production_ext)
                stream << "\" " << PUUID_ATTR << "=\"" << hex_wrap<boost::uint32_t>{item.id} << BUILD_UUID_SUFFIX;
            if (!item.path.empty())
                stream << "\" " << PPATH_ATTR << "=\"" << xml_escape(item.path);
            stream << "\" " << TRANSFORM_ATTR << "=\"";
            add_transformation(stream, item.transform);
            stream << "\" " << PRINTABLE_ATTR << "=\"" << item.printable << "\"/>\n";
        }

        stream << " </" << BUILD_TAG << ">\n";

        return true;
    }

    bool _BBS_3MF_Exporter::_add_cut_information_file_to_archive(mz_zip_archive &archive, Model &model)
    {
        std::string out = "";
        pt::ptree tree;

        unsigned int object_cnt = 0;
        for (const ModelObject* object : model.objects) {
            object_cnt++;
            if (!object->is_cut())
                continue;
            pt::ptree& obj_tree = tree.add("objects.object", "");

            obj_tree.put("<xmlattr>.id", object_cnt);

            // Store info for cut_id
            pt::ptree& cut_id_tree = obj_tree.add("cut_id", "");

            // store cut_id atributes
            cut_id_tree.put("<xmlattr>.id",             object->cut_id.id().id);
            cut_id_tree.put("<xmlattr>.check_sum",      object->cut_id.check_sum());
            cut_id_tree.put("<xmlattr>.connectors_cnt", object->cut_id.connectors_cnt());

            int volume_idx = -1;
            for (const ModelVolume* volume : object->volumes) {
                ++volume_idx;
                if (volume->is_cut_connector()) {
                    pt::ptree& connectors_tree = obj_tree.add("connectors.connector", "");
                    connectors_tree.put("<xmlattr>.volume_id",   volume_idx);
                    connectors_tree.put("<xmlattr>.type",        int(volume->cut_info.connector_type));
                    connectors_tree.put("<xmlattr>.r_tolerance", volume->cut_info.radius_tolerance);
                    connectors_tree.put("<xmlattr>.h_tolerance", volume->cut_info.height_tolerance);
                }
            }
        }

        if (!tree.empty()) {
            std::ostringstream oss;
            pt::write_xml(oss, tree);
            out = oss.str();

            // Post processing("beautification") of the output string for a better preview
            boost::replace_all(out, "><object", ">\n <object");
            boost::replace_all(out, "><cut_id", ">\n  <cut_id");
            boost::replace_all(out, "></cut_id>", ">\n  </cut_id>");
            boost::replace_all(out, "><connectors", ">\n  <connectors");
            boost::replace_all(out, "></connectors>", ">\n  </connectors>");
            boost::replace_all(out, "><connector", ">\n   <connector");
            boost::replace_all(out, "></connector>", ">\n   </connector>");
            boost::replace_all(out, "></object>", ">\n </object>");
            // OR just 
            boost::replace_all(out, "><", ">\n<");
        }

        if (!out.empty()) {
            if (!mz_zip_writer_add_mem(&archive, CUT_INFORMATION_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
                add_error("Unable to add cut information file to archive");
                return false;
            }
        }

        return true;
    }

    bool _BBS_3MF_Exporter::_add_layer_height_profile_file_to_archive(mz_zip_archive& archive, Model& model)
    {
        assert(is_decimal_separator_point());
        std::string out = "";
        char buffer[1024];

        unsigned int count = 0;
        for (const ModelObject* object : model.objects) {
            ++count;
            const std::vector<double>& layer_height_profile = object->layer_height_profile.get();
            if (layer_height_profile.size() >= 4 && layer_height_profile.size() % 2 == 0) {
                snprintf(buffer, 1024, "object_id=%d|", count);
                out += buffer;

                // Store the layer height profile as a single semicolon separated list.
                for (size_t i = 0; i < layer_height_profile.size(); ++i) {
                    snprintf(buffer, 1024, (i == 0) ? "%f" : ";%f", layer_height_profile[i]);
                    out += buffer;
                }

                out += "\n";
            }
        }

        if (!out.empty()) {
            if (!mz_zip_writer_add_mem(&archive, BBS_LAYER_HEIGHTS_PROFILE_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
                add_error("Unable to add layer heights profile file to archive");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format("Unable to add layer heights profile file to archive\n");
                return false;
            }
        }

        return true;
    }

    bool _BBS_3MF_Exporter::_add_layer_config_ranges_file_to_archive(mz_zip_archive& archive, Model& model)
    {
        std::string out = "";
        pt::ptree tree;

        unsigned int object_cnt = 0;
        for (const ModelObject* object : model.objects) {
            object_cnt++;
            const t_layer_config_ranges& ranges = object->layer_config_ranges;
            if (!ranges.empty())
            {
                pt::ptree& obj_tree = tree.add("objects.object","");

                obj_tree.put("<xmlattr>.id", object_cnt);

                // Store the layer config ranges.
                for (const auto& range : ranges) {
                    pt::ptree& range_tree = obj_tree.add("range", "");

                    // store minX and maxZ
                    range_tree.put("<xmlattr>.min_z", range.first.first);
                    range_tree.put("<xmlattr>.max_z", range.first.second);

                    // store range configuration
                    const ModelConfig& config = range.second;
                    for (const std::string& opt_key : config.keys()) {
                        pt::ptree& opt_tree = range_tree.add("option", config.opt_serialize(opt_key));
                        opt_tree.put("<xmlattr>.opt_key", opt_key);
                    }
                }
            }
        }

        if (!tree.empty()) {
            std::ostringstream oss;
            pt::write_xml(oss, tree);
            out = oss.str();

            // Post processing("beautification") of the output string for a better preview
            boost::replace_all(out, "><object",      ">\n <object");
            boost::replace_all(out, "><range",       ">\n  <range");
            boost::replace_all(out, "><option",      ">\n   <option");
            boost::replace_all(out, "></range>",     ">\n  </range>");
            boost::replace_all(out, "></object>",    ">\n </object>");
            // OR just
            boost::replace_all(out, "><",            ">\n<");
        }

        if (!out.empty()) {
            if (!mz_zip_writer_add_mem(&archive, LAYER_CONFIG_RANGES_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
                add_error("Unable to add layer heights profile file to archive");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format("Unable to add layer heights profile file to archive\n");
                return false;
            }
        }

        return true;
    }

    bool _BBS_3MF_Exporter::_add_brim_ear_points_file_to_archive(mz_zip_archive& archive, Model& model)
    {
        std::string out = "";
        char buffer[1024];

        unsigned int count = 0;
        for (const ModelObject* object : model.objects) {
            ++count;
            const BrimPoints& brim_points = object->brim_points;
            if (!brim_points.empty()) {
                sprintf(buffer, "object_id=%d|", count);
                out += buffer;

                // Store the layer height profile as a single space separated list.
                for (size_t i = 0; i < brim_points.size(); ++i) {
                    sprintf(buffer, (i==0 ? "%f %f %f %f" : " %f %f %f %f"),  brim_points[i].pos(0), brim_points[i].pos(1), brim_points[i].pos(2), brim_points[i].head_front_radius);
                    out += buffer;
                }
                out += "\n";
            }
        }

        if (!out.empty()) {
            // Adds version header at the beginning:
            out = std::string("brim_points_format_version=") + std::to_string(brim_points_format_version) + std::string("\n") + out;

            if (!mz_zip_writer_add_mem(&archive, BRIM_EAR_POINTS_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
                add_error("Unable to add brim ear points file to archive");
                return false;
            }
        }
        return true;
    }

    /*
    bool _BBS_3MF_Exporter::_add_sla_support_points_file_to_archive(mz_zip_archive& archive, Model& model)
    {
        assert(is_decimal_separator_point());
        std::string out = "";
        char buffer[1024];

        unsigned int count = 0;
        for (const ModelObject* object : model.objects) {
            ++count;
            const std::vector<sla::SupportPoint>& sla_support_points = object->sla_support_points;
            if (!sla_support_points.empty()) {
                sprintf(buffer, "object_id=%d|", count);
                out += buffer;

                // Store the layer height profile as a single space separated list.
                for (size_t i = 0; i < sla_support_points.size(); ++i) {
                    sprintf(buffer, (i==0 ? "%f %f %f %f %f" : " %f %f %f %f %f"),  sla_support_points[i].pos(0), sla_support_points[i].pos(1), sla_support_points[i].pos(2), sla_support_points[i].head_front_radius, (float)sla_support_points[i].is_new_island);
                    out += buffer;
                }
                out += "\n";
            }
        }

        if (!out.empty()) {
            // Adds version header at the beginning:
            //out = std::string("support_points_format_version=") + std::to_string(support_points_format_version) + std::string("\n") + out;

            if (!mz_zip_writer_add_mem(&archive, SLA_SUPPORT_POINTS_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
                add_error("Unable to add sla support points file to archive");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format("Unable to add sla support points file to archive\n");
                return false;
            }
        }
        return true;
    }

    bool _BBS_3MF_Exporter::_add_sla_drain_holes_file_to_archive(mz_zip_archive& archive, Model& model)
    {
        assert(is_decimal_separator_point());
        const char *const fmt = "object_id=%d|";
        std::string out;

        unsigned int count = 0;
        for (const ModelObject* object : model.objects) {
            ++count;
            sla::DrainHoles drain_holes = object->sla_drain_holes;

            // The holes were placed 1mm above the mesh in the first implementation.
            // This was a bad idea and the reference point was changed in 2.3 so
            // to be on the mesh exactly. The elevated position is still saved
            // in 3MFs for compatibility reasons.
            for (sla::DrainHole& hole : drain_holes) {
                hole.pos -= hole.normal.normalized();
                hole.height += 1.f;
            }

            if (!drain_holes.empty()) {
                out += string_printf(fmt, count);

                // Store the layer height profile as a single space separated list.
                for (size_t i = 0; i < drain_holes.size(); ++i)
                    out += string_printf((i == 0 ? "%f %f %f %f %f %f %f %f" : " %f %f %f %f %f %f %f %f"),
                                         drain_holes[i].pos(0),
                                         drain_holes[i].pos(1),
                                         drain_holes[i].pos(2),
                                         drain_holes[i].normal(0),
                                         drain_holes[i].normal(1),
                                         drain_holes[i].normal(2),
                                         drain_holes[i].radius,
                                         drain_holes[i].height);

                out += "\n";
            }
        }

        if (!out.empty()) {
            // Adds version header at the beginning:
            //out = std::string("drain_holes_format_version=") + std::to_string(drain_holes_format_version) + std::string("\n") + out;

            if (!mz_zip_writer_add_mem(&archive, SLA_DRAIN_HOLES_FILE.c_str(), static_cast<const void*>(out.data()), out.length(), mz_uint(MZ_DEFAULT_COMPRESSION))) {
                add_error("Unable to add sla support points file to archive");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format("Unable to add sla support points file to archive\n");
                return false;
            }
        }
        return true;
    }*/

    bool _BBS_3MF_Exporter::_add_print_config_file_to_archive(mz_zip_archive& archive, const DynamicPrintConfig &config)
    {
        assert(is_decimal_separator_point());
        char buffer[1024];
        snprintf(buffer, 1024, "; %s\n\n", header_slic3r_generated().c_str());
        std::string out = buffer;

        for (const std::string &key : config.keys())
            if (key != "compatible_printers")
                out += "; " + key + " = " + config.opt_serialize(key) + "\n";

        if (!out.empty()) {
            if (!mz_zip_writer_add_mem(&archive, BBS_PRINT_CONFIG_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
                add_error("Unable to add print config file to archive");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format("Unable to add print config file to archive\n");
                return false;
            }
        }

        return true;
    }

    //BBS: add project config file logic for new json format
    bool _BBS_3MF_Exporter::_add_project_config_file_to_archive(mz_zip_archive& archive, const DynamicPrintConfig &config, Model& model)
    {
        const std::string& temp_path = model.get_backup_path();
        std::string temp_file = temp_path + std::string("/") + "_temp_1.config";
        config.save_to_json(temp_file, std::string("project_settings"), std::string("project"), std::string(SoftFever_VERSION));
        return _add_file_to_archive(archive, BBS_PROJECT_CONFIG_FILE, temp_file);
    }

    //BBS: add project embedded preset files
    bool _BBS_3MF_Exporter::_add_project_embedded_presets_to_archive(mz_zip_archive& archive, Model& model, std::vector<Preset*> project_presets)
    {
        char buffer[1024];
        snprintf(buffer, 1024, "; %s\n\n", header_slic3r_generated().c_str());
        std::string out = buffer;
        int print_count = 0, filament_count = 0, printer_count = 0;
        const std::string& temp_path = model.get_backup_path();

        for (int i = 0; i < project_presets.size(); i++)
        {
            Preset* preset = project_presets[i];

            if (preset) {
                preset->file = temp_path + std::string("/") + "_temp_1.config";
                DynamicPrintConfig& config = preset->config;
                //config.save(preset->file);
                config.save_to_json(preset->file, preset->name, std::string("project"), preset->version.to_string());

                std::string dest_file;
                if (preset->type == Preset::TYPE_PRINT) {
                    dest_file = (boost::format(EMBEDDED_PRINT_FILE_FORMAT) % (print_count + 1)).str();
                    print_count++;
                }
                else if (preset->type == Preset::TYPE_FILAMENT) {
                    dest_file = (boost::format(EMBEDDED_FILAMENT_FILE_FORMAT) % (filament_count + 1)).str();
                    filament_count++;
                }
                else if (preset->type == Preset::TYPE_PRINTER) {
                    dest_file = (boost::format(EMBEDDED_PRINTER_FILE_FORMAT) % (printer_count + 1)).str();
                    printer_count++;
                }
                else
                    continue;

                _add_file_to_archive(archive, dest_file, preset->file);
            }
        }

        return true;
    }

    boost::filesystem::path get_dealed_platform_path(std::string path_str) {
#if defined(__linux__) || defined(__LINUX__) || defined(__APPLE__)
        std::string translated_input = path_str;
        std::replace(translated_input.begin(), translated_input.end(), '\\', '/');

        boost::filesystem::path file_path(translated_input);
#else
        boost::filesystem::path file_path(path_str);
#endif
        return file_path;
    }

    bool _BBS_3MF_Exporter::_add_model_config_file_to_archive(mz_zip_archive& archive, const Model& model, PlateDataPtrs& plate_data_list, const ObjectToObjectDataMap &objects_data, const DynamicPrintConfig& config, int export_plate_idx, bool save_gcode, bool use_loaded_id)
    {
        std::stringstream stream;
        // Store mesh transformation in full precision, as the volumes are stored transformed and they need to be transformed back
        // when loaded as accurately as possible.
		stream << std::setprecision(std::numeric_limits<double>::max_digits10);
        stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        stream << "<" << CONFIG_TAG << ">\n";

        if (!m_skip_model)
        for (const ObjectToObjectDataMap::value_type& obj_metadata : objects_data) {
            auto object_data = obj_metadata.second;
            const ModelObject *obj = object_data.object;
            if (obj != nullptr) {
                // Output of instances count added because of github #3435, currently not used by PrusaSlicer
                //stream << "  <"  << OBJECT_TAG << " " << ID_ATTR << "=\"" << obj_metadata.first << "\" " << INSTANCESCOUNT_ATTR << "=\"" << obj->instances.size() << "\">\n";
                stream << "  <" << OBJECT_TAG << " " << ID_ATTR << "=\"" << object_data.object_id << "\">\n";

                // stores object's name
                if (!obj->name.empty())
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"name\" " << VALUE_ATTR << "=\"" << xml_escape(obj->name) << "\"/>\n";

                //BBS: store object's module name
                if (!obj->module_name.empty())
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"module\" " << VALUE_ATTR << "=\"" << xml_escape(obj->module_name) << "\"/>\n";

                // stores object's config data
                for (const std::string& key : obj->config.keys()) {
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << key << "\" " << VALUE_ATTR << "=\"" << obj->config.opt_serialize(key) << "\"/>\n";
                }

                for (const ModelVolume* volume : obj_metadata.second.object->volumes) {
                    if (volume != nullptr) {
                        const VolumeToObjectIDMap& objectIDs = obj_metadata.second.volumes_objectID;
                        VolumeToObjectIDMap::const_iterator it = objectIDs.find(volume);
                        if (it != objectIDs.end()) {
                            // stores volume's offsets
                            stream << "    <" << PART_TAG << " ";
                            //stream << FIRST_TRIANGLE_ID_ATTR << "=\"" << it->second.first_triangle_id << "\" ";
                            //stream << LAST_TRIANGLE_ID_ATTR << "=\"" << it->second.last_triangle_id << "\" ";
                            int volume_id = it->second;
                            if (m_share_mesh && volume_id == 0)
                                volume_id = m_volume_paths.find(volume)->second.second;
                            stream << ID_ATTR << "=\"" << volume_id << "\" ";

                            stream << SUBTYPE_ATTR << "=\"" << ModelVolume::type_to_string(volume->type()) << "\">\n";
                            //stream << "    <" << PART_TAG << " " << ID_ATTR << "=\"" << it->second << "\" " << SUBTYPE_ATTR << "=\"" << ModelVolume::type_to_string(volume->type()) << "\">\n";

                            // stores volume's name
                            if (!volume->name.empty())
                                stream << "      <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << NAME_KEY << "\" " << VALUE_ATTR << "=\"" << xml_escape(volume->name) << "\"/>\n";
                                //stream << "      <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << NAME_KEY << "\" " << VALUE_ATTR << "=\"" << xml_escape(volume->name) << "\"/>\n";

                            // stores volume's modifier field (legacy, to support old slicers)
                            /*if (volume->is_modifier())
                                stream << "      <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << PART_TYPE << "\" " << KEY_ATTR << "=\"" << MODIFIER_KEY << "\" " << VALUE_ATTR << "=\"1\"/>\n";
                            // stores volume's type (overrides the modifier field above)
                            stream << "      <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << PART_TYPE << "\" " << KEY_ATTR << "=\"" << PART_TYPE_KEY << "\" " <<
                                VALUE_ATTR << "=\"" << ModelVolume::type_to_string(volume->type()) << "\"/>\n";*/

                            // stores volume's local matrix
                            stream << "      <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << MATRIX_KEY << "\" " << VALUE_ATTR << "=\"";
                            Transform3d matrix = volume->get_matrix() * volume->source.transform.get_matrix();
                            for (int r = 0; r < 4; ++r) {
                                for (int c = 0; c < 4; ++c) {
                                    stream << matrix(r, c);
                                    if (r != 3 || c != 3)
                                        stream << " ";
                                }
                            }
                            stream << "\"/>\n";

                            // stores volume's source data
                            {
                                auto file_path =get_dealed_platform_path(volume->source.input_file);
                                std::string input_file = xml_escape(m_fullpath_sources ? volume->source.input_file : file_path.filename().string());
                                //std::string prefix = std::string("      <") + METADATA_TAG + " " + KEY_ATTR + "=\"";
                                std::string prefix = std::string("      <") + METADATA_TAG + " " + KEY_ATTR + "=\"";
                                if (! volume->source.input_file.empty()) {
                                    stream << prefix << SOURCE_FILE_KEY      << "\" " << VALUE_ATTR << "=\"" << input_file << "\"/>\n";
                                    stream << prefix << SOURCE_OBJECT_ID_KEY << "\" " << VALUE_ATTR << "=\"" << volume->source.object_idx << "\"/>\n";
                                    stream << prefix << SOURCE_VOLUME_ID_KEY << "\" " << VALUE_ATTR << "=\"" << volume->source.volume_idx << "\"/>\n";
                                    stream << prefix << SOURCE_OFFSET_X_KEY  << "\" " << VALUE_ATTR << "=\"" << volume->source.mesh_offset(0) << "\"/>\n";
                                    stream << prefix << SOURCE_OFFSET_Y_KEY  << "\" " << VALUE_ATTR << "=\"" << volume->source.mesh_offset(1) << "\"/>\n";
                                    stream << prefix << SOURCE_OFFSET_Z_KEY  << "\" " << VALUE_ATTR << "=\"" << volume->source.mesh_offset(2) << "\"/>\n";
                                }
                                assert(! volume->source.is_converted_from_inches || ! volume->source.is_converted_from_meters);
                                if (volume->source.is_converted_from_inches)
                                    stream << prefix << SOURCE_IN_INCHES << "\" " << VALUE_ATTR << "=\"1\"/>\n";
                                else if (volume->source.is_converted_from_meters)
                                    stream << prefix << SOURCE_IN_METERS << "\" " << VALUE_ATTR << "=\"1\"/>\n";
                            }

                            // stores volume's config data
                            for (const std::string& key : volume->config.keys()) {
                                stream << "      <" << METADATA_TAG << " "<< KEY_ATTR << "=\"" << key << "\" " << VALUE_ATTR << "=\"" << volume->config.opt_serialize(key) << "\"/>\n";
                            }

                            if (const std::optional<EmbossShape> &es = volume->emboss_shape; es.has_value()) {
                                to_xml(stream, *es, *volume, archive, m_fullpath_sources);
                            }
                    
                            if (const std::optional<TextConfiguration> &tc = volume->text_configuration;
                                tc.has_value())
                                TextConfigurationSerialization::to_xml(stream, *tc);

                            // stores mesh's statistics
                            const RepairedMeshErrors& stats = volume->mesh().stats().repaired_errors;
                            stream << "      <" << MESH_STAT_TAG << " ";
                            stream << MESH_STAT_EDGES_FIXED        << "=\"" << stats.edges_fixed        << "\" ";
                            stream << MESH_STAT_DEGENERATED_FACETS << "=\"" << stats.degenerate_facets  << "\" ";
                            stream << MESH_STAT_FACETS_REMOVED     << "=\"" << stats.facets_removed     << "\" ";
                            stream << MESH_STAT_FACETS_RESERVED    << "=\"" << stats.facets_reversed    << "\" ";
                            stream << MESH_STAT_BACKWARDS_EDGES    << "=\"" << stats.backwards_edges    << "\"/>\n";

                            stream << "    </" << PART_TAG << ">\n";
                        }
                    }
                }

                stream << "  </" << OBJECT_TAG << ">\n";
            }
        }

        //BBS: store plate related logic
        std::vector<std::string> gcode_paths;
        for (unsigned int i = 0; i < (unsigned int)plate_data_list.size(); ++i)
        {
            PlateData* plate_data = plate_data_list[i];
            int instance_size = plate_data->objects_and_instances.size();

            if (plate_data != nullptr) {
                stream << "  <" << PLATE_TAG << ">\n";
                //plate index
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << PLATERID_ATTR << "\" " << VALUE_ATTR << "=\"" << plate_data->plate_index + 1 << "\"/>\n";
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << PLATER_NAME_ATTR << "\" " << VALUE_ATTR << "=\"" <<  xml_escape(plate_data->plate_name.c_str()) << "\"/>\n";
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << LOCK_ATTR << "\" " << VALUE_ATTR << "=\"" << std::boolalpha<< plate_data->locked<< "\"/>\n";
                ConfigOption* bed_type_opt = plate_data->config.option("curr_bed_type");
                t_config_enum_names bed_type_names = ConfigOptionEnum<BedType>::get_enum_names();
                if (bed_type_opt != nullptr && bed_type_names.size() > bed_type_opt->getInt())
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << BED_TYPE_ATTR << "\" " << VALUE_ATTR << "=\"" << bed_type_names[bed_type_opt->getInt()] << "\"/>\n";

                ConfigOption* print_sequence_opt = plate_data->config.option("print_sequence");
                t_config_enum_names print_sequence_names = ConfigOptionEnum<PrintSequence>::get_enum_names();
                if (print_sequence_opt != nullptr && print_sequence_names.size() > print_sequence_opt->getInt())
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << PRINT_SEQUENCE_ATTR << "\" " << VALUE_ATTR << "=\"" << print_sequence_names[print_sequence_opt->getInt()] << "\"/>\n";

                ConfigOptionInts *first_layer_print_sequence_opt = plate_data->config.option<ConfigOptionInts>("first_layer_print_sequence");
                if (first_layer_print_sequence_opt != nullptr) {
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << FIRST_LAYER_PRINT_SEQUENCE_ATTR << "\" " << VALUE_ATTR << "=\"";
                    const std::vector<int>& values = first_layer_print_sequence_opt->values;
                    for (int i = 0; i < values.size(); ++i) {
                        stream << values[i];
                        if (i != (values.size() - 1))
                            stream << " ";
                    }
                    stream << "\"/>\n";
                }

                ConfigOptionInts *other_layers_print_sequence_opt = plate_data->config.option<ConfigOptionInts>("other_layers_print_sequence");
                if (other_layers_print_sequence_opt != nullptr) {
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << OTHER_LAYERS_PRINT_SEQUENCE_ATTR << "\" " << VALUE_ATTR << "=\"";
                    const std::vector<int> &values = other_layers_print_sequence_opt->values;
                    for (int i = 0; i < values.size(); ++i) {
                        stream << values[i];
                        if (i != (values.size() - 1))
                            stream << " ";
                    }
                    stream << "\"/>\n";
                }

                const ConfigOptionInt *sequence_nums_opt = dynamic_cast<const ConfigOptionInt *>(plate_data->config.option("other_layers_print_sequence_nums"));
                if (sequence_nums_opt != nullptr) {
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << OTHER_LAYERS_PRINT_SEQUENCE_NUMS_ATTR << "\" " << VALUE_ATTR << "=\"" << sequence_nums_opt->getInt() << "\"/>\n";
                }

                ConfigOption* spiral_mode_opt = plate_data->config.option("spiral_mode");
                if (spiral_mode_opt)
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << SPIRAL_VASE_MODE << "\" " << VALUE_ATTR << "=\"" << spiral_mode_opt->getBool() << "\"/>\n";

                //filament map related
                ConfigOption* filament_map_mode_opt = plate_data->config.option("filament_map_mode");
                t_config_enum_names filament_map_mode_names = ConfigOptionEnum<FilamentMapMode>::get_enum_names();
                if (filament_map_mode_opt != nullptr && filament_map_mode_names.size() > filament_map_mode_opt->getInt())
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << FILAMENT_MAP_MODE_ATTR << "\" " << VALUE_ATTR << "=\"" << filament_map_mode_names[filament_map_mode_opt->getInt()] << "\"/>\n";

                ConfigOptionInts* filament_maps_opt = plate_data->config.option<ConfigOptionInts>("filament_map");
                // filament map override global settings only when group mode overrides the global settings
                if (filament_map_mode_opt !=nullptr && filament_maps_opt != nullptr) {
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << FILAMENT_MAP_ATTR << "\" " << VALUE_ATTR << "=\"";
                    const std::vector<int>& values = filament_maps_opt->values;
                    for (int i = 0; i < values.size(); ++i) {
                        stream << values[i];
                        if (i != (values.size() - 1))
                            stream << " ";
                    }
                    stream << "\"/>\n";
                }

                if (save_gcode)
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << GCODE_FILE_ATTR << "\" " << VALUE_ATTR << "=\"" << std::boolalpha << xml_escape(plate_data->gcode_file) << "\"/>\n";
                if (!plate_data->gcode_file.empty()) {
                    gcode_paths.push_back(plate_data->gcode_file);
                }
                if (plate_data->plate_thumbnail.is_valid()) {
                    std::string thumbnail_file_in_3mf = (boost::format(THUMBNAIL_FILE_FORMAT) % (plate_data->plate_index + 1)).str();
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << THUMBNAIL_FILE_ATTR << "\" " << VALUE_ATTR << "=\"" << std::boolalpha << thumbnail_file_in_3mf << "\"/>\n";
                }
                else if (!plate_data->thumbnail_file.empty() && (boost::filesystem::exists(plate_data->thumbnail_file))){
                    std::string thumbnail_file_in_3mf = (boost::format(THUMBNAIL_FILE_FORMAT) % (plate_data->plate_index + 1)).str();
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << THUMBNAIL_FILE_ATTR << "\" " << VALUE_ATTR << "=\"" << std::boolalpha << thumbnail_file_in_3mf << "\"/>\n";
                }

                if (!plate_data->no_light_thumbnail_file.empty()){
                    std::string no_light_thumbnail_file_in_3mf = (boost::format(NO_LIGHT_THUMBNAIL_FILE_FORMAT) % (plate_data->plate_index + 1)).str();
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << NO_LIGHT_THUMBNAIL_FILE_ATTR << "\" " << VALUE_ATTR << "=\"" << std::boolalpha << no_light_thumbnail_file_in_3mf << "\"/>\n";
                }

                if (!plate_data->top_file.empty()) {
                    std::string top_file_in_3mf = (boost::format(TOP_FILE_FORMAT) % (plate_data->plate_index + 1)).str();
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << TOP_FILE_ATTR << "\" " << VALUE_ATTR << "=\"" << std::boolalpha << top_file_in_3mf << "\"/>\n";
                }

                if (!plate_data->pick_file.empty()) {
                    std::string pick_file_in_3mf = (boost::format(PICK_FILE_FORMAT) % (plate_data->plate_index + 1)).str();
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << PICK_FILE_ATTR << "\" " << VALUE_ATTR << "=\"" << std::boolalpha << pick_file_in_3mf << "\"/>\n";
                }

                /*if (!plate_data->pattern_file.empty()) {
                    std::string pattern_file_in_3mf = (boost::format(PATTERN_FILE_FORMAT) % (plate_data->plate_index + 1)).str();
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << PATTERN_FILE_ATTR << "\" " << VALUE_ATTR << "=\"" << std::boolalpha << pattern_file_in_3mf << "\"/>\n";
                }*/
                if (!plate_data->pattern_bbox_file.empty()) {
                    std::string pattern_bbox_file_in_3mf = (boost::format(PATTERN_CONFIG_FILE_FORMAT) % (plate_data->plate_index + 1)).str();
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << PATTERN_BBOX_FILE_ATTR << "\" " << VALUE_ATTR << "=\"" << std::boolalpha << pattern_bbox_file_in_3mf << "\"/>\n";
                }

                if (!m_skip_model && instance_size > 0)
                {
                    for (unsigned int j = 0; j < instance_size; ++j)
                    {
                        stream << "    <" << INSTANCE_TAG << ">\n";
                        int obj_id = plate_data->objects_and_instances[j].first;
                        int inst_id = plate_data->objects_and_instances[j].second;
                        int identify_id = 0;
                        ModelObject* obj = NULL;
                        ModelInstance* inst = NULL;
                        if (obj_id >= model.objects.size()) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ":" << __LINE__ << boost::format("invalid object id %1%\n")%obj_id;
                        }
                        else
                            obj =  model.objects[obj_id];

                        if (obj && (inst_id >= obj->instances.size())) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ":" << __LINE__ << boost::format("invalid instance id %1%\n")%inst_id;
                        }
                        else if (obj){
                            inst =  obj->instances[inst_id];
                            if (use_loaded_id && (inst->loaded_id > 0))
                                identify_id = inst->loaded_id;
                            else
                                identify_id = inst->id().id;
                        }
                        obj_id = objects_data.find(obj)->second.object_id;

                        stream << "      <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << OBJECT_ID_ATTR << "\" " << VALUE_ATTR << "=\"" << obj_id << "\"/>\n";
                        stream << "      <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << INSTANCEID_ATTR << "\" " << VALUE_ATTR << "=\"" << inst_id << "\"/>\n";
                        stream << "      <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << IDENTIFYID_ATTR << "\" " << VALUE_ATTR << "=\"" << identify_id << "\"/>\n";
                        stream << "    </" << INSTANCE_TAG << ">\n";
                    }
                }
                stream << "  </" << PLATE_TAG << ">\n";
            }
        }

        // write model rels
        if (save_gcode)
            _add_relationships_file_to_archive(archive, BBS_MODEL_CONFIG_RELS_FILE, gcode_paths, {"http://schemas.bambulab.com/package/2021/gcode"}, Slic3r::PackingTemporaryData(), export_plate_idx);

        if (!m_skip_model) {
        //BBS: store assemble related info
        stream << "  <" << ASSEMBLE_TAG << ">\n";
        for (const ObjectToObjectDataMap::value_type& obj_metadata : objects_data) {
            auto object_data = obj_metadata.second;
            const ModelObject* obj = object_data.object;
            if (obj != nullptr) {
                for (int instance_idx = 0; instance_idx < obj->instances.size(); ++instance_idx) {
                    if (obj->instances[instance_idx]->is_assemble_initialized()) {
                        stream << "   <" << ASSEMBLE_ITEM_TAG << " " << OBJECT_ID_ATTR << "=\"" << object_data.object_id << "\" ";
                        stream << INSTANCEID_ATTR << "=\"" << instance_idx << "\" " << TRANSFORM_ATTR << "=\"";
                            for (unsigned c = 0; c < 4; ++c) {
                                for (unsigned r = 0; r < 3; ++r) {
                                    const Transform3d assemble_trans = obj->instances[instance_idx]->get_assemble_transformation().get_matrix();
                                    stream << assemble_trans(r, c);
                                    if (r != 2 || c != 3)
                                        stream << " ";
                                }
                            }

                        stream << "\" ";

                        stream << OFFSET_ATTR << "=\"";
                        const Vec3d ofs2ass = obj->instances[instance_idx]->get_offset_to_assembly();
                        stream << ofs2ass(0) << " " << ofs2ass(1) << " " << ofs2ass(2);
                    stream << "\" />\n";
                    }
                }
            }
        }
        stream << "  </" << ASSEMBLE_TAG << ">\n";
        }

        stream << "</" << CONFIG_TAG << ">\n";

        std::string out = stream.str();
        if (!mz_zip_writer_add_mem(&archive, BBS_MODEL_CONFIG_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format("Unable to add model config file to archive\n");
            add_error("Unable to add model config file to archive");
            return false;
        }

        return true;
    }

    bool _BBS_3MF_Exporter::_add_slice_info_config_file_to_archive(mz_zip_archive& archive, const Model& model, PlateDataPtrs& plate_data_list, const ObjectToObjectDataMap &objects_data, const DynamicPrintConfig& config)
    {
        std::stringstream stream;
        // Store mesh transformation in full precision, as the volumes are stored transformed and they need to be transformed back
        // when loaded as accurately as possible.
		stream << std::setprecision(std::numeric_limits<double>::max_digits10);
        stream << std::setiosflags(std::ios::fixed) << std::setprecision(2);
        stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        stream << "<" << CONFIG_TAG << ">\n";

        // save slice header for debug
        stream << "  <" << SLICE_HEADER_TAG << ">\n";
        stream << "    <" << SLICE_HEADER_ITEM_TAG << " " << KEY_ATTR << "=\"" << "X-BBL-Client-Type"    << "\" " << VALUE_ATTR << "=\"" << "slicer" << "\"/>\n";
        stream << "    <" << SLICE_HEADER_ITEM_TAG << " " << KEY_ATTR << "=\"" << "X-BBL-Client-Version" << "\" " << VALUE_ATTR << "=\"" << convert_to_full_version(SoftFever_VERSION) << "\"/>\n";
        stream << "  </" << SLICE_HEADER_TAG << ">\n";

        for (unsigned int i = 0; i < (unsigned int)plate_data_list.size(); ++i)
        {
            PlateData* plate_data = plate_data_list[i];
            //int instance_size = plate_data->objects_and_instances.size();

            if (plate_data != nullptr && plate_data->is_sliced_valid) {
                stream << "  <" << PLATE_TAG << ">\n";
                //plate index
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << PLATE_IDX_ATTR        << "\" " << VALUE_ATTR << "=\"" << plate_data->plate_index + 1 << "\"/>\n";

                int timelapse_type = int(config.opt_enum<TimelapseType>("timelapse_type"));
                for (auto it = plate_data->warnings.begin(); it != plate_data->warnings.end(); it++) {
                    if (it->msg == NOT_GENERATE_TIMELAPSE) {
                        timelapse_type = -1;
                        break;
                    }
                }

                std::vector<int> extruder_types      = config.option<ConfigOptionEnumsGeneric>("extruder_type")->values;
                std::vector<int> nozzle_volume_types = config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values;

                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << EXTRUDER_TYPE_ATTR << "\" " << VALUE_ATTR << "=\"";
                add_vector(stream, extruder_types);
                stream << "\"/>\n";

                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << NOZZLE_VOLUME_TYPE_ATTR << "\" " << VALUE_ATTR << "=\"";
                add_vector(stream, nozzle_volume_types);
                stream << "\"/>\n";

                auto* nozzle_diameter_option = dynamic_cast<const ConfigOptionFloats*>(config.option("nozzle_diameter"));
                std::string nozzle_diameters_str;
                if (nozzle_diameter_option)
                    nozzle_diameters_str = nozzle_diameter_option->serialize();

                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << PRINTER_MODEL_ID_ATTR       << "\" " << VALUE_ATTR << "=\"" << plate_data->printer_model_id << "\"/>\n";
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << NOZZLE_DIAMETERS_ATTR       << "\" " << VALUE_ATTR << "=\"" << nozzle_diameters_str << "\"/>\n";
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << TIMELAPSE_TYPE_ATTR << "\" " << VALUE_ATTR << "=\"" << timelapse_type << "\"/>\n";
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << SLICE_PREDICTION_ATTR << "\" " << VALUE_ATTR << "=\"" << plate_data->get_gcode_prediction_str() << "\"/>\n";
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << SLICE_WEIGHT_ATTR      << "\" " << VALUE_ATTR << "=\"" <<  plate_data->get_gcode_weight_str() << "\"/>\n";
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << FIRST_LAYER_TIME_ATTR      << "\" " << VALUE_ATTR << "=\"" <<  plate_data->first_layer_time << "\"/>\n";
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << OUTSIDE_ATTR      << "\" " << VALUE_ATTR << "=\"" << std::boolalpha<< plate_data->toolpath_outside << "\"/>\n";
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << SUPPORT_USED_ATTR << "\" " << VALUE_ATTR << "=\"" << std::boolalpha<< plate_data->is_support_used << "\"/>\n";
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << LABEL_OBJECT_ENABLED_ATTR << "\" " << VALUE_ATTR << "=\"" << std::boolalpha<< plate_data->is_label_object_enabled << "\"/>\n";

                std::vector<int> filament_maps = plate_data->filament_maps;
                if (filament_maps.empty())
                    filament_maps = config.option<ConfigOptionInts>("filament_map")->values;
                stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << FILAMENT_MAP_ATTR << "\" " << VALUE_ATTR << "=\"";
                add_vector<int>(stream, filament_maps);
                stream << "\"/>\n";

                if (plate_data->limit_filament_maps.size() > 0) {
                    stream << "    <" << METADATA_TAG << " " << KEY_ATTR << "=\"" << LIMIT_FILAMENT_MAP_ATTR << "\" " << VALUE_ATTR << "=\"";
                    add_vector<int>(stream, plate_data->limit_filament_maps);
                    stream << "\"/>\n";
                }

                for (auto it = plate_data->objects_and_instances.begin(); it != plate_data->objects_and_instances.end(); it++)
                {
                        int obj_id = it->first;
                        int inst_id = it->second;
                        int identify_id = 0;
                        ModelObject* obj = NULL;
                        ModelInstance* inst = NULL;
                        if (obj_id >= model.objects.size()) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ":" << __LINE__ << boost::format("invalid object id %1%\n")%obj_id;
                            continue;
                        }
                        obj =  model.objects[obj_id];

                        if (obj && (inst_id >= obj->instances.size())) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ":" << __LINE__ << boost::format("invalid instance id %1%\n")%inst_id;
                            continue;
                        }
                        inst =  obj->instances[inst_id];
                        if (m_use_loaded_id && (inst->loaded_id > 0))
                            identify_id = inst->loaded_id;
                        else
                            identify_id = inst->id().id;
                        bool skipped = std::find(plate_data->skipped_objects.begin(), plate_data->skipped_objects.end(), identify_id) !=
                                       plate_data->skipped_objects.end();
                        stream << "    <" << OBJECT_TAG << " " << IDENTIFYID_ATTR << "=\"" << std::to_string(identify_id) << "\" " << NAME_ATTR << "=\"" << xml_escape(obj->name)
                               << "\" " << SKIPPED_ATTR << "=\"" << (skipped ? "true" : "false")
                               << "\" />\n";
                }

                for (auto it = plate_data->slice_filaments_info.begin(); it != plate_data->slice_filaments_info.end(); it++)
                {
                    stream << "    <" << FILAMENT_TAG << " " << FILAMENT_ID_TAG << "=\"" << std::to_string(it->id + 1) << "\" "
                           << FILAMENT_TRAY_INFO_ID_TAG <<"=\""<< it->filament_id <<"\" "
                           << FILAMENT_TYPE_TAG << "=\"" << it->type << "\" "
                           << FILAMENT_COLOR_TAG << "=\"" << it->color << "\" "
                           << FILAMENT_USED_M_TAG << "=\"" << it->used_m << "\" "
                           << FILAMENT_USED_G_TAG << "=\"" << it->used_g << "\" />\n";
                }

                for (auto it = plate_data->warnings.begin(); it != plate_data->warnings.end(); it++) {
                    stream << "    <" << SLICE_WARNING_TAG << " msg=\"" << it->msg << "\" level=\"" << std::to_string(it->level) << "\" error_code =\"" << it->error_code << "\"  />\n";
                }

                if (!plate_data->layer_filaments.empty()) {
                    stream << "    <" << LAYER_FILAMENT_LISTS_TAG << ">\n";
                    for (auto iter = plate_data->layer_filaments.begin(); iter != plate_data->layer_filaments.end(); ++iter) {
                        // key
                        std::vector<unsigned int> sequence = iter->first;
                        std::stringstream         key_stream;
                        add_vector(key_stream, sequence);

                        // value
                        std::vector<std::pair<int, int>> ranges = iter->second;
                        std::stringstream                value_stream;
                        for (size_t i = 0; i < ranges.size(); ++i) {
                            value_stream << ranges[i].first;
                            value_stream << " ";
                            value_stream << ranges[i].second;
                            if (i != (ranges.size() - 1)) value_stream << ",";
                        }

                        stream << "      <" << LAYER_FILAMENT_LIST_TAG << " filament_list=\"" << key_stream.str() << "\" layer_ranges=\"" << value_stream.str() << "\" />\n";
                    }
                    stream << "    </" << LAYER_FILAMENT_LISTS_TAG << ">\n";
                }

                stream << "  </" << PLATE_TAG << ">\n";
            }
        }
        stream << "</" << CONFIG_TAG << ">\n";

        std::string out = stream.str();

        if (!mz_zip_writer_add_mem(&archive, SLICE_INFO_CONFIG_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
            add_error("Unable to add model config file to archive");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", store  slice-info to 3mf,  length %1%, failed\n") % out.length();
            return false;
        }

        return true;
    }
bool _BBS_3MF_Exporter::_add_gcode_file_to_archive(mz_zip_archive& archive, const Model& model, PlateDataPtrs& plate_data_list, Export3mfProgressFn proFn)
{
    bool result = true;
    bool cb_cancel = false;

    PlateDataPtrs plate_data_list2;
    for (unsigned int i = 0; i < (unsigned int)plate_data_list.size(); ++i)
    {
        if (proFn) {
            proFn(EXPORT_STAGE_ADD_GCODE, i, plate_data_list.size(), cb_cancel);
            if (cb_cancel)
                return false;
        }

        PlateData* plate_data = plate_data_list[i];
        if (!plate_data->gcode_file.empty() && plate_data->is_sliced_valid && boost::filesystem::exists(plate_data->gcode_file)) {
            plate_data_list2.push_back(plate_data);
        }
    }

    boost::mutex mutex;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, plate_data_list2.size(), 1), [this, &plate_data_list2, &root_archive = archive, &mutex, &result](const tbb::blocked_range<size_t>& range) {
        for (int i = range.begin(); i < range.end(); ++i) {
            PlateData* plate_data = plate_data_list2[i];
            auto src_gcode_file = plate_data->gcode_file;
            std::string gcode_in_3mf = (boost::format(GCODE_FILE_FORMAT) % (plate_data->plate_index + 1)).str();

            plate_data->gcode_file = gcode_in_3mf;
            mz_zip_archive archive;
            mz_zip_writer_staged_context context;
            mz_zip_zero_struct(&archive);
            mz_zip_writer_init_heap(&archive, 0, 1024 * 1024);
            {
                mz_zip_writer_add_staged_open(&archive, &context, gcode_in_3mf.c_str(), m_zip64 ? (uint64_t(1) << 30) * 16 : (uint64_t(1) << 32) - 1, nullptr, nullptr, 0,
                    MZ_DEFAULT_COMPRESSION, nullptr, 0, nullptr, 0);
                boost::filesystem::path src_gcode_path(src_gcode_file);
                if (!boost::filesystem::exists(src_gcode_path)) {
                    BOOST_LOG_TRIVIAL(error) << "Gcode is missing, filename = " << src_gcode_file;
                    result = false;
                }
                boost::filesystem::ifstream ifs(src_gcode_file, std::ios::binary);
                std::string buf(64 * 1024, 0);
                while (ifs) {
                    ifs.read(buf.data(), buf.size());
                    mz_zip_writer_add_staged_data(&context, buf.data(), ifs.gcount());
                }
                mz_zip_writer_add_staged_finish(&context);
            }
            void *ppBuf; size_t pSize;
            mz_zip_writer_finalize_heap_archive(&archive, &ppBuf, &pSize);
            mz_zip_writer_end(&archive);
            mz_zip_zero_struct(&archive);
            mz_zip_reader_init_mem(&archive, ppBuf, pSize, 0);
            {
                boost::unique_lock l(mutex);
                mz_zip_writer_add_from_zip_reader(&root_archive, &archive, 0);
            }
            mz_zip_reader_end(&archive);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" <<__LINE__ << boost::format(", store  %1% to 3mf %2%\n") % src_gcode_file % gcode_in_3mf;
        }
    });
    return result;
}

bool _BBS_3MF_Exporter::_add_custom_gcode_per_print_z_file_to_archive(mz_zip_archive& archive, Model& model, const DynamicPrintConfig* config)
{
    //BBS: add plate tree related logic
    std::string out = "";
    bool has_custom_gcode = false;
    pt::ptree tree;
    pt::ptree& main_tree = tree.add("custom_gcodes_per_layer", "");
    for (auto custom_gcodes : model.plates_custom_gcodes) {
            has_custom_gcode = true;
            pt::ptree& plate_tree = main_tree.add("plate", "");
            pt::ptree& plate_idx_tree = plate_tree.add("plate_info", "");
            plate_idx_tree.put("<xmlattr>.id", custom_gcodes.first + 1);

            // store data of custom_gcode_per_print_z
            for (const CustomGCode::Item& code : custom_gcodes.second.gcodes) {
                pt::ptree& code_tree = plate_tree.add("layer", "");
                code_tree.put("<xmlattr>.top_z", code.print_z);
                code_tree.put("<xmlattr>.type", static_cast<int>(code.type));
                code_tree.put("<xmlattr>.extruder", code.extruder);
                code_tree.put("<xmlattr>.color", code.color);
                code_tree.put("<xmlattr>.extra", code.extra);

                //BBS
                std::string gcode = //code.type == CustomGCode::ColorChange ? config->opt_string("color_change_gcode")    :
                    code.type == CustomGCode::PausePrint ? config->opt_string("machine_pause_gcode") :
                    code.type == CustomGCode::Template ? config->opt_string("template_custom_gcode") :
                    code.type == CustomGCode::ToolChange ? "tool_change" : code.extra;
                code_tree.put("<xmlattr>.gcode", gcode);
            }

            pt::ptree& mode_tree = plate_tree.add("mode", "");
            // store mode of a custom_gcode_per_print_z
            mode_tree.put("<xmlattr>.value", custom_gcodes.second.mode == CustomGCode::Mode::SingleExtruder ? CustomGCode::SingleExtruderMode :
                custom_gcodes.second.mode == CustomGCode::Mode::MultiAsSingle ? CustomGCode::MultiAsSingleMode :
                CustomGCode::MultiExtruderMode);

    }
    if (has_custom_gcode) {
        std::ostringstream oss;
        boost::property_tree::write_xml(oss, tree);
        out = oss.str();

        // Post processing("beautification") of the output string
        boost::replace_all(out, "><", ">\n<");
    }

    if (!out.empty()) {
        if (!mz_zip_writer_add_mem(&archive, CUSTOM_GCODE_PER_PRINT_Z_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION)) {
            add_error("Unable to add custom Gcodes per print_z file to archive");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":" << __LINE__ << boost::format(", Unable to add custom Gcodes per print_z file to archive\n");
            return false;
        }
    }

    return true;
}

bool _BBS_3MF_Exporter::_add_auxiliary_dir_to_archive(mz_zip_archive &archive, const std::string &aux_dir, PackingTemporaryData &data)
{
    bool result = true;

    if (aux_dir.empty()) {
        //no accessory directories
        return result;
    }

    boost::filesystem::path dir = boost::filesystem::path(aux_dir);
    if (!boost::filesystem::exists(dir))
    {
        //no accessory directories
        return result;
    }

    static std::string const nocomp_exts[] = {".png", ".jpg", ".mp4", ".jpeg"};
    std::deque<boost::filesystem::path> directories({dir});
    int root_dir_len = dir.string().length() + 1;
    //boost file access
    while (!directories.empty()) {
        boost::system::error_code ec;
        boost::filesystem::directory_iterator iterator(directories.front(), ec);
        directories.pop_front();
        if (ec) continue;
        for (; iterator != end(iterator); iterator.increment(ec))
        {
            if (ec) break;
            auto dir_entry = *iterator;
            std::string src_file;
            std::string dst_in_3mf;
            if (boost::filesystem::is_directory(dir_entry.path(), ec))
            {
                directories.push_back(dir_entry.path());
                continue;
            }
            if (boost::filesystem::is_regular_file(dir_entry.path(), ec) && !m_skip_auxiliary)
            {
                src_file = dir_entry.path().string();
                dst_in_3mf = dir_entry.path().string();
                dst_in_3mf.replace(0, root_dir_len, AUXILIARY_DIR);
                std::replace(dst_in_3mf.begin(), dst_in_3mf.end(), '\\', '/');
                if (_3MF_COVER_FILE.compare(1, _3MF_COVER_FILE.length() - 1, dst_in_3mf) == 0) {
                    data._3mf_thumbnail = dst_in_3mf;
                } else if (!m_thumbnail_small.empty() && m_thumbnail_small.compare(1, m_thumbnail_small.length() - 1, dst_in_3mf) == 0) {
                    data._3mf_printer_thumbnail_small = dst_in_3mf;
                    if (m_thumbnail_middle == m_thumbnail_small) data._3mf_printer_thumbnail_middle = dst_in_3mf;
                } else if (!m_thumbnail_middle.empty() && m_thumbnail_middle.compare(1, m_thumbnail_middle.length() - 1, dst_in_3mf) == 0) {
                    data._3mf_printer_thumbnail_middle = dst_in_3mf;
                }
                result &= _add_file_to_archive(archive, dst_in_3mf, src_file);
            }
        }
    }

    return result;
}

// Perform conversions based on the config values available.
//FIXME provide a version of PrusaSlicer that stored the project file (3MF).
static void handle_legacy_project_loaded(unsigned int version_project_file, DynamicPrintConfig& config)
{
    if (! config.has("brim_object_gap")) {
        if (auto *opt_elephant_foot   = config.option<ConfigOptionFloat>("elefant_foot_compensation", false); opt_elephant_foot) {
            // Conversion from older PrusaSlicer which applied brim separation equal to elephant foot compensation.
            auto *opt_brim_separation = config.option<ConfigOptionFloat>("brim_object_gap", true);
            opt_brim_separation->value = opt_elephant_foot->value;
        }
    }
}

// backup backgroud thread to dispatch tasks and coperate with ui thread
class _BBS_Backup_Manager
{
public:
    static _BBS_Backup_Manager& get() {
        static _BBS_Backup_Manager m;
        return m;
    }

    void set_post_callback(std::function<void(int)> c) {
        boost::lock_guard lock(m_mutex);
        m_post_callback = c;
    }

    void run_ui_tasks() {
        std::deque<Task> tasks;
        {
            boost::lock_guard lock(m_mutex);
            std::swap(tasks, m_ui_tasks);
        }
        for (auto& t : tasks)
        {
            process_ui_task(t);
        }
    }

    void push_object_gaurd(ModelObject& object) {
        m_gaurd_objects.push_back(std::make_pair(&object, 0));
    }

    void pop_object_gaurd() {
        auto object = m_gaurd_objects.back();
        m_gaurd_objects.pop_back();
        if (object.second)
            add_object_mesh(*object.first);
    }

    void add_object_mesh(ModelObject& object) {
        for (auto& g : m_gaurd_objects) {
            if (g.first == &object) {
                ++g.second;
                return;
            }
        }
        // clone object
        auto model = object.get_model();
        auto o = m_temp_model.add_object(object);
        int backup_id = model->get_object_backup_id(object);
        push_task({ AddObject, (size_t) backup_id, object.get_model()->get_backup_path(), o, 1 });
    }

    void remove_object_mesh(ModelObject& object) {
        push_task({ RemoveObject, object.id().id, object.get_model()->get_backup_path() });
    }

    void backup_soon() {
        boost::lock_guard lock(m_mutex);
        m_other_changes_backup = true;
        m_tasks.push_back({ Backup, 0, std::string(), nullptr, ++m_task_seq });
        m_cond.notify_all();
    }

    void remove_backup(Model& model, bool removeAll) {
        BOOST_LOG_TRIVIAL(info)
            << "remove_backup " << model.get_backup_path() << ", " << removeAll;
        std::deque<Task>   canceled_tasks;
        boost::unique_lock lock(m_mutex);
        if (removeAll && model.is_need_backup()) {
            // running task may not be canceled
            for (auto & t : m_ui_tasks)
                canceled_tasks.push_back(t);
            for (auto & t : m_tasks)
                canceled_tasks.push_back(t);
            m_ui_tasks.clear();
            m_tasks.clear();
        }
        m_tasks.push_back({ RemoveBackup, model.id().id, model.get_backup_path(), nullptr, removeAll });
        ++m_task_seq;
        if (model.is_need_backup()) {
            m_other_changes = false;
            m_other_changes_backup = false;
        }
        m_cond.notify_all();
        lock.unlock();
        for (auto& t : canceled_tasks) {
            process_ui_task(t, true);
        }
    }

    void set_interval(long n) {
        boost::lock_guard lock(m_mutex);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " entry, and last interval is: " << m_interval;
        m_next_backup -= boost::posix_time::seconds(m_interval);
        m_interval = n;
        m_next_backup += boost::posix_time::seconds(m_interval);
        m_cond.notify_all();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " exit, and new interval is: " << m_interval;
    }

    void put_other_changes()
    {
        BOOST_LOG_TRIVIAL(info) << "put_other_changes";
        m_other_changes        = true;
        m_other_changes_backup = true;
    }

    void clear_other_changes(bool backup)
    {
        if (backup)
            m_other_changes_backup = false;
        else
            m_other_changes = false;
    }

    bool has_other_changes(bool backup)
    {
        return backup ? m_other_changes_backup : m_other_changes;
    }

private:
    enum TaskType {
        None,
        Backup, // this task is working as response in ui thread
        AddObject,
        RemoveObject,
        RemoveBackup,
        Exit
    };
    struct Task {
        TaskType type;
        size_t id = 0;
        std::string path;
        ModelObject* object = nullptr;
        union {
        size_t delay = 0; // delay sequence, only last task is delayed
        size_t sequence;
        bool removeAll;
        };
        friend bool operator==(Task const& l, Task const& r) {
            return l.type == r.type && l.id == r.id;
        }
        std::string to_string() const {
            constexpr char const *type_names[] = {"None",
                                                  "Backup",
                                                  "AddObject",
                                                  "RemoveObject",
                                                  "RemoveBackup",
                                                  "Exit"};
            std::ostringstream os;
            os << "{ type:" << type_names[type] << ", id:" << id
               << ", path:" << path
               << ", object:" << (object ? object->id().id : 0) << ", extra:" << delay << "}";
            return os.str();
        }
    };

    struct timer {
        timer(char const * msg) : msg(msg), start(boost::posix_time::microsec_clock::universal_time()) { }
        ~timer() {
#ifdef __WIN32__
            auto end = boost::posix_time::microsec_clock::universal_time();
            int duration = (int)(end - start).total_milliseconds();
            char buf[20];
            OutputDebugStringA(msg);
            OutputDebugStringA(": ");
            OutputDebugStringA(itoa(duration, buf, 10));
            OutputDebugStringA("\n");
#endif
        }
        char const* msg;
        boost::posix_time::ptime start;
    };
private:
    _BBS_Backup_Manager() {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " inital and interval = " << m_interval;
        m_next_backup = boost::get_system_time() + boost::posix_time::seconds(m_interval);
        boost::unique_lock lock(m_mutex);
        m_thread = std::move(boost::thread(boost::ref(*this)));
    }

    ~_BBS_Backup_Manager() {
        push_task({Exit});
        m_thread.join();
    }

    void push_task(Task const & t) {
        boost::unique_lock lock(m_mutex);
        if (t.delay && !m_tasks.empty() && m_tasks.back() == t) {
            auto t2 = m_tasks.back();
            m_tasks.back() = t;
            m_tasks.back().delay = t2.delay + 1;
            m_cond.notify_all();
            lock.unlock();
            process_ui_task(t2);
        }
        else {
            m_tasks.push_back(t);
            ++m_task_seq;
            m_cond.notify_all();
        }
    }

    void process_ui_task(Task& t, bool canceled = false) {
        BOOST_LOG_TRIVIAL(info) << "process_ui_task" << t.to_string() << " and interval = " << m_interval;
        switch (t.type) {
            case Backup: {
                if (canceled)
                    break;
                std::function<void(int)> callback;
                boost::unique_lock lock(m_mutex);
                if (m_task_seq != t.sequence) {
                    if (find(m_tasks.begin(), m_tasks.end(), Task{ Backup }) == m_tasks.end()) {
                        t.sequence = ++m_task_seq; // may has pending tasks, retry later
                        m_tasks.push_back(t);
                        m_cond.notify_all();
                    }
                    break;
                }
                callback = m_post_callback;
                lock.unlock();
                {
                    timer t("backup cost");
                    try {
                        if (callback) callback(1);
                    } catch (...) {}
                }
                m_other_changes_backup = false;
                break;
            }
            case AddObject:
                m_temp_model.delete_object(t.object);
                break;
            case RemoveBackup:
                if (t.removeAll) {
                    try {
                        boost::filesystem::remove(t.path + "/lock.txt");
                        boost::filesystem::remove_all(t.path);
                        BOOST_LOG_TRIVIAL(info) << "process_ui_task: remove all of backup path " << t.path;
                    } catch (std::exception &ex) {
                        BOOST_LOG_TRIVIAL(error) << "process_ui_task: failed to remove backup path" << t.path << ": " << ex.what();
                    }
                }
                break;
        }
    }

    void process_task(Task& t) {
        BOOST_LOG_TRIVIAL(info) << "process_task" << t.to_string() << " and interval = " << m_interval;
        switch (t.type) {
            case Backup:
                // do it in response
                break;
            case AddObject: {
                {
                    CNumericLocalesSetter locales_setter;
                    _BBS_3MF_Exporter     e;
                    e.save_object_mesh(t.path, *t.object, (int) t.id);
                    // response to delete cloned object
                }
                break;
            }
            case RemoveObject: {
                boost::system::error_code ec;
                boost::filesystem::remove(t.path + "/mesh_" + boost::lexical_cast<std::string>(t.id) + ".xml", ec);
                t.type = None;
                break;
            }
            case RemoveBackup: {
                try {
                    boost::system::error_code ec;
                    boost::filesystem::remove(t.path + "/.3mf", ec);
                    // We Saved with SplitModel now, so we can safe delete these sub models.
                    boost::filesystem::remove_all(t.path + "/3D/Objects");
                    boost::filesystem::create_directory(t.path + "/3D/Objects");
                }
                catch (...) {}
            }
        }
    }

public:
    void operator()() {
        boost::unique_lock lock(m_mutex);
        while (true)
        {
            while (m_tasks.empty()) {
                if (m_interval > 0)
                    m_cond.timed_wait(lock, m_next_backup);
                else
                    m_cond.wait(lock);
                if (m_interval > 0 && boost::get_system_time() > m_next_backup) {
                    m_tasks.push_back({ Backup, 0, std::string(), nullptr, ++m_task_seq });
                    m_next_backup += boost::posix_time::seconds(m_interval);
                    // Maybe wakeup from power sleep
                    if (m_next_backup < boost::get_system_time())
                        m_next_backup = boost::get_system_time() + boost::posix_time::seconds(m_interval);
                }
            }
            Task t = m_tasks.front();
            if (t.type == Exit) break;
            if (t.object && t.delay) {
                if (!delay_task(t, lock))
                    continue;
            }
            m_tasks.pop_front();
            auto callback = m_post_callback;
            lock.unlock();
            process_task(t);
            lock.lock();
            if (t.type > None) {
                m_ui_tasks.push_back(t);
                if (m_ui_tasks.size() == 1 && callback)
                    callback(0);
            }
        }
    }

    bool delay_task(Task& t, boost::unique_lock<boost::mutex> & lock) {
        // delay last task for 3 seconds after last modify
        auto now = boost::get_system_time();
        auto delay_expire = now + boost::posix_time::seconds(10); // must not delay over this time
        auto wait = now + boost::posix_time::seconds(3);
        while (true) {
            m_cond.timed_wait(lock, wait);
            // Only delay when it's the only-one task
            if (m_tasks.size() != 1 || m_tasks.front().delay == t.delay)
                break;
            t.delay = m_tasks.front().delay;
            now = boost::get_system_time();
            if (now >= delay_expire)
                break;
            wait = now + boost::posix_time::seconds(3);
            if (wait > delay_expire)
                wait = delay_expire;
        };
        // task maybe canceled
        if (m_tasks.empty())
            return false;
        t = m_tasks.front();
        return true;
    }

private:
    boost::mutex m_mutex;
    boost::condition_variable m_cond;
    std::deque<Task> m_tasks;
    std::deque<Task> m_ui_tasks;
    size_t m_task_seq = 0;
    // param 0: should call run_ui_tasks
    // param 1: should backup current project
    std::function<void(int)> m_post_callback;
    long m_interval = 1 * 60;
    boost::system_time m_next_backup;
    Model m_temp_model; // visit only in main thread
    bool m_other_changes = false; // visit only in main thread
    bool m_other_changes_backup = false; // visit only in main thread
    std::vector<std::pair<ModelObject*, size_t>> m_gaurd_objects;
    boost::thread m_thread;
};


//BBS: add plate data list related logic
bool load_bbs_3mf(const char* path, DynamicPrintConfig* config, ConfigSubstitutionContext* config_substitutions, Model* model, PlateDataPtrs* plate_data_list, std::vector<Preset*>* project_presets,
                    bool* is_bbl_3mf, Semver* file_version, Import3mfProgressFn proFn, LoadStrategy strategy, BBLProject *project, int plate_id)
{
    if (path == nullptr || config == nullptr || model == nullptr)
        return false;

    // All import should use "C" locales for number formatting.
    CNumericLocalesSetter locales_setter;
    _BBS_3MF_Importer importer;
    bool res = importer.load_model_from_file(path, *model, *plate_data_list, *project_presets, *config, *config_substitutions, strategy, is_bbl_3mf, *file_version, proFn, project, plate_id);
    importer.log_errors();
    //BBS: remove legacy project logic currently
    //handle_legacy_project_loaded(importer.version(), *config);
    return res;
}

std::string bbs_3mf_get_thumbnail(const char *path)
{
    _BBS_3MF_Importer importer;
    std::string data;
    bool res = importer.get_thumbnail(path, data);
    if (!res) importer.log_errors();
    return data;
}

bool load_gcode_3mf_from_stream(std::istream &data, DynamicPrintConfig *config, Model *model, PlateDataPtrs *plate_data_list, Semver *file_version)
{
    CNumericLocalesSetter locales_setter;
    _BBS_3MF_Importer     importer;
    bool res = importer.load_gcode_3mf_from_stream(data, *model, *plate_data_list, *config, *file_version);
    importer.log_errors();
    return res;
}

bool store_bbs_3mf(StoreParams& store_params)
{
    // All export should use "C" locales for number formatting.
    CNumericLocalesSetter locales_setter;

    if (store_params.path == nullptr || store_params.model == nullptr)
        return false;

    _BBS_3MF_Exporter exporter;
    bool res = exporter.save_model_to_file(store_params);
    if (!res)
        exporter.log_errors();

    return res;
}

//BBS: release plate data list
void release_PlateData_list(PlateDataPtrs& plate_data_list)
{
    //clear
    for (unsigned int i = 0; i < plate_data_list.size(); i++)
    {
        delete plate_data_list[i];
    }
    plate_data_list.clear();

    return;
}

// backup interface

void save_object_mesh(ModelObject& object)
{
    if (!object.get_model() || !object.get_model()->is_need_backup())
        return;
    if (object.volumes.empty() || object.instances.empty())
        return;
    _BBS_Backup_Manager::get().add_object_mesh(object);
}

void delete_object_mesh(ModelObject& object)
{
    // not really remove
    // _BBS_Backup_Manager::get().remove_object_mesh(object);
}

void backup_soon()
{
    _BBS_Backup_Manager::get().backup_soon();
}

void remove_backup(Model& model, bool removeAll)
{
    _BBS_Backup_Manager::get().remove_backup(model, removeAll);
}

void set_backup_interval(long interval)
{
    _BBS_Backup_Manager::get().set_interval(interval);
}

void set_backup_callback(std::function<void(int)> callback)
{
    _BBS_Backup_Manager::get().set_post_callback(callback);
}

void run_backup_ui_tasks()
{
    _BBS_Backup_Manager::get().run_ui_tasks();
}

bool has_restore_data(std::string & path, std::string& origin)
{
    if (path.empty()) {
        origin = "<lock>";
        return false;
    }
    if (boost::filesystem::exists(path + "/lock.txt")) {
        std::string pid;
        load_string_file(path + "/lock.txt", pid);
        try {
            if (get_process_name(boost::lexical_cast<int>(pid)) ==
                get_process_name(0)) {
                origin = "<lock>";
                return false;
            }
        }
        catch (...) {
            return false;
        }
    }
    std::string file3mf = path + "/.3mf";
    if (!boost::filesystem::exists(file3mf))
        return false;
    try {
        if (boost::filesystem::exists(path + "/origin.txt"))
            load_string_file(path + "/origin.txt", origin);
    }
    catch (...) {
    }
    path = file3mf;
    return true;
}

void put_other_changes()
{
    _BBS_Backup_Manager::get().put_other_changes();
}

void clear_other_changes(bool backup)
{
    _BBS_Backup_Manager::get().clear_other_changes(backup);
}

bool has_other_changes(bool backup)
{
    return _BBS_Backup_Manager::get().has_other_changes(backup);
}

SaveObjectGaurd::SaveObjectGaurd(ModelObject& object)
{
    _BBS_Backup_Manager::get().push_object_gaurd(object);
}

SaveObjectGaurd::~SaveObjectGaurd()
{
    _BBS_Backup_Manager::get().pop_object_gaurd();
}

namespace{

// Conversion with bidirectional map
// F .. first, S .. second
template<typename F, typename S>
F bimap_cvt(const boost::bimap<F, S> &bmap, S s, const F & def_value) {
    const auto &map = bmap.right;
    auto found_item = map.find(s);

    // only for back and forward compatibility
    assert(found_item != map.end()); 
    if (found_item == map.end())
        return def_value;

    return found_item->second;
}

template<typename F, typename S> 
S bimap_cvt(const boost::bimap<F, S> &bmap, F f, const S &def_value)
{
    const auto &map = bmap.left;
    auto found_item = map.find(f);

    // only for back and forward compatibility
    assert(found_item != map.end());
    if (found_item == map.end())
        return def_value;

    return found_item->second;
}

} // namespace

/// <summary>
/// TextConfiguration serialization
/// </summary>
const TextConfigurationSerialization::TypeToName TextConfigurationSerialization::type_to_name =
    boost::assign::list_of<TypeToName::relation>
    (EmbossStyle::Type::file_path, "file_name")
    (EmbossStyle::Type::wx_win_font_descr, "wxFontDescriptor_Windows")
    (EmbossStyle::Type::wx_lin_font_descr, "wxFontDescriptor_Linux")
    (EmbossStyle::Type::wx_mac_font_descr, "wxFontDescriptor_MacOsX");

const TextConfigurationSerialization::HorizontalAlignToName TextConfigurationSerialization::horizontal_align_to_name =
    boost::assign::list_of<HorizontalAlignToName::relation>
    (FontProp::HorizontalAlign::left, "left")
    (FontProp::HorizontalAlign::center, "center")
    (FontProp::HorizontalAlign::right, "right");

const TextConfigurationSerialization::VerticalAlignToName TextConfigurationSerialization::vertical_align_to_name =
    boost::assign::list_of<VerticalAlignToName::relation>
    (FontProp::VerticalAlign::top, "top")
    (FontProp::VerticalAlign::center, "middle")
    (FontProp::VerticalAlign::bottom, "bottom");


void TextConfigurationSerialization::to_xml(std::stringstream &stream, const TextConfiguration &tc)
{
    stream << "   <" << TEXT_TAG << " ";

    stream << TEXT_DATA_ATTR << "=\"" << xml_escape_double_quotes_attribute_value(tc.text) << "\" ";
    // font item
    const EmbossStyle &style = tc.style;
    stream << STYLE_NAME_ATTR <<  "=\"" << xml_escape_double_quotes_attribute_value(style.name) << "\" ";
    stream << FONT_DESCRIPTOR_ATTR << "=\"" << xml_escape_double_quotes_attribute_value(style.path) << "\" ";
    constexpr std::string_view dafault_type{"undefined"};
    std::string_view style_type = bimap_cvt(type_to_name, style.type, dafault_type);
    stream << FONT_DESCRIPTOR_TYPE_ATTR << "=\"" << style_type << "\" ";

    // font property
    const FontProp &fp = tc.style.prop;
    if (fp.char_gap.has_value())
        stream << CHAR_GAP_ATTR << "=\"" << *fp.char_gap << "\" ";
    if (fp.line_gap.has_value())
        stream << LINE_GAP_ATTR << "=\"" << *fp.line_gap << "\" ";

    stream << LINE_HEIGHT_ATTR << "=\"" << fp.size_in_mm << "\" ";
    if (fp.boldness.has_value())
        stream << BOLDNESS_ATTR << "=\"" << *fp.boldness << "\" ";
    if (fp.skew.has_value())
        stream << SKEW_ATTR << "=\"" << *fp.skew << "\" ";
    if (fp.per_glyph)
        stream << PER_GLYPH_ATTR << "=\"" << 1 << "\" ";
    stream << HORIZONTAL_ALIGN_ATTR << "=\"" << bimap_cvt(horizontal_align_to_name, fp.align.first, dafault_type) << "\" ";
    stream << VERTICAL_ALIGN_ATTR   << "=\"" << bimap_cvt(vertical_align_to_name,  fp.align.second, dafault_type) << "\" ";
    if (fp.collection_number.has_value())
        stream << COLLECTION_NUMBER_ATTR << "=\"" << *fp.collection_number << "\" ";
    // font descriptor
    if (fp.family.has_value())
        stream << FONT_FAMILY_ATTR << "=\"" << *fp.family << "\" ";
    if (fp.face_name.has_value())
        stream << FONT_FACE_NAME_ATTR << "=\"" << *fp.face_name << "\" ";
    if (fp.style.has_value())
        stream << FONT_STYLE_ATTR << "=\"" << *fp.style << "\" ";
    if (fp.weight.has_value())
        stream << FONT_WEIGHT_ATTR << "=\"" << *fp.weight << "\" ";

    stream << "/>\n"; // end TEXT_TAG
}
namespace {

FontProp::HorizontalAlign read_horizontal_align(const char **attributes, unsigned int num_attributes, const TextConfigurationSerialization::HorizontalAlignToName& horizontal_align_to_name){
    std::string horizontal_align_str = bbs_get_attribute_value_string(attributes, num_attributes, HORIZONTAL_ALIGN_ATTR);

    // Back compatibility
    // PS 2.6.0 do not have align
    if (horizontal_align_str.empty())
        return FontProp::HorizontalAlign::center;

    // Back compatibility
    // PS 2.6.1 store indices(0|1|2) instead of text for align
    if (horizontal_align_str.length() == 1) {
        int horizontal_align_int = 0;
        if(boost::spirit::qi::parse(horizontal_align_str.c_str(), horizontal_align_str.c_str() + 1, boost::spirit::qi::int_, horizontal_align_int))
            return static_cast<FontProp::HorizontalAlign>(horizontal_align_int);
    }

    return bimap_cvt(horizontal_align_to_name, std::string_view(horizontal_align_str), FontProp::HorizontalAlign::center);    
}


FontProp::VerticalAlign read_vertical_align(const char **attributes, unsigned int num_attributes, const TextConfigurationSerialization::VerticalAlignToName& vertical_align_to_name){
    std::string vertical_align_str = bbs_get_attribute_value_string(attributes, num_attributes, VERTICAL_ALIGN_ATTR);

    // Back compatibility
    // PS 2.6.0 do not have align
    if (vertical_align_str.empty())
        return FontProp::VerticalAlign::center;

    // Back compatibility
    // PS 2.6.1 store indices(0|1|2) instead of text for align
    if (vertical_align_str.length() == 1) {
        int vertical_align_int = 0;
        if(boost::spirit::qi::parse(vertical_align_str.c_str(), vertical_align_str.c_str() + 1, boost::spirit::qi::int_, vertical_align_int))
            return static_cast<FontProp::VerticalAlign>(vertical_align_int);
    }

    return bimap_cvt(vertical_align_to_name, std::string_view(vertical_align_str), FontProp::VerticalAlign::center);
}

} // namespace

std::optional<TextConfiguration> TextConfigurationSerialization::read(const char **attributes, unsigned int num_attributes)
{
    FontProp fp;
    int char_gap = bbs_get_attribute_value_int(attributes, num_attributes, CHAR_GAP_ATTR);
    if (char_gap != 0) fp.char_gap = char_gap;
    int line_gap = bbs_get_attribute_value_int(attributes, num_attributes, LINE_GAP_ATTR); 
    if (line_gap != 0) fp.line_gap = line_gap;
    float boldness = bbs_get_attribute_value_float(attributes, num_attributes, BOLDNESS_ATTR);
    if (std::fabs(boldness) > std::numeric_limits<float>::epsilon())
        fp.boldness = boldness;
    float skew = bbs_get_attribute_value_float(attributes, num_attributes, SKEW_ATTR);
    if (std::fabs(skew) > std::numeric_limits<float>::epsilon())
        fp.skew = skew;
    int per_glyph = bbs_get_attribute_value_int(attributes, num_attributes, PER_GLYPH_ATTR);
    if (per_glyph == 1) fp.per_glyph = true;

    fp.align = FontProp::Align(
        read_horizontal_align(attributes, num_attributes, horizontal_align_to_name),
        read_vertical_align(attributes, num_attributes, vertical_align_to_name));

    int collection_number = bbs_get_attribute_value_int(attributes, num_attributes, COLLECTION_NUMBER_ATTR);
    if (collection_number > 0) fp.collection_number = static_cast<unsigned int>(collection_number);

    fp.size_in_mm = bbs_get_attribute_value_float(attributes, num_attributes, LINE_HEIGHT_ATTR);

    std::string family = bbs_get_attribute_value_string(attributes, num_attributes, FONT_FAMILY_ATTR);
    if (!family.empty()) fp.family = family;
    std::string face_name = bbs_get_attribute_value_string(attributes, num_attributes, FONT_FACE_NAME_ATTR);
    if (!face_name.empty()) fp.face_name = face_name;
    std::string style = bbs_get_attribute_value_string(attributes, num_attributes, FONT_STYLE_ATTR);
    if (!style.empty()) fp.style = style;
    std::string weight = bbs_get_attribute_value_string(attributes, num_attributes, FONT_WEIGHT_ATTR);
    if (!weight.empty()) fp.weight = weight;

    std::string style_name = bbs_get_attribute_value_string(attributes, num_attributes, STYLE_NAME_ATTR);
    std::string font_descriptor = bbs_get_attribute_value_string(attributes, num_attributes, FONT_DESCRIPTOR_ATTR);
    std::string type_str = bbs_get_attribute_value_string(attributes, num_attributes, FONT_DESCRIPTOR_TYPE_ATTR);
    EmbossStyle::Type type = bimap_cvt(type_to_name, std::string_view{type_str}, EmbossStyle::Type::undefined);

    std::string text = bbs_get_attribute_value_string(attributes, num_attributes, TEXT_DATA_ATTR);
    EmbossStyle es{style_name, std::move(font_descriptor), type, std::move(fp)};
    return TextConfiguration{std::move(es), std::move(text)};
}

EmbossShape TextConfigurationSerialization::read_old(const char **attributes, unsigned int num_attributes)
{
    EmbossShape es;
    std::string fix_tr_mat_str = bbs_get_attribute_value_string(attributes, num_attributes, TRANSFORM_ATTR);
    if (!fix_tr_mat_str.empty())
        es.fix_3mf_tr = bbs_get_transform_from_3mf_specs_string(fix_tr_mat_str);


    if (bbs_get_attribute_value_int(attributes, num_attributes, USE_SURFACE_ATTR) == 1)
        es.projection.use_surface = true;

    es.projection.depth = bbs_get_attribute_value_float(attributes, num_attributes, DEPTH_ATTR);

    int use_surface = bbs_get_attribute_value_int(attributes, num_attributes, USE_SURFACE_ATTR);
    if (use_surface == 1)
        es.projection.use_surface = true;

    return es;
}

namespace {
Transform3d create_fix(const std::optional<Transform3d> &prev, const ModelVolume &volume)
{
    // IMPROVE: check if volume was modified (translated, rotated OR scaled)
    // when no change do not calculate transformation only store original fix matrix

    // Create transformation used after load actual stored volume
    // Orca: do not bake volume transformation into meshes
    // const Transform3d &actual_trmat = volume.get_matrix();
    const Transform3d& actual_trmat = Transform3d::Identity();

    const auto &vertices = volume.mesh().its.vertices;
    Vec3d       min      = actual_trmat * vertices.front().cast<double>();
    Vec3d       max      = min;
    for (const Vec3f &v : vertices) {
        Vec3d vd = actual_trmat * v.cast<double>();
        for (size_t i = 0; i < 3; ++i) {
            if (min[i] > vd[i])
                min[i] = vd[i];
            if (max[i] < vd[i])
                max[i] = vd[i];
        }
    }
    Vec3d       center     = (max + min) / 2;
    Transform3d post_trmat = Transform3d::Identity();
    post_trmat.translate(center);

    Transform3d fix_trmat = actual_trmat.inverse() * post_trmat;
    if (!prev.has_value())
        return fix_trmat;

    // check whether fix somehow differ previous
    if (fix_trmat.isApprox(Transform3d::Identity(), 1e-5))
        return *prev;

    return *prev * fix_trmat;
}

bool to_xml(std::stringstream &stream, const EmbossShape::SvgFile &svg, const ModelVolume &volume, mz_zip_archive &archive, bool export_full_path)
{
    if (svg.path_in_3mf.empty())
        return true; // EmbossedText OR unwanted store .svg file into .3mf (protection of copyRight)

    if (!svg.path.empty()) {
        auto file_path =get_dealed_platform_path(svg.path);
        std::string input_file = xml_escape(export_full_path ? svg.path : file_path.filename().string());
        stream << SVG_FILE_PATH_ATTR << "=\"" << xml_escape_double_quotes_attribute_value(input_file) << "\" ";
    }
    stream << SVG_FILE_PATH_IN_3MF_ATTR << "=\"" << xml_escape_double_quotes_attribute_value(svg.path_in_3mf) << "\" ";

    std::shared_ptr<std::string> file_data = svg.file_data;
    assert(file_data != nullptr);
    if (file_data == nullptr && !svg.path.empty()) file_data = read_from_disk(svg.path);
    if (file_data == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << "Can't write svg file no filedata";
        return false;
    }
    const std::string &file_data_str = *file_data; 

    return mz_zip_writer_add_mem(&archive, svg.path_in_3mf.c_str(), 
        (const void *) file_data_str.c_str(), file_data_str.size(), MZ_DEFAULT_COMPRESSION);
}

} // namespace

void to_xml(std::stringstream &stream, const EmbossShape &es, const ModelVolume &volume, mz_zip_archive &archive, bool export_full_path)
{
    stream << "   <" << SHAPE_TAG << " ";
    if (es.svg_file.has_value())
        if (!to_xml(stream, *es.svg_file, volume, archive, export_full_path)) {
            BOOST_LOG_TRIVIAL(warning) << "Can't write svg file defiden embossed shape into 3mf";
        }

    stream << SHAPE_SCALE_ATTR << "=\"" << es.scale << "\" ";

    if (!es.final_shape.is_healed)
        stream << UNHEALED_ATTR << "=\"" << 1 << "\" ";

    // projection
    const EmbossProjection &p = es.projection;
    stream << DEPTH_ATTR << "=\"" << p.depth << "\" ";
    if (p.use_surface)
        stream << USE_SURFACE_ATTR << "=\"" << 1 << "\" ";
    
    // FIX of baked transformation
    Transform3d fix = create_fix(es.fix_3mf_tr, volume);
    stream << TRANSFORM_ATTR << "=\"";
    _BBS_3MF_Exporter::add_transformation(stream, fix);
    stream << "\" ";

    stream << "/>\n"; // end SHAPE_TAG    
}

std::optional<EmbossShape> read_emboss_shape(const char **attributes, unsigned int num_attributes) {    
    double scale = bbs_get_attribute_value_float(attributes, num_attributes, SHAPE_SCALE_ATTR);
    int unhealed = bbs_get_attribute_value_int(attributes, num_attributes, UNHEALED_ATTR);
    bool is_healed = unhealed != 1;

    EmbossProjection projection;
    projection.depth = bbs_get_attribute_value_float(attributes, num_attributes, DEPTH_ATTR);
    if (is_approx(projection.depth, 0.))
        projection.depth = 10.;

    int use_surface  = bbs_get_attribute_value_int(attributes, num_attributes, USE_SURFACE_ATTR);
    if (use_surface == 1)
        projection.use_surface = true;     

    std::optional<Transform3d> fix_tr_mat;
    std::string fix_tr_mat_str = bbs_get_attribute_value_string(attributes, num_attributes, TRANSFORM_ATTR);
    if (!fix_tr_mat_str.empty()) { 
        fix_tr_mat = bbs_get_transform_from_3mf_specs_string(fix_tr_mat_str);
    }

    std::string file_path = bbs_get_attribute_value_string(attributes, num_attributes, SVG_FILE_PATH_ATTR);
    std::string file_path_3mf = bbs_get_attribute_value_string(attributes, num_attributes, SVG_FILE_PATH_IN_3MF_ATTR);

    // MayBe: store also shapes to not store svg
    // But be carefull curve will be lost -> scale will not change sampling
    // shapes could be loaded from SVG
    ExPolygonsWithIds shapes; 
    // final shape could be calculated from shapes
    HealedExPolygons final_shape;
    final_shape.is_healed = is_healed;

    EmbossShape::SvgFile svg{file_path, file_path_3mf};
    return EmbossShape{std::move(shapes), std::move(final_shape), scale, std::move(projection), std::move(fix_tr_mat), std::move(svg)};
}


} // namespace Slic3r
