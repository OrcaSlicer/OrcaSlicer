#include "GlbGeometryEditing.hpp"
#include "ModelArtifact.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <Eigen/Geometry>
#include <Eigen/LU>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace Slic3r::AI {
namespace {
using Json = nlohmann::json;
constexpr size_t max_bytes = 512ull * 1024 * 1024;
constexpr size_t max_vertices = 6000000;
constexpr size_t max_faces = 2000000;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

size_t unsigned_value(const Json& value)
{
    require(value.is_number_integer() && (value.is_number_unsigned() || value.get<int64_t>() >= 0),
        "GLB contains an invalid nonnegative integer.");
    const uint64_t number = value.get<uint64_t>();
    require(number <= max_bytes, "GLB integer exceeds the editing limit.");
    return size_t(number);
}

size_t number(const Json& object, const char* name, size_t fallback = 0)
{
    return object.contains(name) ? unsigned_value(object.at(name)) : fallback;
}

const Json& item(const Json& values, size_t index)
{
    require(values.is_array() && index < values.size(), "GLB contains an invalid array reference.");
    return values.at(index);
}

void no_extensions(const Json& value)
{
    require(!value.contains("extensions") || value.at("extensions").empty(),
        "This GLB geometry extension is not supported for texture-preserving edits.");
}

uint32_t u32(const unsigned char* data)
{
    return uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}

void append_u32(std::vector<unsigned char>& data, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) data.push_back(static_cast<unsigned char>(value >> shift));
}

void append_float(std::vector<unsigned char>& data, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(data, bits);
}

std::string bytes_sha256(const std::vector<unsigned char>& bytes)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    unsigned int length = 0;
    require(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) == 1 && length == 32,
        "GLB source snapshot hash could not be computed.");
    std::ostringstream hex;
    for (size_t i = 0; i < length; ++i) hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
    return hex.str();
}

double real(const Json& value)
{
    require(value.is_number(), "GLB transform contains a nonnumeric value.");
    const double result = value.get<double>();
    require(std::isfinite(result), "GLB transform contains a non-finite value.");
    return result;
}

Eigen::Matrix4d node_matrix(const Json& node)
{
    Eigen::Matrix4d matrix = Eigen::Matrix4d::Identity();
    if (node.contains("matrix")) {
        require(!node.contains("translation") && !node.contains("rotation") && !node.contains("scale"),
            "GLB node combines a matrix with TRS transforms.");
        const auto& values = node.at("matrix");
        require(values.is_array() && values.size() == 16, "GLB node matrix is invalid.");
        for (size_t i = 0; i < 16; ++i) matrix(int(i % 4), int(i / 4)) = real(values[i]);
        require(matrix.row(3).isApprox(Eigen::RowVector4d(0, 0, 0, 1), 1e-12), "GLB node matrix must be affine.");
    } else {
        Eigen::Vector3d translation = Eigen::Vector3d::Zero(), scale = Eigen::Vector3d::Ones();
        if (node.contains("translation")) {
            const auto& values = node.at("translation");
            require(values.is_array() && values.size() == 3, "GLB node translation is invalid.");
            for (int i = 0; i < 3; ++i) translation[i] = real(values[i]);
        }
        if (node.contains("scale")) {
            const auto& values = node.at("scale");
            require(values.is_array() && values.size() == 3, "GLB node scale is invalid.");
            for (int i = 0; i < 3; ++i) scale[i] = real(values[i]);
        }
        Eigen::Quaterniond rotation = Eigen::Quaterniond::Identity();
        if (node.contains("rotation")) {
            const auto& values = node.at("rotation");
            require(values.is_array() && values.size() == 4, "GLB node rotation is invalid.");
            rotation = Eigen::Quaterniond(real(values[3]), real(values[0]), real(values[1]), real(values[2]));
            require(std::abs(rotation.norm() - 1.0) < 1e-5, "GLB node rotation must be a unit quaternion.");
            rotation.normalize();
        }
        matrix.block<3, 3>(0, 0) = rotation.toRotationMatrix() * scale.asDiagonal();
        matrix.block<3, 1>(0, 3) = translation;
    }
    return matrix;
}

