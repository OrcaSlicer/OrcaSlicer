#include "GLB.hpp"

#include "libslic3r/TriangleMesh.hpp"

#ifdef _WIN32
#include "libslic3r/Utils.hpp"
#endif

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Slic3r::GLB {
namespace {

using json = nlohmann::json;

constexpr uint32_t GLB_MAGIC = 0x46546c67;
constexpr uint32_t GLB_VERSION = 2;
constexpr uint32_t JSON_CHUNK = 0x4e4f534a;
constexpr uint32_t BIN_CHUNK = 0x004e4942;
constexpr uint32_t ARRAY_BUFFER = 34962;
constexpr uint32_t ELEMENT_ARRAY_BUFFER = 34963;
constexpr uint32_t FLOAT_COMPONENT = 5126;
constexpr uint32_t UNSIGNED_INT_COMPONENT = 5125;
constexpr uint32_t LINE_LOOP = 2;
constexpr uint32_t LINE_STRIP = 3;
constexpr uint32_t TRIANGLES = 4;
constexpr size_t VERTEX_STRIDE = sizeof(float) * 8;

size_t padded_size(size_t size) { return (size + 3) & ~size_t(3); }

template<class T> void append_le(std::vector<uint8_t>& data, T value)
{
    static_assert(std::is_integral_v<T>);
    using U = std::make_unsigned_t<T>;
    const U encoded = static_cast<U>(value);
    for (size_t i = 0; i < sizeof(T); ++i)
        data.push_back(uint8_t(encoded >> (8 * i)));
}

template<class T> T get_le(const uint8_t* data, size_t size, size_t offset)
{
    static_assert(std::is_integral_v<T>);
    if (offset > size || sizeof(T) > size - offset)
        throw std::runtime_error("GLB region out of bounds");
    using U = std::make_unsigned_t<T>;
    U value{};
    for (size_t i = 0; i < sizeof(T); ++i)
        value |= U(data[offset + i]) << (8 * i);
    return static_cast<T>(value);
}

void append_float(std::vector<uint8_t>& data, float value, const char* field)
{
    if (!std::isfinite(value))
        throw std::runtime_error(std::string("GLB contains non-finite ") + field);
    uint32_t encoded;
    std::memcpy(&encoded, &value, sizeof(value));
    append_le(data, encoded);
}

size_t append_buffer_view(json& document, size_t offset, size_t length, uint32_t target, size_t stride = 0)
{
    json view = {{"buffer", 0}, {"byteOffset", offset}, {"byteLength", length}};
    if (target != 0)
        view["target"] = target;
    if (stride != 0)
        view["byteStride"] = stride;
    document["bufferViews"].push_back(std::move(view));
    return document["bufferViews"].size() - 1;
}

size_t append_accessor(json& document, size_t view, size_t offset, uint32_t component_type,
                       size_t count, const char* type)
{
    document["accessors"].push_back({{"bufferView", view}, {"byteOffset", offset},
                                      {"componentType", component_type}, {"count", count}, {"type", type}});
    return document["accessors"].size() - 1;
}

const json& array_at(const json& document, const char* key, size_t index)
{
    if (!document.contains(key) || !document[key].is_array() || index >= document[key].size())
        throw std::runtime_error(std::string("Invalid GLB ") + key + " reference");
    return document[key][index];
}

size_t accessor_count(const json& document, size_t index)
{
    const json& accessor = array_at(document, "accessors", index);
    if (!accessor.contains("count") || !accessor["count"].is_number_unsigned())
        throw std::runtime_error("Invalid GLB accessor");
    return accessor["count"].get<size_t>();
}

size_t component_size(uint32_t type)
{
    switch (type) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: throw std::runtime_error("Invalid GLB accessor component type");
    }
}

size_t component_count(const std::string& type)
{
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4" || type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    throw std::runtime_error("Invalid GLB accessor type");
}

} // namespace

