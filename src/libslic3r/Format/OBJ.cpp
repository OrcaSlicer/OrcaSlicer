#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"
#include "../TexturePainting.hpp"
#include "ResourcePathUtils.hpp"

#include "OBJ.hpp"
#include "objparser.hpp"

#include <string>

#include <boost/log/trivial.hpp>

#ifdef _WIN32
#define DIR_SEPARATOR '\\'
#else
#define DIR_SEPARATOR '/'
#endif

//Translation
#include "I18N.hpp"
#define _L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

bool load_obj(const char *path, TriangleMesh *meshptr, ObjInfo& obj_info, std::string &message, ObjParser::MtlData *out_mtl)
{
    if (meshptr == nullptr)
        return false;
    // Parse the OBJ file.
    ObjParser::ObjData data;
    ObjParser::MtlData mtl_data;
    if (! ObjParser::objparse(path, data)) {
        BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path;
        message = _L("load_obj: failed to parse");
        return false;
    }
    bool exist_mtl = false;
    if (data.mtllibs.size() > 0) { // read mtl
        for (auto mtl_name : data.mtllibs) {
            if (mtl_name.size() == 0){
                continue;
            }
            exist_mtl = true;
            bool                    mtl_name_is_path = false;
            boost::filesystem::path mtl_abs_path(mtl_name);
            if (boost::filesystem::exists(mtl_abs_path)) {
                mtl_name_is_path = true;
            }
            boost::filesystem::path mtl_path;
            if (!mtl_name_is_path) {
                boost::filesystem::path full_path(path);
                std::string             dir = full_path.parent_path().string();
                auto                    mtl_file = dir + "/" + mtl_name;
                boost::filesystem::path temp_mtl_path(mtl_file);
                mtl_path = temp_mtl_path;
            }
            auto    _mtl_path = mtl_name_is_path ? mtl_abs_path.string().c_str() : mtl_path.string().c_str();
            if (boost::filesystem::exists(mtl_name_is_path ? mtl_abs_path : mtl_path)) {
                if (!ObjParser::mtlparse(_mtl_path, mtl_data)) {
                    BOOST_LOG_TRIVIAL(error) << "load_obj:load_mtl: failed to parse " << _mtl_path;
                    message = _L("load mtl in obj: failed to parse");
                    return false;
                }
            }
            else {
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to load mtl_path:" << _mtl_path;
            }
        }
    }
    // Count the faces and verify, that all faces are triangular.
    size_t num_faces = 0;
    size_t num_quads = 0;
    for (size_t i = 0; i < data.vertices.size(); ++ i) {
        // Find the end of face.
        size_t j = i;
        for (; j < data.vertices.size() && data.vertices[j].coordIdx != -1; ++ j) ;
        if (size_t num_face_vertices = j - i; num_face_vertices > 0) {
            if (num_face_vertices > 4) {
                // Non-triangular and non-quad faces are not supported as of now.
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains polygons with more than 4 vertices.";
                message = _L("The file contains polygons with more than 4 vertices.");
                return false;
            } else if (num_face_vertices < 3) {
                // Non-triangular and non-quad faces are not supported as of now.
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains polygons with less than 2 vertices.";
                message = _L("The file contains polygons with less than 2 vertices.");
                return false;
            }
            if (num_face_vertices == 4)
                ++ num_quads;
            ++ num_faces;
            i = j;
        }
    }
    // Convert ObjData into indexed triangle set.
    indexed_triangle_set its;
    size_t               num_vertices = data.coordinates.size() / OBJ_VERTEX_LENGTH;
    its.vertices.reserve(num_vertices);
    its.indices.reserve(num_faces + num_quads);
    if (exist_mtl) {
        obj_info.is_single_mtl = data.usemtls.size() == 1 && mtl_data.new_mtl_unmap.size() == 1;
        obj_info.usemtls = data.usemtls;
        obj_info.face_colors.reserve(num_faces + num_quads);
    }
    bool has_color = data.has_vertex_color;
    for (size_t i = 0; i < num_vertices; ++ i) {
        size_t j = i * OBJ_VERTEX_LENGTH;
        its.vertices.emplace_back(data.coordinates[j], data.coordinates[j + 1], data.coordinates[j + 2]);
        if (data.has_vertex_color) {
            RGBA color{std::clamp(data.coordinates[j + 3], 0.f, 1.f), std::clamp(data.coordinates[j + 4], 0.f, 1.f), std::clamp(data.coordinates[j + 5], 0.f, 1.f),
                       std::clamp(data.coordinates[j + 6], 0.f, 1.f)};
            obj_info.vertex_colors.emplace_back(color);
        }
    }
    int indices[ONE_FACE_SIZE];
    int uvs[ONE_FACE_SIZE];
    for (size_t i = 0; i < data.vertices.size();)
        if (data.vertices[i].coordIdx == -1)
            ++ i;
        else {
            int cnt = 0;
            while (i < data.vertices.size())
                if (const ObjParser::ObjVertex &vertex = data.vertices[i ++]; vertex.coordIdx == -1) {
                    break;
                } else {
                    assert(cnt < OBJ_VERTEX_LENGTH);
                    if (vertex.coordIdx < 0 || vertex.coordIdx >= int(its.vertices.size())) {
                        BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains invalid vertex index.";
                        message = _L("The file contains invalid vertex index.");
                        return false;
                    }
                    indices[cnt] = vertex.coordIdx;
                    uvs[cnt]     = vertex.textureCoordIdx;
                    cnt++;
                }
            if (cnt) {
                assert(cnt == 3 || cnt == 4);
                // Insert one or two faces (triangulate a quad).
                its.indices.emplace_back(indices[0], indices[1], indices[2]);
                int  face_index =its.indices.size() - 1;
                RGBA face_color;
                auto set_face_color = [&uvs, &data, &mtl_data, &obj_info, &face_color](int face_index, const std::string mtl_name) {
                    if (mtl_data.new_mtl_unmap.find(mtl_name) != mtl_data.new_mtl_unmap.end()) {
                        bool is_merge_ka_kd = true;
                        for (size_t n = 0; n < 3; n++) {
                            if (float(mtl_data.new_mtl_unmap[mtl_name]->Ka[n] + mtl_data.new_mtl_unmap[mtl_name]->Kd[n]) > 1.0) {
                                is_merge_ka_kd=false;
                                break;
                            }
                        }
                        for (size_t n = 0; n < 3; n++) {
                            if (is_merge_ka_kd) {
                                face_color[n] = std::clamp(float(mtl_data.new_mtl_unmap[mtl_name]->Ka[n] + mtl_data.new_mtl_unmap[mtl_name]->Kd[n]), 0.f, 1.f);
                            }
                            else {
                                face_color[n] = std::clamp(float(mtl_data.new_mtl_unmap[mtl_name]->Kd[n]), 0.f, 1.f);
                            }
                        }
                        face_color[3] = mtl_data.new_mtl_unmap[mtl_name]->Tr; // alpha
                        if (mtl_data.new_mtl_unmap[mtl_name]->map_Kd.size() > 0) {
                            auto png_name       = mtl_data.new_mtl_unmap[mtl_name]->map_Kd;
                            obj_info.has_uv_png = true;
                            if (obj_info.pngs.find(png_name) == obj_info.pngs.end()) { obj_info.pngs[png_name] = false; }
                            obj_info.uv_map_pngs[face_index] = png_name;
                        }
                        if (data.textureCoordinates.size() > 0) {
                            Vec2f                uv0(data.textureCoordinates[uvs[0] * 2], data.textureCoordinates[uvs[0] * 2 + 1]);
                            Vec2f                uv1(data.textureCoordinates[uvs[1] * 2], data.textureCoordinates[uvs[1] * 2 + 1]);
                            Vec2f                uv2(data.textureCoordinates[uvs[2] * 2], data.textureCoordinates[uvs[2] * 2 + 1]);
                            std::array<Vec2f, 3> uv_array{uv0, uv1, uv2};
                            obj_info.uvs.emplace_back(uv_array);
                        }
                        obj_info.face_colors.emplace_back(face_color);
                    }
                    else {
                        if (obj_info.lost_material_name.empty()) {
                            obj_info.lost_material_name = mtl_name;
                        }
                    }
                };
                auto set_face_color_by_mtl = [&data, &set_face_color](int face_index) {
                    if (data.usemtls.size() == 1) {
                        set_face_color(face_index, data.usemtls[0].name);
                    } else {
                        for (size_t k = 0; k < data.usemtls.size(); k++) {
                            auto mtl = data.usemtls[k];
                            if (face_index >= mtl.face_start && face_index <= mtl.face_end) {
                                set_face_color(face_index, data.usemtls[k].name);
                                break;
                            }
                        }
                    }
                };
                if (exist_mtl) {
                    set_face_color_by_mtl(face_index);
                }
                if (cnt == 4) {
                    its.indices.emplace_back(indices[0], indices[2], indices[3]);
                    int face_index = its.indices.size() - 1;
                    if (exist_mtl) {
                        set_face_color_by_mtl(face_index);
                    }
                }
            }
        }

    *meshptr = TriangleMesh(std::move(its));
    if (meshptr->empty()) {
        BOOST_LOG_TRIVIAL(error) << "load_obj: This OBJ file couldn't be read because it's empty. " << path;
        message = _L("This OBJ file couldn't be read because it's empty.");
        return false;
    }
    if (meshptr->volume() < 0)
        meshptr->flip_triangles();
    // Hand the parsed material table back so callers can build a TexturedMesh from it.
    if (out_mtl)
        *out_mtl = mtl_data;
    return true;
}