Eigen::Matrix4d model_transform(const Json& doc)
{
    const auto& nodes = doc.at("nodes");
    const auto& scenes = doc.at("scenes");
    require(nodes.is_array() && !nodes.empty() && nodes.size() <= 256,
        "GLB node hierarchy is missing or exceeds the editing limit.");
    require(scenes.is_array() && scenes.size() == 1 && number(doc, "scene") == 0,
        "Texture-preserving geometry edits currently require one GLB scene.");
    require(!doc.contains("animations") || doc.at("animations").empty(), "Bake GLB animations before editing geometry.");
    require(!doc.contains("skins") || doc.at("skins").empty(), "Bake GLB skins before editing geometry.");
    std::vector<bool> visited(nodes.size(), false);
    size_t mesh_count = 0;
    Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
    std::function<void(size_t, const Eigen::Matrix4d&)> visit = [&](size_t index, const Eigen::Matrix4d& parent) {
        const auto& node = item(nodes, index);
        require(!visited[index], "GLB contains a repeated or cyclic node reference.");
        visited[index] = true;
        no_extensions(node);
        require(!node.contains("skin") && !node.contains("weights"), "Bake animated GLB geometry before editing.");
        const Eigen::Matrix4d world = parent * node_matrix(node);
        require(world.allFinite(), "GLB transform is not finite.");
        if (node.contains("mesh")) {
            require(number(node, "mesh") == 0 && ++mesh_count == 1,
                "Texture-preserving geometry edits do not yet support multiple mesh instances.");
            result = world;
        }
        if (node.contains("children")) {
            require(node.at("children").is_array(), "GLB node children must be an array.");
            for (const auto& child : node.at("children")) visit(unsigned_value(child), world);
        }
    };
    const auto& roots = scenes.at(0).at("nodes");
    require(roots.is_array(), "GLB scene roots must be an array.");
    for (const auto& root : roots) visit(unsigned_value(root), Eigen::Matrix4d::Identity());
    require(mesh_count == 1 && std::all_of(visited.begin(), visited.end(), [](bool seen) { return seen; }),
        "Texture-preserving geometry edits require one fully referenced static mesh.");
    const Eigen::Matrix3d linear = result.block<3, 3>(0, 0);
    const double scale = linear.col(0).norm() * linear.col(1).norm() * linear.col(2).norm();
    require(scale > 0 && std::isfinite(scale) && std::abs(linear.determinant()) > scale * 1e-10,
        "GLB transform is singular or too ill-conditioned for geometry editing.");
    Eigen::Matrix4d axes = Eigen::Matrix4d::Zero();
    axes(0, 0) = 1000; axes(1, 2) = -1000; axes(2, 1) = 1000; axes(3, 3) = 1;
    result = axes * result;
    require(result.inverse().allFinite(), "GLB inverse transform is not finite.");
    return result;
}

struct DenseAccessor {
    size_t offset, stride, count, width;
    int component;
};

DenseAccessor accessor(const Json& doc, size_t index, size_t binary_size, bool positions)
{
    const auto& value = item(doc.at("accessors"), index);
    no_extensions(value);
    require(!value.contains("sparse"), "Sparse GLB geometry accessors are not supported for texture-preserving edits.");
    require(!value.value("normalized", false), "Quantized GLB geometry must be decoded before texture-preserving edits.");
    const int component = value.at("componentType").get<int>();
    require(positions ? component == 5126 && value.at("type") == "VEC3" :
        (component == 5121 || component == 5123 || component == 5125) && value.at("type") == "SCALAR",
        "Texture-preserving edits require float VEC3 positions/normals and unsigned scalar indices.");
    const size_t width = component == 5121 ? 1 : component == 5123 ? 2 : 4;
    const size_t element = positions ? 12 : width;
    const size_t count = number(value, "count");
    require(count > 0 && count <= max_vertices, "GLB accessor count exceeds the editing limit.");
    const auto& view = item(doc.at("bufferViews"), number(value, "bufferView", max_bytes));
    no_extensions(view);
    require(number(view, "buffer", max_bytes) == 0, "GLB geometry must use the embedded buffer.");
    const size_t start = number(view, "byteOffset"), length = number(view, "byteLength");
    require(start <= binary_size && length <= binary_size - start, "GLB buffer view exceeds the embedded buffer.");
    const size_t local = number(value, "byteOffset"), stride = number(view, "byteStride", element);
    require(!view.contains("byteStride") || positions, "GLB indices cannot use a strided buffer view.");
    require(stride >= element && stride <= 252 && stride % width == 0 && local % width == 0 && start % width == 0 &&
        (!positions || (start + local) % 4 == 0) && local <= length && element <= length - local &&
        count - 1 <= (length - local - element) / stride, "GLB geometry accessor exceeds its buffer view or has invalid alignment.");
    return {start + local, stride, count, width, component};
}

