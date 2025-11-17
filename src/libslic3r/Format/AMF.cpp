#include <limits>
#include <string.h>
#include <map>
#include <string>
#include <expat.h>

#include <boost/nowide/cstdio.hpp>

#include "../libslic3r.h"
#include "../Exception.hpp"
#include "../Model.hpp"
#include "../GCode.hpp"
#include "../PrintConfig.hpp"
#include "../Utils.hpp"
#include "../I18N.hpp"
#include "../Geometry.hpp"
#include "../CustomGCode.hpp"
#include "../LocalesUtils.hpp"

#include "AMF.hpp"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
namespace pt = boost::property_tree;

#include <boost/filesystem/operations.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include "miniz_extension.hpp"

#if 0
// Enable debugging and assert in this file.
#define DEBUG
#define _DEBUG
#undef NDEBUG
#endif

#include <assert.h>

// VERSION NUMBERS
// 0 : .amf, .amf.xml and .zip.amf files saved by older slic3r. No version definition in them.
// 1 : Introduction of amf versioning. No other change in data saved into amf files.
// 2 : Added z component of offset
//     Added x and y components of rotation
//     Added x, y and z components of scale
//     Added x, y and z components of mirror
// 3 : Added volumes' matrices and source data, meshes transformed back to their coordinate system on loading.
// WARNING !! -> the version number has been rolled back to 2
//               the next change should use 4
//const unsigned int VERSION_AMF = 2;
//const unsigned int VERSION_AMF_COMPATIBLE = 3;
//const char* SLIC3RPE_AMF_VERSION = "slic3rpe_amf_version";
//
//const char* SLIC3R_CONFIG_TYPE = "slic3rpe_config";

namespace Slic3r
{

//! macro used to mark string used at localization,
//! return same string
#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

struct AMFParserContext
{
    AMFParserContext(XML_Parser parser, DynamicPrintConfig* config, ConfigSubstitutionContext* config_substitutions, Model* model) :
        m_parser(parser),
        m_model(*model),
        m_config(config),
        m_config_substitutions(config_substitutions)
    {
        m_path.reserve(12);
    }

    void stop(const std::string &msg = std::string())
    {
        assert(! m_error);
        assert(m_error_message.empty());
        m_error = true;
        m_error_message = msg;
        XML_StopParser(m_parser, 0);
    }

    bool        error()         const { return m_error; }
    const char* error_message() const {
        return m_error ?
            // The error was signalled by the user code, not the expat parser.
            (m_error_message.empty() ? "Parse AMF file failed" : m_error_message.c_str()) :
            // The error was signalled by the expat parser.
            XML_ErrorString(XML_GetErrorCode(m_parser));
    }

    void startElement(const char *name, const char **atts);
    void endElement(const char *name);
    void endDocument();
    void characters(const XML_Char *s, int len);

    static void XMLCALL startElement(void *userData, const char *name, const char **atts)
    {
        AMFParserContext *ctx = (AMFParserContext*)userData;
        ctx->startElement(name, atts);
    }

    static void XMLCALL endElement(void *userData, const char *name)
    {
        AMFParserContext *ctx = (AMFParserContext*)userData;
        ctx->endElement(name);
    }

    /* s is not 0 terminated. */
    static void XMLCALL characters(void *userData, const XML_Char *s, int len)
    {
        AMFParserContext *ctx = (AMFParserContext*)userData;
        ctx->characters(s, len);
    }

    static const char* get_attribute(const char **atts, const char *id) {
        if (atts == nullptr)
            return nullptr;
        while (*atts != nullptr) {
            if (strcmp(*(atts ++), id) == 0)
                return *atts;
            ++ atts;
        }
        return nullptr;
    }

    enum AMFNodeType {
        NODE_TYPE_INVALID = 0,
        NODE_TYPE_UNKNOWN,
        NODE_TYPE_AMF,                  // amf
                                        // amf/metadata
        NODE_TYPE_MATERIAL,             // amf/material
                                        // amf/material/metadata
        NODE_TYPE_OBJECT,               // amf/object
                                        // amf/object/metadata
       // NODE_TYPE_LAYER_CONFIG,         // amf/object/layer_config_ranges
        //NODE_TYPE_RANGE,                // amf/object/layer_config_ranges/range
                                        // amf/object/layer_config_ranges/range/metadata
        NODE_TYPE_MESH,                 // amf/object/mesh
        NODE_TYPE_VERTICES,             // amf/object/mesh/vertices
        NODE_TYPE_VERTEX,               // amf/object/mesh/vertices/vertex
        NODE_TYPE_COORDINATES,          // amf/object/mesh/vertices/vertex/coordinates
        NODE_TYPE_COORDINATE_X,         // amf/object/mesh/vertices/vertex/coordinates/x
        NODE_TYPE_COORDINATE_Y,         // amf/object/mesh/vertices/vertex/coordinates/y
        NODE_TYPE_COORDINATE_Z,         // amf/object/mesh/vertices/vertex/coordinates/z
        NODE_TYPE_VOLUME,               // amf/object/mesh/volume
                                        // amf/object/mesh/volume/metadata
        NODE_TYPE_TRIANGLE,             // amf/object/mesh/volume/triangle
        NODE_TYPE_VERTEX1,              // amf/object/mesh/volume/triangle/v1
        NODE_TYPE_VERTEX2,              // amf/object/mesh/volume/triangle/v2
        NODE_TYPE_VERTEX3,              // amf/object/mesh/volume/triangle/v3
        NODE_TYPE_CONSTELLATION,        // amf/constellation
        NODE_TYPE_INSTANCE,             // amf/constellation/instance
        NODE_TYPE_DELTAX,               // amf/constellation/instance/deltax
        NODE_TYPE_DELTAY,               // amf/constellation/instance/deltay
        NODE_TYPE_DELTAZ,               // amf/constellation/instance/deltaz
        NODE_TYPE_RX,                   // amf/constellation/instance/rx
        NODE_TYPE_RY,                   // amf/constellation/instance/ry
        NODE_TYPE_RZ,                   // amf/constellation/instance/rz
        NODE_TYPE_SCALE,                // amf/constellation/instance/scale
        NODE_TYPE_SCALEX,               // amf/constellation/instance/scalex
        NODE_TYPE_SCALEY,               // amf/constellation/instance/scaley
        NODE_TYPE_SCALEZ,               // amf/constellation/instance/scalez
        NODE_TYPE_MIRRORX,              // amf/constellation/instance/mirrorx
        NODE_TYPE_MIRRORY,              // amf/constellation/instance/mirrory
        NODE_TYPE_MIRRORZ,              // amf/constellation/instance/mirrorz
        NODE_TYPE_PRINTABLE,            // amf/constellation/instance/mirrorz
        //NODE_TYPE_CUSTOM_GCODE,         // amf/custom_code_per_height
        //NODE_TYPE_GCODE_PER_HEIGHT,     // amf/custom_code_per_height/code
        //NODE_TYPE_CUSTOM_GCODE_MODE,    // amf/custom_code_per_height/mode
        NODE_TYPE_METADATA,             // anywhere under amf/*/metadata
    };