bool load_obj(const char *path, Model *model, ObjInfo& obj_info, std::string &message, const char *object_name_in, ObjParser::MtlData *out_mtl)
{
    TriangleMesh mesh;

    bool ret = load_obj(path, &mesh, obj_info, message, out_mtl);

    if (ret) {
        std::string  object_name;
        if (object_name_in == nullptr) {
            const char *last_slash = strrchr(path, DIR_SEPARATOR);
            object_name.assign((last_slash == nullptr) ? path : last_slash + 1);
        } else
           object_name.assign(object_name_in);
        model->add_object(object_name.c_str(), path, std::move(mesh));
    }

    return ret;
}

bool obj_to_textured_mesh(
    const ObjInfo& obj_info,
    const indexed_triangle_set& its,
    const ObjParser::MtlData& mtl_data,
    const std::string& obj_directory,
    TexturedMesh& out)
{
    if (its.vertices.empty() || its.indices.empty() || !obj_info.has_uv_png)
        return false;

    const size_t nv = its.vertices.size();
    const size_t nf = its.indices.size();

    // 1. Copy vertices
    out.vertices.resize(nv);
    for (size_t i = 0; i < nv; ++i)
        out.vertices[i] = {its.vertices[i].x(), its.vertices[i].y(), its.vertices[i].z()};

    // 2. Copy face indices
    out.indices.resize(nf);
    for (size_t i = 0; i < nf; ++i)
        out.indices[i] = {its.indices[i][0], its.indices[i][1], its.indices[i][2]};

    // 3. Build per-face UV (uv_coords + uv_indices)
    // OBJ UV convention: V=0 at bottom (OpenGL); texture sampling expects V=0 at top (like glTF/OpenCV).
    // Flip V here so downstream code works uniformly.
    if (!obj_info.uvs.empty()) {
        const size_t uv_face_count = obj_info.uvs.size();
        out.uv_coords.resize(uv_face_count * 3);
        out.uv_indices.resize(nf);
        for (size_t fi = 0; fi < nf; ++fi) {
            if (fi < uv_face_count) {
                int base = static_cast<int>(fi * 3);
                out.uv_coords[base + 0] = {obj_info.uvs[fi][0].x(), 1.f - obj_info.uvs[fi][0].y()};
                out.uv_coords[base + 1] = {obj_info.uvs[fi][1].x(), 1.f - obj_info.uvs[fi][1].y()};
                out.uv_coords[base + 2] = {obj_info.uvs[fi][2].x(), 1.f - obj_info.uvs[fi][2].y()};
                out.uv_indices[fi] = {base, base + 1, base + 2};
            } else {
                out.uv_indices[fi] = {0, 0, 0};
            }
        }
    }

    // 4. Build material list and load textures from disk
    // Map: material name -> material index
    std::map<std::string, int> mtl_name_to_idx;
    for (size_t i = 0; i < mtl_data.mtl_orders.size(); ++i)
        mtl_name_to_idx[mtl_data.mtl_orders[i]] = static_cast<int>(i);

    const int num_materials = static_cast<int>(mtl_data.mtl_orders.size());
    out.material_colors.resize(num_materials, {1.f, 1.f, 1.f, 1.f});
    out.material_texture_map.resize(num_materials, -1);

    // Map: texture filename -> index in out.textures
    std::map<std::string, int> png_to_tex_idx;

    for (int mi = 0; mi < num_materials; ++mi) {
        const std::string& name = mtl_data.mtl_orders[mi];
        auto it = mtl_data.new_mtl_unmap.find(name);
        if (it == mtl_data.new_mtl_unmap.end())
            continue;
        const auto& mtl = *(it->second);

        // Material color from Kd
        out.material_colors[mi] = {mtl.Kd[0], mtl.Kd[1], mtl.Kd[2], mtl.Tr};

        // Texture from map_Kd
        if (mtl.map_Kd.empty())
            continue;

        auto tex_it = png_to_tex_idx.find(mtl.map_Kd);
        if (tex_it != png_to_tex_idx.end()) {
            out.material_texture_map[mi] = tex_it->second;
            continue;
        }

        // Resolve texture file path.
        const boost::filesystem::path requested_tex_path(mtl.map_Kd);
        const boost::filesystem::path tex_path = requested_tex_path.is_absolute() ?
            resource_path::resolve_existing_path_case_insensitive(requested_tex_path, "obj_to_textured_mesh: map_Kd") :
            resource_path::resolve_existing_relative_path_case_insensitive(
                boost::filesystem::path(obj_directory), requested_tex_path, "obj_to_textured_mesh: map_Kd");

        if (tex_path.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "obj_to_textured_mesh: texture not found: " << requested_tex_path;
            continue;
        }

        // Read raw file bytes
        boost::nowide::ifstream file(tex_path.string(), std::ios::binary | std::ios::ate);
        if (!file.is_open())
            continue;
        auto file_size = file.tellg();
        if (file_size <= 0)
            continue;
        file.seekg(0, std::ios::beg);

        TextureImage ti;
        ti.data.resize(static_cast<size_t>(file_size));
        file.read(reinterpret_cast<char*>(ti.data.data()), file_size);
        ti.width = -1;
        ti.height = -1;
        ti.channels = 0;

        int new_idx = static_cast<int>(out.textures.size());
        out.textures.push_back(std::move(ti));
        png_to_tex_idx[mtl.map_Kd] = new_idx;
        out.material_texture_map[mi] = new_idx;
    }

    // 5. Build per-face material_ids from usemtls ranges
    out.material_ids.resize(nf, -1);
    if (!obj_info.usemtls.empty()) {
        for (size_t fi = 0; fi < nf; ++fi) {
            int face_idx = static_cast<int>(fi);
            for (size_t k = 0; k < obj_info.usemtls.size(); ++k) {
                const auto& um = obj_info.usemtls[k];
                if (face_idx >= um.face_start && face_idx <= um.face_end) {
                    auto name_it = mtl_name_to_idx.find(um.name);
                    if (name_it != mtl_name_to_idx.end())
                        out.material_ids[fi] = name_it->second;
                    break;
                }
            }
        }
    }

    if (out.textures.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "obj_to_textured_mesh: no textures loaded";
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "obj_to_textured_mesh: " << nf << " faces, "
                            << out.textures.size() << " textures, "
                            << num_materials << " materials";
    return true;
}

bool store_obj(const char *path, TriangleMesh *mesh)
{
    //FIXME returning false even if write failed.
    mesh->WriteOBJFile(path);
    return true;
}

bool store_obj(const char *path, ModelObject *model_object)
{
    TriangleMesh mesh = model_object->mesh();
    return store_obj(path, &mesh);
}

bool store_obj(const char *path, Model *model)
{
    TriangleMesh mesh = model->mesh();
    return store_obj(path, &mesh);
}

}; // namespace Slic3r
