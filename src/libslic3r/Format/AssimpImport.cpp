#include "AssimpImport.hpp"

#include "../TexturePainting.hpp"
#include "ResourcePathUtils.hpp"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace Slic3r {
namespace {

void clear_textured_mesh(TexturedMesh& out)
{
    out.vertices.clear();
    out.indices.clear();
    out.uvs.clear();
    out.uv_coords.clear();
    out.uv_indices.clear();
    out.textures.clear();
    out.material_ids.clear();
    out.material_texture_map.clear();
    out.material_colors.clear();
    out.precomputed_vertex_colors.clear();
    out.precomputed_face_colors.clear();
}

void set_error_message(std::string* error_message, const std::string& message)
{
    if (error_message)
        *error_message = message;
}

bool is_fbx_path(const std::string& path)
{
    return boost::algorithm::iends_with(path, ".fbx");
}

bool should_flip_uvs(const std::string& path)
{
    return boost::algorithm::iends_with(path, ".fbx") ||
           boost::algorithm::iends_with(path, ".glb");
}

unsigned int assimp_import_flags(const std::string& path)
{
    unsigned int flags = aiProcess_Triangulate
                       | aiProcess_GenNormals
                       | aiProcess_PreTransformVertices
                       | aiProcess_SortByPType;
    if (should_flip_uvs(path))
        flags |= aiProcess_FlipUVs;
    return flags;
}

void configure_importer(Assimp::Importer& importer, const std::string& path, unsigned int flags)
{
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
        aiPrimitiveType_POINT | aiPrimitiveType_LINE);

    if (flags & aiProcess_PreTransformVertices)
        importer.SetPropertyBool(AI_CONFIG_PP_PTV_KEEP_HIERARCHY, true);

    if (is_fbx_path(path)) {
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ALL_GEOMETRY_LAYERS, true);
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_MATERIALS, true);
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_TEXTURES, true);
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ANIMATIONS, false);
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_LIGHTS, false);
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_CAMERAS, false);
    }
}