    struct Instance {
        Instance()
            : deltax_set(false), deltay_set(false), deltaz_set(false)
            , rx_set(false), ry_set(false), rz_set(false)
            , scalex_set(false), scaley_set(false), scalez_set(false)
            , mirrorx_set(false), mirrory_set(false), mirrorz_set(false)
            , printable(true) {}
        // Shift in the X axis.
        float deltax;
        bool  deltax_set;
        // Shift in the Y axis.
        float deltay;
        bool  deltay_set;
        // Shift in the Z axis.
        float deltaz;
        bool  deltaz_set;
        // Rotation around the X axis.
        float rx;
        bool  rx_set;
        // Rotation around the Y axis.
        float ry;
        bool  ry_set;
        // Rotation around the Z axis.
        float rz;
        bool  rz_set;
        // Scaling factors
        float scalex;
        bool  scalex_set;
        float scaley;
        bool  scaley_set;
        float scalez;
        bool  scalez_set;
        // Mirroring factors
        float mirrorx;
        bool  mirrorx_set;
        float mirrory;
        bool  mirrory_set;
        float mirrorz;
        bool  mirrorz_set;
        // printable property
        bool  printable;

        bool anything_set() const { return deltax_set || deltay_set || deltaz_set ||
                                           rx_set || ry_set || rz_set ||
                                           scalex_set || scaley_set || scalez_set ||
                                           mirrorx_set || mirrory_set || mirrorz_set; }
    };

    struct Object {
        Object() : idx(-1) {}
        int                     idx;
        std::vector<Instance>   instances;
    };

