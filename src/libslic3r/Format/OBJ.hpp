#ifndef slic3r_Format_OBJ_hpp_
#define slic3r_Format_OBJ_hpp_
#include "libslic3r/Color.hpp"
#include <unordered_map>
namespace Slic3r {

class TriangleMesh;
class Model;
class ModelObject;
// Load an OBJ file into a provided model.
struct ObjInfo {
    std::vector<RGBA> vertex_colors;
    std::vector<RGBA> face_colors;
    bool              is_single_mtl{false};
    std::string       lost_material_name{""};
    std::vector<std::array<Vec2f,3>> uvs;
    std::string        obj_dircetory;
    std::map<std::string,bool>  pngs;
    std::unordered_map<int, std::string> uv_map_pngs;
    bool              has_uv_png{false};

};
struct ObjDialogInOut
{ // input:colors array
    std::vector<RGBA> input_colors;
    bool              is_single_color{false};
    // colors array output:
    std::vector<unsigned char> filament_ids;
    unsigned char              first_extruder_id;
    bool                       deal_vertex_color;
    Model *                    model{nullptr};
    std::string lost_material_name{""};
};
typedef std::function<void(ObjDialogInOut &in_out)> ObjImportColorFn;
extern bool load_obj(const char *path, TriangleMesh *mesh, ObjInfo &vertex_colors, std::string &message);
extern bool load_obj(const char *path, Model *model, ObjInfo &vertex_colors, std::string &message, const char *object_name = nullptr);

extern bool store_obj(const char *path, TriangleMesh *mesh);
extern bool store_obj(const char *path, ModelObject *model);
extern bool store_obj(const char *path, Model *model);

}; // namespace Slic3r

#endif /* slic3r_Format_OBJ_hpp_ */