std::vector<Vec3f> read_vectors(const std::vector<unsigned char>& binary, const DenseAccessor& layout,
    const std::function<void()>& checkpoint)
{
    std::vector<Vec3f> values(layout.count);
    for (size_t i = 0; i < layout.count; ++i) {
        if ((i & 4095) == 0 && checkpoint) checkpoint();
        for (size_t axis = 0; axis < 3; ++axis) {
            const uint32_t bits = u32(binary.data() + layout.offset + i * layout.stride + axis * 4);
            std::memcpy(&values[i][axis], &bits, 4);
        }
        require(values[i].allFinite(), "GLB geometry contains a non-finite value.");
    }
    return values;
}

void append_vectors(Json& doc, std::vector<unsigned char>& binary, size_t old_accessor,
    const std::vector<Vec3f>& values, const char* attribute, const std::function<void()>& checkpoint)
{
    while (binary.size() % 4) binary.push_back(0);
    const size_t offset = binary.size();
    require(values.size() <= (max_bytes - offset) / 12, "The edited GLB exceeds 512 MB.");
    Vec3f minimum = values.front(), maximum = minimum;
    for (size_t i = 0; i < values.size(); ++i) {
        if ((i & 4095) == 0 && checkpoint) checkpoint();
        require(values[i].allFinite(), "The edited GLB contains invalid geometry.");
        for (int axis = 0; axis < 3; ++axis) append_float(binary, values[i][axis]);
        minimum = minimum.cwiseMin(values[i]); maximum = maximum.cwiseMax(values[i]);
    }
    Json replacement = doc.at("accessors").at(old_accessor);
    replacement["bufferView"] = doc.at("bufferViews").size();
    replacement["byteOffset"] = 0;
    replacement["min"] = {minimum.x(), minimum.y(), minimum.z()};
    replacement["max"] = {maximum.x(), maximum.y(), maximum.z()};
    doc["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", values.size() * 12}, {"target", 34962}});
    doc["meshes"][0]["primitives"][0]["attributes"][attribute] = doc.at("accessors").size();
    doc["accessors"].push_back(std::move(replacement));
}
} // namespace

struct GlbGeometrySource {
    boost::filesystem::path path;
    std::string sha256;
    Json doc;
    std::vector<unsigned char> original, binary;
    std::vector<Vec3f> positions, normals;
    std::vector<size_t> indices, editor_to_source;
    indexed_triangle_set editor_mesh;
    Eigen::Matrix4d world;
    size_t position_accessor {0}, normal_accessor {0};
};