    // Version of the amf file
    //unsigned int             m_version { 0 };
    // Current Expat XML parser instance.
    XML_Parser               m_parser;
    // Error code returned by the application side of the parser. In that case the expat may not reliably deliver the error state
    // after returning from XML_Parse() function, thus we keep the error state here.
    bool                     m_error { false };
    std::string              m_error_message;
    // Model to receive objects extracted from an AMF file.
    Model                   &m_model;
    // Current parsing path in the XML file.
    std::vector<AMFNodeType> m_path;
    // Current object allocated for an amf/object XML subtree.
    ModelObject             *m_object { nullptr };
    // Map from obect name to object idx & instances.
    std::map<std::string, Object> m_object_instances_map;
    // Vertices parsed for the current m_object.
    std::vector<Vec3f>       m_object_vertices;
    // Current volume allocated for an amf/object/mesh/volume subtree.
    ModelVolume             *m_volume { nullptr };
    // Faces collected for the current m_volume.
    std::vector<Vec3i32>       m_volume_facets;
    // Transformation matrix of a volume mesh from its coordinate system to Object's coordinate system.
    Transform3d 			 m_volume_transform;
    // Current material allocated for an amf/metadata subtree.
    ModelMaterial           *m_material { nullptr };
    // Current instance allocated for an amf/constellation/instance subtree.
    Instance                *m_instance { nullptr };
    // Generic string buffer for vertices, face indices, metadata etc.
    std::string              m_value[5];
    // Pointer to config to update if config data are stored inside the amf file
    DynamicPrintConfig      *m_config { nullptr };
    // Config substitution rules and collected config substitution log.
    ConfigSubstitutionContext *m_config_substitutions { nullptr };
    //BBS: add units logic
    bool                     m_use_inches { false };

private:
    AMFParserContext& operator=(AMFParserContext&);
};

void AMFParserContext::startElement(const char *name, const char **atts)
{
    AMFNodeType node_type_new = NODE_TYPE_UNKNOWN;
    switch (m_path.size()) {
    case 0: {
        // An AMF file must start with an <amf> tag.
        node_type_new = NODE_TYPE_AMF;
        if (strcmp(name, "amf") != 0) this->stop();
        // BBS: add units logic
        const char *units = get_attribute(atts, "unit");
        if (units && (strcmp(units, "inch") == 0)) { m_use_inches = true; }
        break;
    }
    case 1:
        if (strcmp(name, "metadata") == 0) {
            const char *type = get_attribute(atts, "type");
            if (type != nullptr) {
                m_value[0] = type;
                node_type_new = NODE_TYPE_METADATA;
            }
        } else if (strcmp(name, "material") == 0) {
            const char *material_id = get_attribute(atts, "id");
            m_material = m_model.add_material((material_id == nullptr) ? "_" : material_id);
            node_type_new = NODE_TYPE_MATERIAL;
        } else if (strcmp(name, "object") == 0) {
            const char *object_id = get_attribute(atts, "id");
            if (object_id == nullptr)
                this->stop();
            else {
				assert(m_object_vertices.empty());
                m_object = m_model.add_object();
                m_object_instances_map[object_id].idx = int(m_model.objects.size())-1;
                node_type_new = NODE_TYPE_OBJECT;
            }
        } else if (strcmp(name, "constellation") == 0) {
            node_type_new = NODE_TYPE_CONSTELLATION;
        }/*else if (strcmp(name, "custom_gcodes_per_height") == 0) {
            node_type_new = NODE_TYPE_CUSTOM_GCODE;
        }*/
        break;
    case 2:
        if (strcmp(name, "metadata") == 0) {
            if (m_path[1] == NODE_TYPE_MATERIAL || m_path[1] == NODE_TYPE_OBJECT) {
                m_value[0] = get_attribute(atts, "type");
                node_type_new = NODE_TYPE_METADATA;
            }
        }/* else if (strcmp(name, "layer_config_ranges") == 0 && m_path[1] == NODE_TYPE_OBJECT)
                node_type_new = NODE_TYPE_LAYER_CONFIG;*/
        else if (strcmp(name, "mesh") == 0) {
            if (m_path[1] == NODE_TYPE_OBJECT)
                node_type_new = NODE_TYPE_MESH;
        } else if (strcmp(name, "instance") == 0) {
            if (m_path[1] == NODE_TYPE_CONSTELLATION) {
                const char *object_id = get_attribute(atts, "objectid");
                if (object_id == nullptr)
                    this->stop();
                else {
                    m_object_instances_map[object_id].instances.push_back(AMFParserContext::Instance());
                    m_instance = &m_object_instances_map[object_id].instances.back();
                    node_type_new = NODE_TYPE_INSTANCE;
                }
            }
            else
                this->stop();
        }
        /*else if (m_path[1] == NODE_TYPE_CUSTOM_GCODE) {
            if (strcmp(name, "code") == 0) {
                node_type_new = NODE_TYPE_GCODE_PER_HEIGHT;
                m_value[0] = get_attribute(atts, "print_z");
                m_value[1] = get_attribute(atts, "extruder");
                m_value[2] = get_attribute(atts, "color");
                if (get_attribute(atts, "type"))
                {
                    m_value[3] = get_attribute(atts, "type");
                    m_value[4] = get_attribute(atts, "extra");
                }
                else
                {
                    // It means that data was saved in old version (2.2.0 and older) of PrusaSlicer
                    // read old data ...
                    std::string gcode = get_attribute(atts, "gcode");
                    // ... and interpret them to the new data
                    CustomGCode::Type type= gcode == "M600" ? CustomGCode::ColorChange :
                                            gcode == "M601" ? CustomGCode::PausePrint :
                                            gcode == "tool_change" ? CustomGCode::ToolChange : CustomGCode::Custom;
                    m_value[3] = std::to_string(static_cast<int>(type));
                    m_value[4] = type == CustomGCode::PausePrint ? m_value[2] :
                                 type == CustomGCode::Custom ? gcode : "";
                }
            }
            else if (strcmp(name, "mode") == 0) {
                node_type_new = NODE_TYPE_CUSTOM_GCODE_MODE;
                m_value[0] = get_attribute(atts, "value");
            }
        }*/
        break;
    case 3:
        if (m_path[2] == NODE_TYPE_MESH) {
			assert(m_object);
            if (strcmp(name, "vertices") == 0)
                node_type_new = NODE_TYPE_VERTICES;
			else if (strcmp(name, "volume") == 0) {
				assert(! m_volume);
                m_volume = m_object->add_volume(TriangleMesh());
                m_volume_transform = Transform3d::Identity();
                node_type_new = NODE_TYPE_VOLUME;
			}
        }
        else if (m_path[2] == NODE_TYPE_INSTANCE) {
            assert(m_instance);
            if (strcmp(name, "deltax") == 0)
                node_type_new = NODE_TYPE_DELTAX;
            else if (strcmp(name, "deltay") == 0)
                node_type_new = NODE_TYPE_DELTAY;
            else if (strcmp(name, "deltaz") == 0)
                node_type_new = NODE_TYPE_DELTAZ;
            else if (strcmp(name, "rx") == 0)
                node_type_new = NODE_TYPE_RX;
            else if (strcmp(name, "ry") == 0)
                node_type_new = NODE_TYPE_RY;
            else if (strcmp(name, "rz") == 0)
                node_type_new = NODE_TYPE_RZ;
            else if (strcmp(name, "scalex") == 0)
                node_type_new = NODE_TYPE_SCALEX;
            else if (strcmp(name, "scaley") == 0)
                node_type_new = NODE_TYPE_SCALEY;
            else if (strcmp(name, "scalez") == 0)
                node_type_new = NODE_TYPE_SCALEZ;
            else if (strcmp(name, "scale") == 0)
                node_type_new = NODE_TYPE_SCALE;
            else if (strcmp(name, "mirrorx") == 0)
                node_type_new = NODE_TYPE_MIRRORX;
            else if (strcmp(name, "mirrory") == 0)
                node_type_new = NODE_TYPE_MIRRORY;
            else if (strcmp(name, "mirrorz") == 0)
                node_type_new = NODE_TYPE_MIRRORZ;
            else if (strcmp(name, "printable") == 0)
                node_type_new = NODE_TYPE_PRINTABLE;
        }
        /*else if (m_path[2] == NODE_TYPE_LAYER_CONFIG && strcmp(name, "range") == 0) {
            assert(m_object);
            node_type_new = NODE_TYPE_RANGE;
        }*/
        break;
    case 4:
        if (m_path[3] == NODE_TYPE_VERTICES) {
            if (strcmp(name, "vertex") == 0)
                node_type_new = NODE_TYPE_VERTEX;
        } else if (m_path[3] == NODE_TYPE_VOLUME) {
            if (strcmp(name, "metadata") == 0) {
                const char *type = get_attribute(atts, "type");
                if (type == nullptr)
                    this->stop();
                else {
                    m_value[0] = type;
                    node_type_new = NODE_TYPE_METADATA;
                }
            } else if (strcmp(name, "triangle") == 0)
                node_type_new = NODE_TYPE_TRIANGLE;
        }
        /*else if (m_path[3] == NODE_TYPE_RANGE && strcmp(name, "metadata") == 0) {
            m_value[0] = get_attribute(atts, "type");
            node_type_new = NODE_TYPE_METADATA;
        }*/
        break;
    case 5:
        if (strcmp(name, "coordinates") == 0) {
            if (m_path[4] == NODE_TYPE_VERTEX) {
                node_type_new = NODE_TYPE_COORDINATES;
            } else
                this->stop();
        } else if (name[0] == 'v' && name[1] >= '1' && name[1] <= '3' && name[2] == 0) {
            if (m_path[4] == NODE_TYPE_TRIANGLE) {
                node_type_new = AMFNodeType(NODE_TYPE_VERTEX1 + name[1] - '1');
            } else
                this->stop();
        }
        break;
    case 6:
        if ((name[0] == 'x' || name[0] == 'y' || name[0] == 'z') && name[1] == 0) {
            if (m_path[5] == NODE_TYPE_COORDINATES)
                node_type_new = AMFNodeType(NODE_TYPE_COORDINATE_X + name[0] - 'x');
            else
                this->stop();
        }
        break;
    default:
        break;
    }

    m_path.push_back(node_type_new);
}

void AMFParserContext::characters(const XML_Char *s, int len)
{
    if (m_path.back() == NODE_TYPE_METADATA) {
        m_value[1].append(s, len);
    }
    else
    {
        switch (m_path.size()) {
        case 4:
            if (m_path.back() == NODE_TYPE_DELTAX ||
                m_path.back() == NODE_TYPE_DELTAY ||
                m_path.back() == NODE_TYPE_DELTAZ ||
                m_path.back() == NODE_TYPE_RX ||
                m_path.back() == NODE_TYPE_RY ||
                m_path.back() == NODE_TYPE_RZ ||
                m_path.back() == NODE_TYPE_SCALEX ||
                m_path.back() == NODE_TYPE_SCALEY ||
                m_path.back() == NODE_TYPE_SCALEZ ||
                m_path.back() == NODE_TYPE_SCALE ||
                m_path.back() == NODE_TYPE_MIRRORX ||
                m_path.back() == NODE_TYPE_MIRRORY ||
                m_path.back() == NODE_TYPE_MIRRORZ ||
                m_path.back() == NODE_TYPE_PRINTABLE)
                m_value[0].append(s, len);
            break;
        case 6:
            switch (m_path.back()) {
                case NODE_TYPE_VERTEX1: m_value[0].append(s, len); break;
                case NODE_TYPE_VERTEX2: m_value[1].append(s, len); break;
                case NODE_TYPE_VERTEX3: m_value[2].append(s, len); break;
                default: break;
            }
        case 7:
            switch (m_path.back()) {
                case NODE_TYPE_COORDINATE_X: m_value[0].append(s, len); break;
                case NODE_TYPE_COORDINATE_Y: m_value[1].append(s, len); break;
                case NODE_TYPE_COORDINATE_Z: m_value[2].append(s, len); break;
                default: break;
            }
        default:
            break;
        }
    }
}

void AMFParserContext::endElement(const char * /* name */)
{
    assert(is_decimal_separator_point());
    switch (m_path.back()) {

    // Constellation transformation:
    case NODE_TYPE_DELTAX:
        assert(m_instance);
        m_instance->deltax = float(atof(m_value[0].c_str()));
        m_instance->deltax_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_DELTAY:
        assert(m_instance);
        m_instance->deltay = float(atof(m_value[0].c_str()));
        m_instance->deltay_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_DELTAZ:
        assert(m_instance);
        m_instance->deltaz = float(atof(m_value[0].c_str()));
        m_instance->deltaz_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_RX:
        assert(m_instance);
        m_instance->rx = float(atof(m_value[0].c_str()));
        m_instance->rx_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_RY:
        assert(m_instance);
        m_instance->ry = float(atof(m_value[0].c_str()));
        m_instance->ry_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_RZ:
        assert(m_instance);
        m_instance->rz = float(atof(m_value[0].c_str()));
        m_instance->rz_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_SCALE:
        assert(m_instance);
        m_instance->scalex = float(atof(m_value[0].c_str()));
        m_instance->scalex_set = true;
        m_instance->scaley = float(atof(m_value[0].c_str()));
        m_instance->scaley_set = true;
        m_instance->scalez = float(atof(m_value[0].c_str()));
        m_instance->scalez_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_SCALEX:
        assert(m_instance);
        m_instance->scalex = float(atof(m_value[0].c_str()));
        m_instance->scalex_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_SCALEY:
        assert(m_instance);
        m_instance->scaley = float(atof(m_value[0].c_str()));
        m_instance->scaley_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_SCALEZ:
        assert(m_instance);
        m_instance->scalez = float(atof(m_value[0].c_str()));
        m_instance->scalez_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_MIRRORX:
        assert(m_instance);
        m_instance->mirrorx = float(atof(m_value[0].c_str()));
        m_instance->mirrorx_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_MIRRORY:
        assert(m_instance);
        m_instance->mirrory = float(atof(m_value[0].c_str()));
        m_instance->mirrory_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_MIRRORZ:
        assert(m_instance);
        m_instance->mirrorz = float(atof(m_value[0].c_str()));
        m_instance->mirrorz_set = true;
        m_value[0].clear();
        break;
    case NODE_TYPE_PRINTABLE:
        assert(m_instance);
        m_instance->printable = bool(atoi(m_value[0].c_str()));
        m_value[0].clear();
        break;

    // Object vertices:
    case NODE_TYPE_VERTEX:
        assert(m_object);
        // Parse the vertex data
        m_object_vertices.emplace_back(float(atof(m_value[0].c_str())), float(atof(m_value[1].c_str())), float(atof(m_value[2].c_str())));
        m_value[0].clear();
        m_value[1].clear();
        m_value[2].clear();
        break;

    // Faces of the current volume:
    case NODE_TYPE_TRIANGLE:
        assert(m_object && m_volume);
        m_volume_facets.emplace_back(atoi(m_value[0].c_str()), atoi(m_value[1].c_str()), atoi(m_value[2].c_str()));
        m_value[0].clear();
        m_value[1].clear();
        m_value[2].clear();
        break;

    // Closing the current volume. Create an STL from m_volume_facets pointing to m_object_vertices.
    case NODE_TYPE_VOLUME:
    {
		assert(m_object && m_volume);
        if (m_volume_facets.empty()) {
            this->stop("Found an empty triangle mesh");
            return;
        }

        {
            // Verify validity of face indices, find the vertex span.
            int min_id = m_volume_facets.front()[0];
            int max_id = min_id;
            for (const Vec3i32& face : m_volume_facets) {
                for (const int tri_id : face) {
                    if (tri_id < 0 || tri_id >= int(m_object_vertices.size())) {
                        this->stop("Found a malformed triangle mesh");
                        return;
                    }
                    min_id = std::min(min_id, tri_id);
                    max_id = std::max(max_id, tri_id);
                }
            }

            // rebase indices to the current vertices list
            for (Vec3i32 &face : m_volume_facets)
                face -= Vec3i32(min_id, min_id, min_id);

            indexed_triangle_set its { std::move(m_volume_facets), { m_object_vertices.begin() + min_id, m_object_vertices.begin() + max_id + 1 } };
            its_compactify_vertices(its);
            if (its_volume(its) < 0)
                its_flip_triangles(its);
            m_volume->set_mesh(std::move(its));
        }

        // stores the volume matrix taken from the metadata, if present
        if (bool has_transform = !m_volume_transform.isApprox(Transform3d::Identity(), 1e-10); has_transform)
            m_volume->source.transform = Slic3r::Geometry::Transformation(m_volume_transform);

        if (m_volume->source.input_file.empty() && (m_volume->type() == ModelVolumeType::MODEL_PART)) {
            m_volume->source.object_idx = (int)m_model.objects.size() - 1;
            m_volume->source.volume_idx = (int)m_model.objects.back()->volumes.size() - 1;
            m_volume->center_geometry_after_creation();
        } else
            // pass false if the mesh offset has been already taken from the data
            m_volume->center_geometry_after_creation(m_volume->source.input_file.empty());

        m_volume->calculate_convex_hull();
        m_volume_facets.clear();
        m_volume = nullptr;
        break;
    }

    case NODE_TYPE_OBJECT:
        assert(m_object);
        m_object_vertices.clear();
        m_object = nullptr;
        break;

    case NODE_TYPE_MATERIAL:
        assert(m_material);
        m_material = nullptr;
        break;

    case NODE_TYPE_INSTANCE:
        assert(m_instance);
        m_instance = nullptr;
        break;

    /*case NODE_TYPE_GCODE_PER_HEIGHT: {
        double print_z          = double(atof(m_value[0].c_str()));
        int extruder            = atoi(m_value[1].c_str());
        const std::string& color= m_value[2];
        CustomGCode::Type type  = static_cast<CustomGCode::Type>(atoi(m_value[3].c_str()));
        const std::string& extra= m_value[4];

        m_model.custom_gcode_per_print_z.gcodes.push_back(CustomGCode::Item{print_z, type, extruder, color, extra});

        for (std::string& val: m_value)
            val.clear();
        break;
        }

    case NODE_TYPE_CUSTOM_GCODE_MODE: {
        const std::string& mode = m_value[0];

        m_model.custom_gcode_per_print_z.mode = mode == CustomGCode::SingleExtruderMode ? CustomGCode::Mode::SingleExtruder :
                                                mode == CustomGCode::MultiAsSingleMode  ? CustomGCode::Mode::MultiAsSingle  :
                                                                                          CustomGCode::Mode::MultiExtruder;
        for (std::string& val: m_value)
            val.clear();
        break;
        }
        */
    case NODE_TYPE_METADATA:
        /*if ((m_config != nullptr) && strncmp(m_value[0].c_str(), SLIC3R_CONFIG_TYPE, strlen(SLIC3R_CONFIG_TYPE)) == 0) {
            //FIXME Loading a "will be one day a legacy format" of configuration in a form of a G-code comment.
            // Each config line is prefixed with a semicolon (G-code comment), that is ugly.

            // Replacing the legacy function with load_from_ini_string_commented leads to issues when
            // parsing 3MFs from before PrusaSlicer 2.0.0 (which can have duplicated entries in the INI.
            // See https://github.com/prusa3d/PrusaSlicer/issues/7155. We'll revert it for now.
            //m_config_substitutions->substitutions = m_config->load_from_ini_string_commented(std::move(m_value[1].c_str()), m_config_substitutions->rule);
            ConfigBase::load_from_gcode_string_legacy(*m_config, std::move(m_value[1].c_str()), *m_config_substitutions);
        }
        else if (strncmp(m_value[0].c_str(), "slic3r.", 7) == 0) {
            const char *opt_key = m_value[0].c_str() + 7;
            if (print_config_def.options.find(opt_key) != print_config_def.options.end()) {
                ModelConfig *config = nullptr;
                if (m_path.size() == 3) {
                    if (m_path[1] == NODE_TYPE_MATERIAL && m_material)
                        config = &m_material->config;
                    else if (m_path[1] == NODE_TYPE_OBJECT && m_object)
                        config = &m_object->config;
                }
                else if (m_path.size() == 5 && m_path[3] == NODE_TYPE_VOLUME && m_volume)
                    config = &m_volume->config;
                else if (m_path.size() == 5 && m_path[3] == NODE_TYPE_RANGE && m_object && !m_object->layer_config_ranges.empty()) {
                    auto it  = --m_object->layer_config_ranges.end();
                    config = &it->second;
                }
                if (config)
                    config->set_deserialize(opt_key, m_value[1], *m_config_substitutions);
            } else if (m_path.size() == 3 && m_path[1] == NODE_TYPE_OBJECT && m_object && strcmp(opt_key, "layer_height_profile") == 0) {
                // Parse object's layer height profile, a semicolon separated list of floats.
                char *p = m_value[1].data();
                std::vector<coordf_t> data;
                for (;;) {
                    char *end = strchr(p, ';');
                    if (end != nullptr)
	                    *end = 0;
                    data.emplace_back(float(atof(p)));
					if (end == nullptr)
						break;
					p = end + 1;
                }
                m_object->layer_height_profile.set(std::move(data));
            }
            else if (m_path.size() == 3 && m_path[1] == NODE_TYPE_OBJECT && m_object && strcmp(opt_key, "sla_support_points") == 0) {
                // Parse object's layer height profile, a semicolon separated list of floats.
                unsigned char coord_idx = 0;
                Eigen::Matrix<float, 5, 1, Eigen::DontAlign> point(Eigen::Matrix<float, 5, 1, Eigen::DontAlign>::Zero());
                char *p = m_value[1].data();
                for (;;) {
                    char *end = strchr(p, ';');
                    if (end != nullptr)
	                    *end = 0;

                    point(coord_idx) = float(atof(p));
                    if (++coord_idx == 5) {
                        m_object->sla_support_points.push_back(sla::SupportPoint(point));
                        coord_idx = 0;
                    }
					if (end == nullptr)
						break;
					p = end + 1;
                }
                m_object->sla_points_status = sla::PointsStatus::UserModified;
            }
            else if (m_path.size() == 5 && m_path[1] == NODE_TYPE_OBJECT && m_path[3] == NODE_TYPE_RANGE &&
                     m_object && strcmp(opt_key, "layer_height_range") == 0) {
                // Parse object's layer_height_range, a semicolon separated doubles.
                char* p = m_value[1].data();
                char* end = strchr(p, ';');
                *end = 0;

                const t_layer_height_range range = {double(atof(p)), double(atof(end + 1))};
                m_object->layer_config_ranges[range];
            }
            else if (m_path.size() == 5 && m_path[3] == NODE_TYPE_VOLUME && m_volume) {
                if (strcmp(opt_key, "modifier") == 0) {
                    // Is this volume a modifier volume?
                    // "modifier" flag comes first in the XML file, so it may be later overwritten by the "type" flag.
					m_volume->set_type((atoi(m_value[1].c_str()) == 1) ? ModelVolumeType::PARAMETER_MODIFIER : ModelVolumeType::MODEL_PART);
                } else if (strcmp(opt_key, "volume_type") == 0) {
                    m_volume->set_type(ModelVolume::type_from_string(m_value[1]));
                }
                else if (strcmp(opt_key, "matrix") == 0) {
                    m_volume_transform = Slic3r::Geometry::transform3d_from_string(m_value[1]);
                }
                else if (strcmp(opt_key, "source_file") == 0) {
                    m_volume->source.input_file = m_value[1];
                }
                else if (strcmp(opt_key, "source_object_id") == 0) {
                    m_volume->source.object_idx = ::atoi(m_value[1].c_str());
                }
                else if (strcmp(opt_key, "source_volume_id") == 0) {
                    m_volume->source.volume_idx = ::atoi(m_value[1].c_str());
                }
                else if (strcmp(opt_key, "source_offset_x") == 0) {
                    m_volume->source.mesh_offset(0) = ::atof(m_value[1].c_str());
                }
                else if (strcmp(opt_key, "source_offset_y") == 0) {
                    m_volume->source.mesh_offset(1) = ::atof(m_value[1].c_str());
                }
                else if (strcmp(opt_key, "source_offset_z") == 0) {
                    m_volume->source.mesh_offset(2) = ::atof(m_value[1].c_str());
                }
                else if (strcmp(opt_key, "source_in_inches") == 0) {
                    m_volume->source.is_converted_from_inches = m_value[1] == "1";
                }
                else if (strcmp(opt_key, "source_in_meters") == 0) {
                    m_volume->source.is_converted_from_meters = m_value[1] == "1";
                }
            }
        }
        else */if (m_path.size() == 3) {
            if (m_path[1] == NODE_TYPE_MATERIAL) {
                if (m_material)
                    m_material->attributes[m_value[0]] = m_value[1];
            } else if (m_path[1] == NODE_TYPE_OBJECT) {
                if (m_object && m_value[0] == "name")
                    m_object->name = std::move(m_value[1]);
            }
        } else if (m_path.size() == 5 && m_path[3] == NODE_TYPE_VOLUME) {
            if (m_volume && m_value[0] == "name")
                m_volume->name = std::move(m_value[1]);
        }
        /*else if (strncmp(m_value[0].c_str(), SLIC3RPE_AMF_VERSION, strlen(SLIC3RPE_AMF_VERSION)) == 0) {
            m_version = (unsigned int)atoi(m_value[1].c_str());
        }*/

        m_value[0].clear();
        m_value[1].clear();
        break;
    default:
        break;
    }
    m_path.pop_back();
}

void AMFParserContext::endDocument()
{
    for (const auto &object : m_object_instances_map) {
        if (object.second.idx == -1) {
            BOOST_LOG_TRIVIAL(error) << "Undefined object " << object.first.c_str() << " referenced in constellation";
            continue;
        }
        int index = 0;
        for (const Instance &instance : object.second.instances) {
            ModelObject *model_object = m_model.objects[object.second.idx];
            index++;
            if (index > 1) {
                ModelObject *new_model_object = m_model.add_object(*model_object);
                new_model_object->clear_instances();
                model_object = new_model_object;
            }

            if (instance.anything_set()) {
                ModelInstance *mi = model_object->add_instance();
                mi->set_offset(Vec3d(instance.deltax_set ? (double) instance.deltax : 0.0, instance.deltay_set ? (double) instance.deltay : 0.0,
                                     instance.deltaz_set ? (double) instance.deltaz : 0.0));
                mi->set_rotation(Vec3d(instance.rx_set ? (double) instance.rx : 0.0, instance.ry_set ? (double) instance.ry : 0.0, instance.rz_set ? (double) instance.rz : 0.0));
                mi->set_scaling_factor(Vec3d(instance.scalex_set ? (double) instance.scalex : 1.0, instance.scaley_set ? (double) instance.scaley : 1.0,
                                             instance.scalez_set ? (double) instance.scalez : 1.0));
                mi->set_mirror(Vec3d(instance.mirrorx_set ? (double) instance.mirrorx : 1.0, instance.mirrory_set ? (double) instance.mirrory : 1.0,
                                     instance.mirrorz_set ? (double) instance.mirrorz : 1.0));
                mi->printable = instance.printable;
            }
        }
    }
}

// Load an AMF file into a provided model.
//BBS: add inches check logic
bool load_amf_file(const char *path, DynamicPrintConfig *config, ConfigSubstitutionContext *config_substitutions, Model *model, bool *use_inches)
{
    if ((path == nullptr) || (model == nullptr))
        return false;

    XML_Parser parser = XML_ParserCreate(nullptr); // encoding
    if (!parser) {
        BOOST_LOG_TRIVIAL(error) << "Couldn't allocate memory for parser";
        return false;
    }

    FILE *pFile = boost::nowide::fopen(path, "rt");
    if (pFile == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Cannot open file " << path;
        return false;
    }

    AMFParserContext ctx(parser, config, config_substitutions, model);
    XML_SetUserData(parser, (void*)&ctx);
    XML_SetElementHandler(parser, AMFParserContext::startElement, AMFParserContext::endElement);
    XML_SetCharacterDataHandler(parser, AMFParserContext::characters);

    char buff[8192];
    bool result = false;
    for (;;) {
        int len = (int)fread(buff, 1, 8192, pFile);
        if (ferror(pFile)) {
            BOOST_LOG_TRIVIAL(error) << "AMF parser: Read error";
            break;
        }
        int done = feof(pFile);
        if (XML_Parse(parser, buff, len, done) == XML_STATUS_ERROR || ctx.error()) {
            BOOST_LOG_TRIVIAL(error) << "AMF parser: Parse error at line " << int(XML_GetCurrentLineNumber(parser)) << ": " << ctx.error_message();
            break;
        }
        if (done) {
            result = true;
            break;
        }
    }

    XML_ParserFree(parser);
    ::fclose(pFile);

    if (result)
        ctx.endDocument();
    if (use_inches) {
        *use_inches = ctx.m_use_inches;
    }

    for (ModelObject* o : model->objects)
    {
        for (ModelVolume* v : o->volumes)
        {
            if (v->source.input_file.empty() && (v->type() == ModelVolumeType::MODEL_PART))
                v->source.input_file = path;
        }
    }

    return result;
}

//BBS: add inches logic
bool extract_model_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, DynamicPrintConfig* config, ConfigSubstitutionContext* config_substitutions, Model* model, bool* use_inches)
{
    if (stat.m_uncomp_size == 0)
    {
        BOOST_LOG_TRIVIAL(error) << "Found invalid size";
        close_zip_reader(&archive);
        return false;
    }

    XML_Parser parser = XML_ParserCreate(nullptr); // encoding
    if (!parser) {
        BOOST_LOG_TRIVIAL(error) << "Couldn't allocate memory for parser";
        close_zip_reader(&archive);
        return false;
    }

    AMFParserContext ctx(parser, config, config_substitutions, model);
    XML_SetUserData(parser, (void*)&ctx);
    XML_SetElementHandler(parser, AMFParserContext::startElement, AMFParserContext::endElement);
    XML_SetCharacterDataHandler(parser, AMFParserContext::characters);

    struct CallbackData
    {
        XML_Parser& parser;
        AMFParserContext& ctx;
        const mz_zip_archive_file_stat& stat;

        CallbackData(XML_Parser& parser, AMFParserContext& ctx, const mz_zip_archive_file_stat& stat) : parser(parser), ctx(ctx), stat(stat) {}
    };

    CallbackData data(parser, ctx, stat);

    mz_bool res = 0;

    try
    {
        res = mz_zip_reader_extract_file_to_callback(&archive, stat.m_filename, [](void* pOpaque, mz_uint64 file_ofs, const void* pBuf, size_t n)->size_t {
            CallbackData* data = (CallbackData*)pOpaque;
            if (!XML_Parse(data->parser, (const char*)pBuf, (int)n, (file_ofs + n == data->stat.m_uncomp_size) ? 1 : 0) || data->ctx.error())
            {
                char error_buf[1024];
                ::sprintf(error_buf, "Parsing file %s error at line %d: {%s}", data->stat.m_filename, (int) XML_GetCurrentLineNumber(data->parser), data->ctx.error_message());
                throw Slic3r::FileIOError(error_buf);
            }

            return n;
            }, &data, 0);
    }
    catch (std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << "Error reading AMF file: " << e.what();
        close_zip_reader(&archive);
        return false;
    }

    if (res == 0)
    {
        BOOST_LOG_TRIVIAL(error) << "Error while extracting model data from zip archive";
        close_zip_reader(&archive);
        return false;
    }

    ctx.endDocument();
    if (use_inches) {
        *use_inches = ctx.m_use_inches;
    }
    //if (check_version && (ctx.m_version > VERSION_AMF_COMPATIBLE))
    //{
        // std::string msg = _(L("The selected amf file has been saved with a newer version of " + std::string(SLIC3R_APP_NAME) + " and is not compatible."));
        // throw Slic3r::FileIOError(msg.c_str());
        //const std::string msg = (boost::format(_(L("The selected amf file has been saved with a newer version of %1% and is not compatible."))) % std::string(SLIC3R_APP_NAME)).str();
        //throw Slic3r::FileIOError(msg);
    //}

    return true;
}