bool read_external_texture_file(const boost::filesystem::path& path, TextureImage& out)
{
    boost::nowide::ifstream file(path.string(), std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    const std::streamoff size = file.tellg();
    if (size <= 0)
        return false;
    if (static_cast<uintmax_t>(size) > static_cast<uintmax_t>(std::numeric_limits<size_t>::max()))
        return false;

    file.seekg(0);
    out.width = -1;
    out.height = -1;
    out.channels = 0;
    out.data.resize(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(out.data.data()), size);
    if (!file && !file.eof()) {
        out.data.clear();
        return false;
    }
    return true;
}

bool read_embedded_texture(const aiTexture& texture, TextureImage& out)
{
    out.data.clear();
    if (texture.mHeight == 0) {
        if (texture.mWidth == 0)
            return false;
        out.width = -1;
        out.height = -1;
        out.channels = 0;
        out.data.assign(
            reinterpret_cast<const unsigned char*>(texture.pcData),
            reinterpret_cast<const unsigned char*>(texture.pcData) + texture.mWidth);
        return !out.data.empty();
    }

    if (texture.mWidth == 0 || texture.mHeight == 0)
        return false;
    if (texture.mWidth > static_cast<unsigned int>(std::numeric_limits<int>::max()) ||
        texture.mHeight > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
        return false;
    }
    const size_t width = static_cast<size_t>(texture.mWidth);
    const size_t height = static_cast<size_t>(texture.mHeight);
    if (width > std::numeric_limits<size_t>::max() / height ||
        width * height > std::numeric_limits<size_t>::max() / 4) {
        return false;
    }

    out.width = static_cast<int>(texture.mWidth);
    out.height = static_cast<int>(texture.mHeight);
    out.channels = 4;
    const size_t pixel_count = width * height;
    out.data.resize(pixel_count * 4);
    for (size_t i = 0; i < pixel_count; ++i) {
        const aiTexel& texel = texture.pcData[i];
        out.data[i * 4 + 0] = texel.r;
        out.data[i * 4 + 1] = texel.g;
        out.data[i * 4 + 2] = texel.b;
        out.data[i * 4 + 3] = texel.a;
    }
    return !out.data.empty();
}

bool get_material_texture(const aiMaterial& material, aiString& texture_path)
{
    if (material.GetTextureCount(aiTextureType_DIFFUSE) > 0 &&
        material.GetTexture(aiTextureType_DIFFUSE, 0, &texture_path) == AI_SUCCESS) {
        return true;
    }

    if (material.GetTextureCount(aiTextureType_BASE_COLOR) > 0 &&
        material.GetTexture(aiTextureType_BASE_COLOR, 0, &texture_path) == AI_SUCCESS) {
        return true;
    }

    return false;
}

std::array<float, 4> get_material_color(const aiMaterial& material)
{
    aiColor4D color(1.f, 1.f, 1.f, 1.f);
    if (material.Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS)
        return {color.r, color.g, color.b, color.a};
    if (material.Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS)
        return {color.r, color.g, color.b, color.a};
    return {1.f, 1.f, 1.f, 1.f};
}

bool collect_mesh(const aiMesh& mesh, size_t& vertex_offset, TexturedMesh& out, std::string& error,
                  std::vector<std::array<float, 4>>* raw_vertex_colors, bool precompute_colors)
{
    if (mesh.mNumVertices > static_cast<size_t>(std::numeric_limits<int>::max()) - vertex_offset) {
        error = "Assimp mesh has too many vertices for TexturedMesh indices";
        return false;
    }

    const auto factor = mesh.mMaterialIndex < out.material_colors.size()
        ? out.material_colors[mesh.mMaterialIndex] : std::array<float, 4>{1.f, 1.f, 1.f, 1.f};
    for (unsigned int i = 0; i < mesh.mNumVertices; ++i) {
        const aiVector3D& v = mesh.mVertices[i];
        out.vertices.push_back({v.x, v.y, v.z});
        const aiColor4D color = mesh.HasVertexColors(0) ? mesh.mColors[0][i] : aiColor4D(1.f, 1.f, 1.f, 1.f);
        if (raw_vertex_colors) {
            raw_vertex_colors->push_back({color.r, color.g, color.b, color.a});
        }
        if (precompute_colors) {
            std::array<float, 4> converted {color.r, color.g, color.b, color.a};
            for (size_t channel = 0; channel < converted.size(); ++channel) {
                const float value = converted[channel] * factor[channel];
                if (!std::isfinite(value)) {
                    error = "Assimp mesh has a non-finite vertex or material color";
                    return false;
                }
                const float linear = std::clamp(value, 0.f, 1.f);
                converted[channel] = channel == 3 ? linear : linear <= 0.0031308f
                    ? 12.92f * linear : 1.055f * std::pow(linear, 1.f / 2.4f) - 0.055f;
            }
            out.precomputed_vertex_colors.push_back(converted);
        }

        if (mesh.HasTextureCoords(0)) {
            const aiVector3D& uv = mesh.mTextureCoords[0][i];
            out.uvs.push_back({uv.x, uv.y});
        } else {
            out.uvs.push_back({0.f, 0.f});
        }
    }

    const int material_index = static_cast<int>(mesh.mMaterialIndex);
    for (unsigned int i = 0; i < mesh.mNumFaces; ++i) {
        const aiFace& face = mesh.mFaces[i];
        if (face.mNumIndices != 3)
            continue;
        if (face.mIndices[0] >= mesh.mNumVertices ||
            face.mIndices[1] >= mesh.mNumVertices ||
            face.mIndices[2] >= mesh.mNumVertices) {
            error = "Assimp mesh face index is out of bounds";
            return false;
        }
        out.indices.push_back({
            static_cast<int>(static_cast<size_t>(face.mIndices[0]) + vertex_offset),
            static_cast<int>(static_cast<size_t>(face.mIndices[1]) + vertex_offset),
            static_cast<int>(static_cast<size_t>(face.mIndices[2]) + vertex_offset)});
        out.material_ids.push_back(material_index);
        if (precompute_colors) {
            const auto& indices = out.indices.back();
            std::array<size_t, 3> average;
            for (size_t channel = 0; channel < average.size(); ++channel) {
                const float value = (out.precomputed_vertex_colors[indices[0]][channel] +
                                     out.precomputed_vertex_colors[indices[1]][channel] +
                                     out.precomputed_vertex_colors[indices[2]][channel]) / 3.f * 255.f;
                average[channel] = static_cast<size_t>(std::clamp(value, 0.f, 255.f));
            }
            out.precomputed_face_colors.push_back(average);
        }
    }

    vertex_offset += mesh.mNumVertices;
    return true;
}

void collect_materials(const aiScene& scene, const boost::filesystem::path& base_dir, TexturedMesh& out)
{
    out.material_texture_map.assign(scene.mNumMaterials, -1);
    out.material_colors.assign(scene.mNumMaterials, {1.f, 1.f, 1.f, 1.f});

    for (unsigned int material_index = 0; material_index < scene.mNumMaterials; ++material_index) {
        const aiMaterial* material = scene.mMaterials[material_index];
        if (!material)
            continue;

        out.material_colors[material_index] = get_material_color(*material);

        aiString texture_path;
        if (!get_material_texture(*material, texture_path))
            continue;

        TextureImage image;
        const aiTexture* embedded_texture = scene.GetEmbeddedTexture(texture_path.C_Str());
        if (embedded_texture) {
            if (!read_embedded_texture(*embedded_texture, image))
                continue;
        } else {
            const boost::filesystem::path resolved = resource_path::resolve_external_resource_path(
                base_dir, texture_path.C_Str(), "Assimp texture");
            if (resolved.empty()) {
                BOOST_LOG_TRIVIAL(warning) << "AssimpImport: texture file not found: "
                                           << texture_path.C_Str();
                continue;
            }
            if (!read_external_texture_file(resolved, image)) {
                BOOST_LOG_TRIVIAL(warning) << "AssimpImport: failed to read texture: "
                                           << resolved;
                continue;
            }
        }

        out.material_texture_map[material_index] = static_cast<int>(out.textures.size());
        out.textures.push_back(std::move(image));
    }
}

std::string scene_failure_summary(const std::string& path, const char* assimp_error)
{
    std::ostringstream ss;
    ss << "Assimp failed to import " << path;
    if (assimp_error && assimp_error[0] != '\0')
        ss << ": " << assimp_error;
    return ss.str();
}

} // namespace