std::vector<uint8_t> serialize(const Scene& scene)
{
    if (scene.vertices.empty() || scene.indices.empty() || scene.primitives.empty())
        throw std::runtime_error("GLB scene has an empty required section");
    for (size_t i = 0; i < 3; ++i)
        if (!std::isfinite(scene.bounds_min[i]) || !std::isfinite(scene.bounds_max[i]) ||
            scene.bounds_min[i] > scene.bounds_max[i])
            throw std::runtime_error("GLB scene contains invalid bounds");

    json scene_object = {{"nodes", json::array({0})}};
    if (!scene.extras.is_null() && !scene.extras.empty())
        scene_object["extras"] = scene.extras;
    json node = {{"mesh", 0}};
    if (scene.root_rotation != std::array<double, 4>{{0, 0, 0, 1}})
        node["rotation"] = scene.root_rotation;
    json document = {
        {"asset", {{"version", "2.0"}, {"generator", "OrcaSlicer"}}}, {"scene", 0},
        {"scenes", json::array({std::move(scene_object)})}, {"nodes", json::array({std::move(node)})},
        {"meshes", json::array()}, {"materials", json::array()}, {"accessors", json::array()},
        {"bufferViews", json::array()}, {"buffers", json::array()}
    };

    std::vector<uint8_t> binary;
    for (const Vertex& vertex : scene.vertices) {
        for (float value : vertex.position) append_float(binary, value, "position");
        float normal_length_squared = 0;
        for (float value : vertex.normal) {
            append_float(binary, value, "normal");
            normal_length_squared += value * value;
        }
        if (normal_length_squared < 0.25f || normal_length_squared > 2.25f)
            throw std::runtime_error("GLB scene contains an invalid normal");
        for (float value : vertex.uv) append_float(binary, value, "UV");
    }
    const size_t vertex_view = append_buffer_view(document, 0, binary.size(), ARRAY_BUFFER, VERTEX_STRIDE);
    const size_t positions = append_accessor(document, vertex_view, 0, FLOAT_COMPONENT, scene.vertices.size(), "VEC3");
    document["accessors"][positions]["min"] = scene.bounds_min;
    document["accessors"][positions]["max"] = scene.bounds_max;
    const size_t normals = append_accessor(document, vertex_view, 3 * sizeof(float), FLOAT_COMPONENT,
                                            scene.vertices.size(), "VEC3");
    const size_t texcoords = append_accessor(document, vertex_view, 6 * sizeof(float), FLOAT_COMPONENT,
                                              scene.vertices.size(), "VEC2");

    const size_t indices_offset = binary.size();
    for (uint32_t index : scene.indices) {
        if (index >= scene.vertices.size())
            throw std::runtime_error("GLB scene index out of range");
        append_le(binary, index);
    }
    const size_t index_view = append_buffer_view(document, indices_offset, binary.size() - indices_offset,
                                                  ELEMENT_ARRAY_BUFFER);

    for (const Material& material : scene.materials) {
        json value = {{"pbrMetallicRoughness", {{"baseColorFactor", {
                           material.base_color[0] / 255.0, material.base_color[1] / 255.0,
                           material.base_color[2] / 255.0, material.base_color[3] / 255.0}},
                                                  {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}},
                      {"doubleSided", true}};
        if (!material.name.empty()) value["name"] = material.name;
        if (!material.extras.is_null() && !material.extras.empty()) value["extras"] = material.extras;
        if (material.base_color[3] != 255) value["alphaMode"] = "BLEND";
        document["materials"].push_back(std::move(value));
    }

    json primitives = json::array();
    for (const Primitive& primitive : scene.primitives) {
        if (primitive.index_count == 0 || primitive.index_count % 3 != 0 ||
            uint64_t(primitive.first_index) + primitive.index_count > scene.indices.size() ||
            primitive.material < -1 || (primitive.material >= 0 && size_t(primitive.material) >= scene.materials.size()))
            throw std::runtime_error("GLB scene contains an invalid primitive");
        const size_t indices = append_accessor(document, index_view, size_t(primitive.first_index) * sizeof(uint32_t),
                                                UNSIGNED_INT_COMPONENT, primitive.index_count, "SCALAR");
        json value = {{"attributes", {{"POSITION", positions}, {"NORMAL", normals}, {"TEXCOORD_0", texcoords}}},
                      {"indices", indices}, {"mode", TRIANGLES}};
        if (primitive.material >= 0) value["material"] = primitive.material;
        if (!primitive.extras.is_null() && !primitive.extras.empty()) value["extras"] = primitive.extras;
        primitives.push_back(std::move(value));
    }

    for (const Polyline& polyline : scene.polylines) {
        if (polyline.points.size() < 2)
            continue;
        const size_t offset = binary.size();
        for (const auto& point : polyline.points)
            for (float value : point) append_float(binary, value, "polyline point");
        const size_t view = append_buffer_view(document, offset, binary.size() - offset, ARRAY_BUFFER);
        const size_t accessor = append_accessor(document, view, 0, FLOAT_COMPONENT, polyline.points.size(), "VEC3");
        json value = {{"attributes", {{"POSITION", accessor}}},
                      {"mode", polyline.closed ? LINE_LOOP : LINE_STRIP}};
        if (!polyline.extras.is_null() && !polyline.extras.empty()) value["extras"] = polyline.extras;
        primitives.push_back(std::move(value));
    }

    document["meshes"].push_back({{"name", scene.name}, {"primitives", std::move(primitives)}});
    document["buffers"].push_back({{"byteLength", binary.size()}});
    std::string json_text = document.dump();
    json_text.resize(padded_size(json_text.size()), ' ');
    binary.resize(padded_size(binary.size()), 0);
    const uint64_t total_size = 12 + 8 + json_text.size() + 8 + binary.size();
    if (total_size > DEFAULT_MAX_FILE_SIZE)
        throw std::runtime_error("GLB exceeds its 32-bit file-size limit");

    std::vector<uint8_t> output;
    output.reserve(size_t(total_size));
    append_le(output, GLB_MAGIC); append_le(output, GLB_VERSION); append_le(output, uint32_t(total_size));
    append_le(output, uint32_t(json_text.size())); append_le(output, JSON_CHUNK);
    output.insert(output.end(), json_text.begin(), json_text.end());
    append_le(output, uint32_t(binary.size())); append_le(output, BIN_CHUNK);
    output.insert(output.end(), binary.begin(), binary.end());
    validate(output);
    return output;
}

DocumentInfo validate(const uint8_t* data, size_t size, uint64_t max_file_size)
{
    if (data == nullptr || size < 28 || size > max_file_size || get_le<uint32_t>(data, size, 0) != GLB_MAGIC)
        throw std::runtime_error("Invalid GLB header");
    const uint32_t version = get_le<uint32_t>(data, size, 4);
    const uint32_t declared_size = get_le<uint32_t>(data, size, 8);
    if (version != GLB_VERSION || declared_size != size)
        throw std::runtime_error("Unsupported or truncated GLB");
    const uint32_t json_size = get_le<uint32_t>(data, size, 12);
    if (json_size == 0 || json_size % 4 != 0 || get_le<uint32_t>(data, size, 16) != JSON_CHUNK ||
        json_size > size - 28)
        throw std::runtime_error("Invalid GLB JSON chunk");
    const size_t binary_header = 20 + json_size;
    if (get_le<uint32_t>(data, size, binary_header + 4) != BIN_CHUNK)
        throw std::runtime_error("Invalid GLB binary chunk");
    const uint32_t binary_size = get_le<uint32_t>(data, size, binary_header);
    if (binary_size % 4 != 0 || binary_size != size - binary_header - 8)
        throw std::runtime_error("Invalid GLB binary chunk length");

    json document;
    try {
        document = json::parse(data + 20, data + 20 + json_size);
    } catch (const json::exception& error) {
        throw std::runtime_error(std::string("Invalid GLB JSON: ") + error.what());
    }
    if (!document.contains("asset") || document["asset"].value("version", "") != "2.0" ||
        !document.contains("buffers") || document["buffers"].size() != 1 ||
        document["buffers"][0].value("byteLength", size_t(binary_size + 1)) > binary_size)
        throw std::runtime_error("Invalid GLB document structure");

    const size_t declared_binary_size = document["buffers"][0]["byteLength"].get<size_t>();
    if (!document.contains("bufferViews") || !document["bufferViews"].is_array() ||
        !document.contains("accessors") || !document["accessors"].is_array())
        throw std::runtime_error("GLB is missing buffer declarations");
    for (const json& view : document["bufferViews"]) {
        const size_t offset = view.value("byteOffset", size_t(0));
        const size_t length = view.value("byteLength", size_t(0));
        if (view.value("buffer", size_t(1)) != 0 || offset > declared_binary_size ||
            length > declared_binary_size - offset)
            throw std::runtime_error("GLB buffer view is out of range");
    }
    for (const json& accessor : document["accessors"]) {
        const size_t view_index = accessor.value("bufferView", document["bufferViews"].size());
        const json& view = array_at(document, "bufferViews", view_index);
        const size_t count = accessor.value("count", size_t(0));
        const size_t element_size = component_size(accessor.value("componentType", uint32_t(0))) *
                                    component_count(accessor.value("type", std::string()));
        const size_t stride = view.value("byteStride", element_size);
        const size_t offset = accessor.value("byteOffset", size_t(0));
        const size_t view_length = view["byteLength"].get<size_t>();
        if (count == 0 || stride < element_size || offset > view_length ||
            (count - 1) > (view_length - offset) / stride ||
            element_size > view_length - offset - (count - 1) * stride)
            throw std::runtime_error("GLB accessor is out of range");
    }

    const json& mesh = array_at(document, "meshes", 0);
    if (!mesh.contains("primitives") || !mesh["primitives"].is_array() || mesh["primitives"].empty())
        throw std::runtime_error("GLB contains no primitives");
    const json& scene = array_at(document, "scenes", document.value("scene", size_t(0)));
    DocumentInfo info;
    info.version = version;
    info.file_size = declared_size;
    info.material_count = document.value("materials", json::array()).size();
    info.scene_extras = scene.value("extras", json::object());
    for (const json& primitive : mesh["primitives"]) {
        if (!primitive.contains("attributes") || !primitive["attributes"].contains("POSITION"))
            throw std::runtime_error("GLB primitive has no positions");
        if (primitive.value("mode", TRIANGLES) == TRIANGLES) {
            if (!primitive.contains("indices"))
                throw std::runtime_error("GLB triangle primitive has no indices");
            info.index_count += accessor_count(document, primitive["indices"].get<size_t>());
            info.vertex_count = std::max(info.vertex_count,
                accessor_count(document, primitive["attributes"]["POSITION"].get<size_t>()));
            ++info.primitive_count;
        }
    }
    if (info.vertex_count == 0 || info.index_count == 0 || info.primitive_count == 0)
        throw std::runtime_error("GLB contains an empty triangle scene");
    return info;
}

DocumentInfo validate(const std::vector<uint8_t>& data, uint64_t max_file_size)
{
    return validate(data.data(), data.size(), max_file_size);
}

} // namespace Slic3r::GLB