// Load an AMF archive into a provided model.
bool load_amf_archive(const char* path, DynamicPrintConfig* config, ConfigSubstitutionContext* config_substitutions, Model* model, bool* use_inches)
{
    if ((path == nullptr) || (model == nullptr))
        return false;

    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);

    if (!open_zip_reader(&archive, path))
    {
        BOOST_LOG_TRIVIAL(error) << "Unable to init zip reader";
        return false;
    }

    mz_uint num_entries = mz_zip_reader_get_num_files(&archive);

    mz_zip_archive_file_stat stat;
    // we first loop the entries to read from the archive the .amf file only, in order to extract the version from it
    for (mz_uint i = 0; i < num_entries; ++i)
    {
        if (mz_zip_reader_file_stat(&archive, i, &stat))
        {
            if (boost::iends_with(stat.m_filename, ".amf"))
            {
                try
                {
                    if (!extract_model_from_archive(archive, stat, config, config_substitutions, model, use_inches))
                    {
                        close_zip_reader(&archive);
                        BOOST_LOG_TRIVIAL(error) << "Archive does not contain a valid model";
                        return false;
                    }
                }
                catch (const std::exception& e)
                {
                    // ensure the zip archive is closed and rethrow the exception
                    close_zip_reader(&archive);
                    throw Slic3r::FileIOError(e.what());
                }

                break;
            }
        }
    }