std::shared_ptr<GlbGeometrySource> read_glb_geometry_source(const boost::filesystem::path& path,
    const indexed_triangle_set& editor_mesh, const std::function<void()>& checkpoint)
{
    if (checkpoint) checkpoint();
    auto source = std::make_shared<GlbGeometrySource>();
    source->path = path;
    const size_t length = size_t(boost::filesystem::file_size(path));
    require(length >= 28 && length <= max_bytes, "GLB file is missing or exceeds the editing limit.");
    source->original.resize(length);
    boost::filesystem::ifstream input(path, std::ios::binary);
    for (size_t offset = 0; offset < length; offset += 65536) {
        if (checkpoint) checkpoint();
        const size_t count = std::min(size_t(65536), length - offset);
        input.read(reinterpret_cast<char*>(source->original.data() + offset), count);
        require(bool(input), "GLB source could not be read completely.");
    }
    const auto* bytes = source->original.data();
    require(u32(bytes) == 0x46546c67 && u32(bytes + 4) == 2 && u32(bytes + 8) == length && u32(bytes + 16) == 0x4e4f534a,
        "Texture-preserving edits require a valid GLB 2.0 file.");
    const size_t json_size = u32(bytes + 12);
    require(json_size > 0 && json_size % 4 == 0 && json_size <= 32 * 1024 * 1024 && json_size <= length - 28,
        "GLB JSON chunk is invalid.");
    const size_t bin_header = 20 + json_size;
    const size_t bin_size = u32(bytes + bin_header);
    require(u32(bytes + bin_header + 4) == 0x004e4942 && bin_size % 4 == 0 && bin_size == length - bin_header - 8,
        "Texture-preserving edits require exactly one JSON chunk and one embedded BIN chunk.");
    source->doc = Json::parse(bytes + 20, bytes + 20 + json_size);
    const auto& doc = source->doc;
    require(doc.at("asset").at("version") == "2.0", "Only GLB 2.0 geometry can be edited.");
    no_extensions(doc);
    for (const char* field : {"extensionsUsed", "extensionsRequired"}) if (doc.contains(field)) {
        require(doc.at(field).is_array(), "GLB extension list is invalid.");
        for (const auto& extension : doc.at(field))
            // Specular is material-only; the complete material and texture payload is retained.
            require(extension == "KHR_materials_unlit" || extension == "KHR_materials_specular" ||
                extension == "KHR_texture_transform" || extension == "KHR_mesh_quantization",
                "This GLB extension is not supported for texture-preserving geometry edits.");
    }
    require(doc.at("buffers").size() == 1 && !doc.at("buffers")[0].contains("uri"), "GLB geometry must be embedded in one buffer.");
    const size_t payload_size = number(doc.at("buffers")[0], "byteLength");
    require(payload_size <= bin_size && bin_size - payload_size <= 3, "GLB binary payload length is invalid.");
    source->binary.assign(bytes + bin_header + 8, bytes + bin_header + 8 + payload_size);
    for (const auto& image : doc.value("images", Json::array())) if (image.contains("uri"))
        require(image.at("uri").get<std::string>().rfind("data:", 0) == 0, "GLB images must be embedded for geometry editing.");
    require(doc.at("meshes").is_array() && doc.at("meshes").size() == 1,
        "Texture-preserving geometry edits currently require one GLB mesh.");
    const auto& mesh = doc.at("meshes")[0];
    no_extensions(mesh);
    require(!mesh.contains("weights") && mesh.at("primitives").is_array() && mesh.at("primitives").size() == 1,
        "Texture-preserving geometry edits currently require one static material primitive.");
    const auto& primitive = mesh.at("primitives")[0];
    no_extensions(primitive);
    require(number(primitive, "mode", 4) == 4 && !primitive.contains("targets"), "GLB geometry must contain static triangles.");
    const auto& attributes = primitive.at("attributes");
    require(attributes.is_object(), "GLB attributes must be an object.");
    for (auto it = attributes.begin(); it != attributes.end(); ++it)
        require(it.key() == "POSITION" || it.key() == "NORMAL" || it.key() == "TEXCOORD_0" || it.key() == "COLOR_0",
            "This GLB geometry attribute (including explicit tangents) is not supported for texture-preserving edits.");
    source->world = model_transform(doc);
    source->position_accessor = unsigned_value(attributes.at("POSITION"));
    source->positions = read_vectors(source->binary, accessor(doc, source->position_accessor, payload_size, true), checkpoint);
    if (attributes.contains("NORMAL")) {
        source->normal_accessor = unsigned_value(attributes.at("NORMAL"));
        require(source->normal_accessor != source->position_accessor, "GLB positions and normals cannot share an accessor.");
        source->normals = read_vectors(source->binary, accessor(doc, source->normal_accessor, payload_size, true), checkpoint);
        require(source->normals.size() == source->positions.size(), "GLB normals do not match the position count.");
    }
    for (const char* semantic : {"TEXCOORD_0", "COLOR_0"}) if (attributes.contains(semantic)) {
        const size_t attribute = unsigned_value(attributes.at(semantic));
        require(attribute != source->position_accessor && (source->normals.empty() || attribute != source->normal_accessor),
            "GLB geometry accessors cannot also be used for UVs or colors.");
    }
    if (primitive.contains("indices")) {
        const auto layout = accessor(doc, unsigned_value(primitive.at("indices")), payload_size, false);
        require(layout.count % 3 == 0 && layout.count / 3 <= max_faces, "GLB triangle count is invalid or excessive.");
        source->indices.resize(layout.count);
        for (size_t i = 0; i < layout.count; ++i) {
            if ((i & 4095) == 0 && checkpoint) checkpoint();
            const auto* value = source->binary.data() + layout.offset + i * layout.stride;
            const uint32_t index = layout.width == 1 ? value[0] : layout.width == 2 ? uint32_t(value[0]) | uint32_t(value[1]) << 8 : u32(value);
            require(index < source->positions.size(), "GLB triangle index is out of range.");
            source->indices[i] = index;
        }
    } else {
        require(source->positions.size() % 3 == 0 && source->positions.size() / 3 <= max_faces, "Unindexed GLB must contain complete triangles.");
        source->indices.resize(source->positions.size());
        for (size_t i = 0; i < source->indices.size(); ++i) source->indices[i] = i;
    }
    require(editor_mesh.indices.size() * 3 == source->indices.size(), "GLB triangle mapping changed during import; reload a supported model.");
    std::vector<size_t> source_to_editor(source->positions.size(), max_vertices);
    for (size_t vertex : source->indices) if (source_to_editor[vertex] == max_vertices) {
        source_to_editor[vertex] = source->editor_to_source.size();
        source->editor_to_source.push_back(vertex);
    }
    require(source->editor_to_source.size() == editor_mesh.vertices.size(), "GLB vertex mapping changed during import; geometry editing was not applied.");
    // Assimp's first-use index compaction is only a candidate mapping. Verify
    // every vertex AND every triangle; never use an unverified import ordinal.
    for (size_t i = 0; i < source->editor_to_source.size(); ++i) {
        if ((i & 4095) == 0 && checkpoint) checkpoint();
        const Vec3d world = (source->world * source->positions[source->editor_to_source[i]].cast<double>().homogeneous()).head<3>();
        const double tolerance = std::max(1e-5, world.cwiseAbs().maxCoeff() * 2e-6);
        require(editor_mesh.vertices[i].allFinite() && (world - editor_mesh.vertices[i].cast<double>()).norm() <= tolerance,
            "GLB vertex mapping could not be verified; no texture-preserving edit was written.");
    }
    int winding = 0;
    for (size_t i = 0; i < editor_mesh.indices.size(); ++i) {
        if ((i & 4095) == 0 && checkpoint) checkpoint();
        const auto& actual = editor_mesh.indices[i];
        const size_t a = source_to_editor[source->indices[i * 3]], b = source_to_editor[source->indices[i * 3 + 1]], c = source_to_editor[source->indices[i * 3 + 2]];
        int direction = 0;
        const std::array<size_t, 3> expected {a, b, c};
        for (size_t start = 0; start < 3; ++start) {
            if (actual[0] == int(expected[start]) && actual[1] == int(expected[(start + 1) % 3]) && actual[2] == int(expected[(start + 2) % 3])) direction = 1;
            if (actual[0] == int(expected[start]) && actual[1] == int(expected[(start + 2) % 3]) && actual[2] == int(expected[(start + 1) % 3])) direction = -1;
        }
        require(direction != 0 && (!winding || winding == direction), "GLB triangle order or topology does not match the editor; no edit was written.");
        winding = direction;
    }
    source->editor_mesh = editor_mesh;
    source->sha256 = bytes_sha256(source->original);
    require(model_artifact_sha256(path) == source->sha256,
        "The source GLB changed while its geometry was being read; reload it before editing.");
    if (checkpoint) checkpoint();
    return source;
}