void Slic3r::store_glb(const char* path, const GLB::Scene& scene)
{
    if (path == nullptr)
        throw std::invalid_argument("GLB output path is null");
    namespace fs = boost::filesystem;
    const std::vector<uint8_t> bytes = GLB::serialize(scene);
    const fs::path target(path);
    if (!target.parent_path().empty()) fs::create_directories(target.parent_path());
    fs::path temp = target;
    temp += ".tmp-" + fs::unique_path("%%%%-%%%%-%%%%").string();
    try {
        boost::nowide::ofstream output(temp.string(), std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        output.flush();
        output.close();
        if (!output) throw std::runtime_error("Failed to write GLB");
#ifdef _WIN32
        const std::error_code error = rename_file(temp.string(), target.string());
        if (error) throw std::runtime_error("Failed to publish GLB: " + error.message());
#else
        boost::system::error_code error;
        fs::rename(temp, target, error);
        if (error) throw std::runtime_error("Failed to publish GLB: " + error.message());
#endif
    } catch (...) {
        boost::system::error_code ignored;
        fs::remove(temp, ignored);
        throw;
    }
}

void Slic3r::store_glb(const char* path, const TriangleMesh* mesh)
{
    if (mesh == nullptr)
        throw std::invalid_argument("GLB input mesh is null");
    if (mesh->its.vertices.empty() || mesh->its.indices.empty())
        throw std::invalid_argument("GLB input mesh is empty");

    GLB::Scene scene;
    scene.name = "OrcaSlicer model";
    scene.root_rotation = {{-0.7071067811865476, 0, 0, 0.7071067811865476}};
    scene.vertices.reserve(mesh->its.indices.size() * 3);
    scene.indices.reserve(mesh->its.indices.size() * 3);
    for (const auto& face : mesh->its.indices) {
        for (size_t corner = 0; corner < 3; ++corner)
            if (face[int(corner)] < 0 || size_t(face[int(corner)]) >= mesh->its.vertices.size())
                throw std::invalid_argument("GLB input mesh contains an invalid index");
        const Vec3f& a = mesh->its.vertices[size_t(face[0])];
        const Vec3f& b = mesh->its.vertices[size_t(face[1])];
        const Vec3f& c = mesh->its.vertices[size_t(face[2])];
        Vec3f normal = (b - a).cross(c - a);
        if (normal.squaredNorm() > 0.0f)
            normal.normalize();
        for (size_t corner = 0; corner < 3; ++corner) {
            const Vec3f& position = mesh->its.vertices[size_t(face[int(corner)])];
            scene.vertices.push_back({{{position.x(), position.y(), position.z()}},
                                      {{normal.x(), normal.y(), normal.z()}}, {}});
            scene.indices.push_back(uint32_t(scene.indices.size()));
        }
    }
    const auto bounds = mesh->bounding_box();
    scene.bounds_min = {{float(bounds.min.x()), float(bounds.min.y()), float(bounds.min.z())}};
    scene.bounds_max = {{float(bounds.max.x()), float(bounds.max.y()), float(bounds.max.z())}};
    scene.primitives.push_back({0, uint32_t(scene.indices.size()), -1, {}});
    store_glb(path, scene);
}