#if 0 // forward compatibility
    // we then loop again the entries to read other files stored in the archive
    for (mz_uint i = 0; i < num_entries; ++i)
    {
        if (mz_zip_reader_file_stat(&archive, i, &stat))
        {
            // add code to extract the file
        }
    }
#endif // forward compatibility

    close_zip_reader(&archive);

    for (ModelObject *o : model->objects)
        for (ModelVolume *v : o->volumes)
            if (v->source.input_file.empty() && (v->type() == ModelVolumeType::MODEL_PART))
                v->source.input_file = path;

    return true;
}

// Load an AMF file into a provided model.
// If config is not a null pointer, updates it if the amf file/archive contains config data
//BBS: refine the amf logic
bool load_amf(const char *path, DynamicPrintConfig *config, ConfigSubstitutionContext *config_substitutions, Model *model, bool* use_inches)
{
    CNumericLocalesSetter locales_setter; // use "C" locales and point as a decimal separator

    if (boost::iends_with(path, ".amf"))
    {
        boost::nowide::ifstream file(path, boost::nowide::ifstream::binary);
        if (!file.good())
            return false;

        std::string zip_mask(2, '\0');
        file.read(zip_mask.data(), 2);
        file.close();

        return (zip_mask == "PK") ? load_amf_archive(path, config, config_substitutions, model, use_inches) : load_amf_file(path, config, config_substitutions, model, use_inches);
    }
    else
        return false;
}