void write_glb_geometry_edit(const GlbGeometrySource& source, const boost::filesystem::path& destination,
    const indexed_triangle_set& edited_mesh, const std::vector<size_t>& selected_faces, const std::function<void()>& checkpoint)
{
    if (checkpoint) checkpoint();
    require(edited_mesh.vertices.size() == source.editor_mesh.vertices.size() && edited_mesh.indices == source.editor_mesh.indices,
        "Texture-preserving GLB edits cannot change vertex or triangle topology.");
    require(model_artifact_format(destination) == "glb", "Save geometry edits as GLB to retain its original textures and materials.");
    auto temporary = destination; temporary += ".partial";
    require(!boost::filesystem::exists(destination) && !boost::filesystem::exists(temporary), "The output already exists; choose a new GLB version.");
    std::vector<bool> selected(source.indices.size() / 3, selected_faces.empty()), outside(source.positions.size(), false);
    for (size_t face : selected_faces) { require(face < selected.size(), "GLB selected face is out of range."); selected[face] = true; }
    for (size_t i = 0; i < selected.size(); ++i) if (!selected[i])
        for (size_t corner = 0; corner < 3; ++corner) outside[source.indices[i * 3 + corner]] = true;
    std::vector<Vec3f> positions = source.positions;
    std::vector<bool> changed(positions.size(), false);
    const Eigen::Matrix4d inverse = source.world.inverse();
    bool moved = false;
    for (size_t i = 0; i < edited_mesh.vertices.size(); ++i) {
        if ((i & 4095) == 0 && checkpoint) checkpoint();
        const auto& edited = edited_mesh.vertices[i];
        require(edited.allFinite(), "The edited model contains a non-finite position.");
        if (edited == source.editor_mesh.vertices[i]) continue;
        const size_t raw = source.editor_to_source[i];
        require(!outside[raw], "A GLB geometry edit changed a vertex outside the selected patch.");
        // Transform the displacement so unchanged source coordinates retain
        // their original precision despite the editor's world-space floats.
        const Vec3d delta = edited.cast<double>() - source.editor_mesh.vertices[i].cast<double>();
        positions[raw] = (source.positions[raw].cast<double>() + inverse.block<3, 3>(0, 0) * delta).cast<float>();
        changed[raw] = moved = true;
    }
    Json doc = source.doc;
    std::vector<unsigned char> binary = source.binary;
    if (moved) {
        append_vectors(doc, binary, source.position_accessor, positions, "POSITION", checkpoint);
        if (!source.normals.empty()) {
            std::vector<Vec3d> summed(positions.size(), Vec3d::Zero());
            std::vector<bool> affected(positions.size(), false);
            for (size_t face = 0; face < selected.size(); ++face) {
                if ((face & 4095) == 0 && checkpoint) checkpoint();
                const size_t a = source.indices[face * 3], b = source.indices[face * 3 + 1], c = source.indices[face * 3 + 2];
                const Vec3d normal = (positions[b].cast<double>() - positions[a].cast<double>()).cross(positions[c].cast<double>() - positions[a].cast<double>());
                for (size_t vertex : {a, b, c}) {
                    summed[vertex] += normal;
                    affected[vertex] = affected[vertex] || changed[a] || changed[b] || changed[c];
                }
            }
            auto normals = source.normals;
            for (size_t i = 0; i < normals.size(); ++i) if (affected[i] && !outside[i]) {
                require(summed[i].allFinite() && summed[i].squaredNorm() > 0, "Edited GLB normal is degenerate; the original is preserved.");
                normals[i] = summed[i].normalized().cast<float>();
            }
            append_vectors(doc, binary, source.normal_accessor, normals, "NORMAL", checkpoint);
        }
        doc["buffers"][0]["byteLength"] = binary.size();
    }
    std::vector<unsigned char> encoded;
    if (!moved) encoded = source.original;
    else {
        std::string json = doc.dump();
        while (json.size() % 4) json += ' ';
        while (binary.size() % 4) binary.push_back(0);
        require(json.size() <= 32 * 1024 * 1024 && json.size() + binary.size() + 28 <= max_bytes, "The edited GLB exceeds 512 MB.");
        append_u32(encoded, 0x46546c67); append_u32(encoded, 2); append_u32(encoded, uint32_t(json.size() + binary.size() + 28));
        append_u32(encoded, uint32_t(json.size())); append_u32(encoded, 0x4e4f534a);
        encoded.insert(encoded.end(), json.begin(), json.end());
        append_u32(encoded, uint32_t(binary.size())); append_u32(encoded, 0x004e4942);
        encoded.insert(encoded.end(), binary.begin(), binary.end());
    }
    bool owns_temporary = false, owns_destination = false;
    try {
        if (checkpoint) checkpoint();
        boost::filesystem::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        require(bool(output), "Unable to create the edited GLB version.");
        owns_temporary = true;
        for (size_t offset = 0; offset < encoded.size(); offset += 65536) {
            if (checkpoint) checkpoint();
            const size_t count = std::min(size_t(65536), encoded.size() - offset);
            output.write(reinterpret_cast<const char*>(encoded.data() + offset), count);
        }
        output.close();
        require(bool(output) && boost::filesystem::file_size(temporary) == encoded.size(), "The edited GLB was not written completely.");
        if (checkpoint) checkpoint();
        require(model_artifact_sha256(source.path) == source.sha256, "The source GLB changed during editing; reload it before saving.");
        if (checkpoint) checkpoint();
        require(!boost::filesystem::exists(destination), "The destination GLB already exists; choose a new version.");
        if (checkpoint) checkpoint();
        // Unlike POSIX rename, hard-link publication never replaces a version
        // created concurrently after the existence check. Both paths are in
        // the same directory and the fully written file is visible atomically.
        boost::system::error_code error;
        boost::filesystem::create_hard_link(temporary, destination, error);
        if (error) throw std::runtime_error("Unable to publish the new GLB without overwriting another version. Choose a new filename on a filesystem that supports hard links: " + error.message());
        owns_destination = true;
        boost::filesystem::remove(temporary, error);
        if (error) throw std::runtime_error("The completed GLB staging file could not be removed: " + error.message());
        owns_temporary = false;
    } catch (...) {
        if (owns_destination) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
        if (owns_temporary) { boost::system::error_code ignored; boost::filesystem::remove(temporary, ignored); }
        throw;
    }
}
} // namespace Slic3r::AI