bool load_assimp_textured_model(const std::string& path, TexturedMesh& out, std::string* error_message,
                               std::vector<std::array<float, 4>>* raw_vertex_colors)
{
    clear_textured_mesh(out);
    if (raw_vertex_colors) raw_vertex_colors->clear();

    Assimp::Importer importer;
    const unsigned int flags = assimp_import_flags(path);
    configure_importer(importer, path, flags);

    const aiScene* scene = importer.ReadFile(path, flags);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        const std::string message = scene_failure_summary(path, importer.GetErrorString());
        BOOST_LOG_TRIVIAL(error) << "AssimpImport: " << message;
        set_error_message(error_message, message);
        return false;
    }

    if (scene->mNumMeshes == 0) {
        const std::string message = "Assimp scene has no meshes: " + path;
        BOOST_LOG_TRIVIAL(error) << "AssimpImport: " << message;
        set_error_message(error_message, message);
        return false;
    }

    collect_materials(*scene, boost::filesystem::path(path).parent_path(), out);

    // Precomputed colors bypass texture sampling. Only use them for a glTF
    // scene with COLOR_0 and no color textures, including on other meshes.
    bool has_vertex_colors = false;
    bool has_color_texture = false;
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* mesh = scene->mMeshes[i];
        if (!mesh || !mesh->HasPositions())
            continue;
        has_vertex_colors |= mesh->HasVertexColors(0);
        aiString texture_path;
        if (mesh->mMaterialIndex < scene->mNumMaterials && scene->mMaterials[mesh->mMaterialIndex])
            has_color_texture |= get_material_texture(*scene->mMaterials[mesh->mMaterialIndex], texture_path);
    }
    const bool precompute_colors = has_vertex_colors && !has_color_texture &&
        (boost::algorithm::iends_with(path, ".glb") || boost::algorithm::iends_with(path, ".gltf"));

    size_t vertex_offset = 0;
    for (unsigned int mesh_index = 0; mesh_index < scene->mNumMeshes; ++mesh_index) {
        const aiMesh* mesh = scene->mMeshes[mesh_index];
        if (!mesh || !mesh->HasPositions())
            continue;
        std::string mesh_error;
        if (!collect_mesh(*mesh, vertex_offset, out, mesh_error, raw_vertex_colors, precompute_colors)) {
            const std::string message = mesh_error + ": " + path;
            BOOST_LOG_TRIVIAL(error) << "AssimpImport: " << message;
            set_error_message(error_message, message);
            clear_textured_mesh(out);
            return false;
        }
    }

    if (out.vertices.empty() || out.indices.empty()) {
        const std::string message = "Assimp extracted no valid triangles: " + path;
        BOOST_LOG_TRIVIAL(error) << "AssimpImport: " << message;
        set_error_message(error_message, message);
        clear_textured_mesh(out);
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "AssimpImport: loaded " << out.vertices.size()
                            << " vertices, " << out.indices.size()
                            << " triangles, " << out.textures.size()
                            << " textures from " << path;
    return true;
}

} // namespace Slic3r