/*bool store_amf(const char* path, Model* model, const DynamicPrintConfig* config, bool fullpath_sources)
{
    if ((path == nullptr) || (model == nullptr))
        return false;

    // forces ".zip.amf" extension
    std::string export_path = path;
    if (!boost::iends_with(export_path, ".zip.amf"))
        export_path = boost::filesystem::path(export_path).replace_extension(".zip.amf").string();

    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);

    if (!open_zip_writer(&archive, export_path)) return false;

    std::stringstream stream;
    // https://en.cppreference.com/w/cpp/types/numeric_limits/max_digits10
    // Conversion of a floating-point value to text and back is exact as long as at least max_digits10 were used (9 for float, 17 for double).
    // It is guaranteed to produce the same floating-point value, even though the intermediate text representation is not exact.
    // The default value of std::stream precision is 6 digits only!
    stream << std::setprecision(std::numeric_limits<float>::max_digits10);
    stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    stream << "<amf unit=\"millimeter\">\n";
    stream << "<metadata type=\"cad\">Slic3r " << SLIC3R_VERSION << "</metadata>\n";
    stream << "<metadata type=\"" << SLIC3RPE_AMF_VERSION << "\">" << VERSION_AMF << "</metadata>\n";

    if (config != nullptr)
    {
        std::string str_config = "\n";
        for (const std::string &key : config->keys())
            if (key != "compatible_printers")
                str_config += "; " + key + " = " + config->opt_serialize(key) + "\n";
        stream << "<metadata type=\"" << SLIC3R_CONFIG_TYPE << "\">" << xml_escape(str_config) << "</metadata>\n";
    }

    for (const auto &material : model->materials) {
        if (material.first.empty())
            continue;
        // note that material-id must never be 0 since it's reserved by the AMF spec
        stream << "  <material id=\"" << material.first << "\">\n";
        for (const auto &attr : material.second->attributes)
            stream << "    <metadata type=\"" << attr.first << "\">" << attr.second << "</metadata>\n";
        for (const std::string &key : material.second->config.keys())
            stream << "    <metadata type=\"slic3r." << key << "\">" << material.second->config.opt_serialize(key) << "</metadata>\n";
        stream << "  </material>\n";
    }
    std::string instances;
    for (size_t object_id = 0; object_id < model->objects.size(); ++ object_id) {
        ModelObject *object = model->objects[object_id];
        stream << "  <object id=\"" << object_id << "\">\n";
        for (const std::string &key : object->config.keys())
            stream << "    <metadata type=\"slic3r." << key << "\">" << object->config.opt_serialize(key) << "</metadata>\n";
        if (!object->name.empty())
            stream << "    <metadata type=\"name\">" << xml_escape(object->name) << "</metadata>\n";
        const std::vector<double> &layer_height_profile = object->layer_height_profile.get();
        if (layer_height_profile.size() >= 4 && (layer_height_profile.size() % 2) == 0) {
            // Store the layer height profile as a single semicolon separated list.
            stream << "    <metadata type=\"slic3r.layer_height_profile\">";
            stream << layer_height_profile.front();
            for (size_t i = 1; i < layer_height_profile.size(); ++i)
                stream << ";" << layer_height_profile[i];
            stream << "\n    </metadata>\n";
        }

        // Export layer height ranges including the layer range specific config overrides.
        const t_layer_config_ranges& config_ranges = object->layer_config_ranges;
        if (!config_ranges.empty())
        {
            // Store the layer config range as a single semicolon separated list.
            stream << "    <layer_config_ranges>\n";
            size_t layer_counter = 0;
            for (const auto &range : config_ranges) {
                stream << "      <range id=\"" << layer_counter << "\">\n";

                stream << "        <metadata type=\"slic3r.layer_height_range\">";
                stream << range.first.first << ";" << range.first.second << "</metadata>\n";

                for (const std::string& key : range.second.keys())
                    stream << "        <metadata type=\"slic3r." << key << "\">" << range.second.opt_serialize(key) << "</metadata>\n";

                stream << "      </range>\n";
                layer_counter++;
            }

            stream << "    </layer_config_ranges>\n";
        }


        const std::vector<sla::SupportPoint>& sla_support_points = object->sla_support_points;
        if (!sla_support_points.empty()) {
            // Store the SLA supports as a single semicolon separated list.
            stream << "    <metadata type=\"slic3r.sla_support_points\">";
            for (size_t i = 0; i < sla_support_points.size(); ++i) {
                if (i != 0)
                    stream << ";";
                stream << sla_support_points[i].pos(0) << ";" << sla_support_points[i].pos(1) << ";" << sla_support_points[i].pos(2) << ";" << sla_support_points[i].head_front_radius << ";" << sla_support_points[i].is_new_island;
            }
            stream << "\n    </metadata>\n";
        }

        stream << "    <mesh>\n";
        stream << "      <vertices>\n";
        std::vector<int> vertices_offsets;
        int              num_vertices = 0;
        for (ModelVolume *volume : object->volumes) {
            vertices_offsets.push_back(num_vertices);
            const indexed_triangle_set &its = volume->mesh().its;
            const Transform3d& matrix = volume->get_matrix();
            for (size_t i = 0; i < its.vertices.size(); ++i) {
                stream << "         <vertex>\n";
                stream << "           <coordinates>\n";
                Vec3f v = (matrix * its.vertices[i].cast<double>()).cast<float>();
                stream << "             <x>" << v(0) << "</x>\n";
                stream << "             <y>" << v(1) << "</y>\n";
                stream << "             <z>" << v(2) << "</z>\n";
                stream << "           </coordinates>\n";
                stream << "         </vertex>\n";
            }
            num_vertices += (int)its.vertices.size();
        }
        stream << "      </vertices>\n";
        for (size_t i_volume = 0; i_volume < object->volumes.size(); ++i_volume) {
            ModelVolume *volume = object->volumes[i_volume];
            int vertices_offset = vertices_offsets[i_volume];
            if (volume->material_id().empty())
                stream << "      <volume>\n";
            else
                stream << "      <volume materialid=\"" << volume->material_id() << "\">\n";
            for (const std::string &key : volume->config.keys())
                stream << "        <metadata type=\"slic3r." << key << "\">" << volume->config.opt_serialize(key) << "</metadata>\n";
            if (!volume->name.empty())
                stream << "        <metadata type=\"name\">" << xml_escape(volume->name) << "</metadata>\n";
            if (volume->is_modifier())
                stream << "        <metadata type=\"slic3r.modifier\">1</metadata>\n";
            stream << "        <metadata type=\"slic3r.volume_type\">" << ModelVolume::type_to_string(volume->type()) << "</metadata>\n";
            stream << "        <metadata type=\"slic3r.matrix\">";
            const Transform3d& matrix = volume->get_matrix() * volume->source.transform.get_matrix();
            stream << std::setprecision(std::numeric_limits<double>::max_digits10);
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    stream << matrix(r, c);
                    if ((r != 3) || (c != 3))
                        stream << " ";
                }
            }
            stream << "</metadata>\n";
            if (!volume->source.input_file.empty())
            {
                std::string input_file = xml_escape(fullpath_sources ? volume->source.input_file : boost::filesystem::path(volume->source.input_file).filename().string());
                stream << "        <metadata type=\"slic3r.source_file\">" << input_file << "</metadata>\n";
                stream << "        <metadata type=\"slic3r.source_object_id\">" << volume->source.object_idx << "</metadata>\n";
                stream << "        <metadata type=\"slic3r.source_volume_id\">" << volume->source.volume_idx << "</metadata>\n";
                stream << "        <metadata type=\"slic3r.source_offset_x\">" << volume->source.mesh_offset(0) << "</metadata>\n";
                stream << "        <metadata type=\"slic3r.source_offset_y\">" << volume->source.mesh_offset(1) << "</metadata>\n";
                stream << "        <metadata type=\"slic3r.source_offset_z\">" << volume->source.mesh_offset(2) << "</metadata>\n";
            }
            assert(! volume->source.is_converted_from_inches || ! volume->source.is_converted_from_meters);
            if (volume->source.is_converted_from_inches)
                stream << "        <metadata type=\"slic3r.source_in_inches\">1</metadata>\n";
            else if (volume->source.is_converted_from_meters)
                stream << "        <metadata type=\"slic3r.source_in_meters\">1</metadata>\n";
			stream << std::setprecision(std::numeric_limits<float>::max_digits10);
            const indexed_triangle_set &its = volume->mesh().its;
            for (size_t i = 0; i < its.indices.size(); ++i) {
                stream << "        <triangle>\n";
                for (int j = 0; j < 3; ++j)
                stream << "          <v" << j + 1 << ">" << its.indices[i][j] + vertices_offset << "</v" << j + 1 << ">\n";
                stream << "        </triangle>\n";
            }
            stream << "      </volume>\n";
        }
        stream << "    </mesh>\n";
        stream << "  </object>\n";
        if (!object->instances.empty()) {
            for (ModelInstance *instance : object->instances) {
                std::stringstream buf;
                buf << "    <instance objectid=\"" << object_id << "\">\n"
                    << "      <deltax>"  << instance->get_offset(X)         << "</deltax>\n"
                    << "      <deltay>"  << instance->get_offset(Y)         << "</deltay>\n"
                    << "      <deltaz>"  << instance->get_offset(Z)         << "</deltaz>\n"
                    << "      <rx>"      << instance->get_rotation(X)       << "</rx>\n"
                    << "      <ry>"      << instance->get_rotation(Y)       << "</ry>\n"
                    << "      <rz>"      << instance->get_rotation(Z)       << "</rz>\n"
                    << "      <scalex>"  << instance->get_scaling_factor(X) << "</scalex>\n"
                    << "      <scaley>"  << instance->get_scaling_factor(Y) << "</scaley>\n"
                    << "      <scalez>"  << instance->get_scaling_factor(Z) << "</scalez>\n"
                    << "      <mirrorx>" << instance->get_mirror(X)         << "</mirrorx>\n"
                    << "      <mirrory>" << instance->get_mirror(Y)         << "</mirrory>\n"
                    << "      <mirrorz>" << instance->get_mirror(Z)         << "</mirrorz>\n"
                    << "      <printable>" << instance->printable << "</printable>\n"
                    << "    </instance>\n";

                //FIXME missing instance->scaling_factor
                instances.append(buf.str());
            }
        }
    }
    if (! instances.empty()) {
        stream << "  <constellation id=\"1\">\n";
        stream << instances;
        stream << "  </constellation>\n";
    }

    if (!model->custom_gcode_per_print_z.gcodes.empty())
    {
        std::string out = "";
        pt::ptree tree;

        pt::ptree& main_tree = tree.add("custom_gcodes_per_height", "");

        for (const CustomGCode::Item& code : model->custom_gcode_per_print_z.gcodes)
        {
            pt::ptree& code_tree = main_tree.add("code", "");
            // store custom_gcode_per_print_z gcodes information
            code_tree.put("<xmlattr>.print_z"   , code.print_z  );
            code_tree.put("<xmlattr>.type"      , static_cast<int>(code.type));
            code_tree.put("<xmlattr>.extruder"  , code.extruder );
            code_tree.put("<xmlattr>.color"     , code.color    );
            code_tree.put("<xmlattr>.extra"     , code.extra    );

            //BBS
            std::string gcode = //code.type == CustomGCode::ColorChange ? config->opt_string("color_change_gcode")    :
                                code.type == CustomGCode::PausePrint  ? config->opt_string("machine_pause_gcode")     :
                                code.type == CustomGCode::Template    ? config->opt_string("template_custom_gcode")   :
                                code.type == CustomGCode::ToolChange  ? "tool_change"   : code.extra;
            code_tree.put("<xmlattr>.gcode"     , gcode   );
        }

        pt::ptree& mode_tree = main_tree.add("mode", "");
        // store mode of a custom_gcode_per_print_z
        mode_tree.put("<xmlattr>.value",
                      model->custom_gcode_per_print_z.mode == CustomGCode::Mode::SingleExtruder ? CustomGCode::SingleExtruderMode :
                      model->custom_gcode_per_print_z.mode == CustomGCode::Mode::MultiAsSingle  ?
                      CustomGCode::MultiAsSingleMode  : CustomGCode::MultiExtruderMode);

        if (!tree.empty())
        {
            std::ostringstream oss;
            pt::write_xml(oss, tree);
            out = oss.str();

            size_t del_header_pos = out.find("<custom_gcodes_per_height");
            if (del_header_pos != std::string::npos)
                out.erase(out.begin(), out.begin() + del_header_pos);

            // Post processing("beautification") of the output string
            boost::replace_all(out, "><code", ">\n  <code");
            boost::replace_all(out, "><mode", ">\n  <mode");
            boost::replace_all(out, "><", ">\n<");

            stream << out << "\n";
        }
    }

    stream << "</amf>\n";

    std::string internal_amf_filename = boost::ireplace_last_copy(boost::filesystem::path(export_path).filename().string(), ".zip.amf", ".amf");
    std::string out = stream.str();

    if (!mz_zip_writer_add_mem(&archive, internal_amf_filename.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION))
    {
        close_zip_writer(&archive);
        boost::filesystem::remove(export_path);
        return false;
    }

    if (!mz_zip_writer_finalize_archive(&archive))
    {
        close_zip_writer(&archive);
        boost::filesystem::remove(export_path);
        return false;
    }

    close_zip_writer(&archive);

    return true;
}*/

}; // namespace Slic3r
