#include "ModelArtifact.hpp"

#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/TexturePainting.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include "libslic3r/TriangleMesh.hpp"
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <stdexcept>
#include <openssl/evp.h>
#include <memory>
#include <sstream>

namespace Slic3r::AI {
namespace {
constexpr uint64_t max_bytes = 512ull * 1024 * 1024;
float linear(float v) { return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f); }
float srgb(float v) {
    v = std::clamp(v, 0.f, 1.f);
    return v <= .0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - .055f;
}
uint32_t read_u32(const unsigned char* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void append_u32(std::vector<unsigned char>& data, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) data.push_back(static_cast<unsigned char>(value >> shift));
}
void append_float(std::vector<unsigned char>& data, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(data, bits);
}
void write_u32(std::ostream& output, uint32_t value) {
    std::array<unsigned char, 4> bytes;
    for (size_t i = 0; i < 4; ++i) bytes[i] = static_cast<unsigned char>(value >> (i * 8));
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
nlohmann::json glb_description(const boost::filesystem::path& path) {
    boost::filesystem::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 20> header {};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    const uint32_t json_size = read_u32(header.data() + 12);
    if (!input || read_u32(header.data()) != 0x46546c67 || read_u32(header.data() + 4) != 2 ||
        read_u32(header.data() + 8) != boost::filesystem::file_size(path) ||
        read_u32(header.data() + 16) != 0x4e4f534a || !json_size || json_size % 4 ||
        json_size > 32u * 1024 * 1024 || json_size > boost::filesystem::file_size(path) - 20)
        throw std::runtime_error("Invalid or incomplete GLB 2.0 file.");
    std::string encoded(json_size, '\0');
    input.read(encoded.data(), encoded.size());
    if (!input) throw std::runtime_error("GLB description is incomplete.");
    auto doc = nlohmann::json::parse(encoded);
    if (doc.at("asset").value("version", "") != "2.0") throw std::runtime_error("Only GLB 2.0 is supported.");
    for (const auto& extension : doc.value("extensionsRequired", nlohmann::json::array()))
        if (extension != "KHR_materials_unlit" && extension != "KHR_texture_transform" && extension != "KHR_mesh_quantization")
            throw std::runtime_error("Unsupported GLB extension: " + extension.get<std::string>());
    for (const auto& node : doc.value("nodes", nlohmann::json::array()))
        if (node.contains("skin") || node.contains("weights")) throw std::runtime_error("Bake animated GLB geometry before editing.");
    for (const auto& mesh : doc.at("meshes")) for (const auto& primitive : mesh.at("primitives"))
        if (primitive.value("mode", 4) != 4 || primitive.contains("targets"))
            throw std::runtime_error("GLB must contain static triangle meshes.");
    for (const auto& material : doc.value("materials", nlohmann::json::array())) {
        const auto pbr = material.value("pbrMetallicRoughness", nlohmann::json::object());
        if (pbr.contains("baseColorTexture")) {
            const auto& ti = pbr["baseColorTexture"];
            const auto transform = ti.value("extensions", nlohmann::json::object()).value("KHR_texture_transform", nlohmann::json::object());
            if (transform.value("texCoord", ti.value("texCoord", 0)) != 0)
                throw std::runtime_error("GLB color textures must use TEXCOORD_0 for local editing.");
        }
    }
    for (const auto& buffer : doc.at("buffers"))
        if (buffer.contains("uri")) throw std::runtime_error("GLB geometry must be embedded in the file.");
    for (const auto& image : doc.value("images", nlohmann::json::array()))
        if (image.contains("uri") && image.at("uri").get<std::string>().rfind("data:", 0) != 0)
            throw std::runtime_error("GLB images must be embedded in the file.");
    return doc;
}
float wrap(float value, int mode) {
    if (mode == 33071) return std::clamp(value, 0.f, 1.f);
    if (mode == 33648) { value -= 2.f * std::floor(value / 2.f); return 1.f - std::abs(value - 1.f); }
    if (mode == 10497) return value - std::floor(value);
    throw std::runtime_error("Unsupported GLB texture wrapping mode.");
}
} // namespace

std::string model_artifact_format(const boost::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return extension == ".glb" ? "glb" : extension == ".obj" ? "obj" : "";
}

std::string model_artifact_sha256(const boost::filesystem::path& path) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    boost::filesystem::ifstream input(path, std::ios::binary);
    if (!input || !digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) return {};
    std::array<char, 65536> buffer;
    while (input) {
        input.read(buffer.data(), buffer.size());
        if (input.gcount() && EVP_DigestUpdate(digest.get(), buffer.data(), size_t(input.gcount())) != 1) return {};
    }
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned length = 0;
    if (!input.eof() || EVP_DigestFinal_ex(digest.get(), bytes, &length) != 1) return {};
    std::ostringstream hex;
    for (unsigned i = 0; i < length; ++i) hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
    return hex.str();
}

bool is_model_artifact(const boost::filesystem::path& path) {
    boost::system::error_code ec;
    if (model_artifact_format(path).empty() || !boost::filesystem::is_regular_file(path, ec) || ec) return false;
    const auto size = boost::filesystem::file_size(path, ec);
    return !ec && size > 0 && size <= max_bytes;
}

bool load_model_artifact(const boost::filesystem::path& path, TriangleMesh& mesh, ObjInfo& info, std::string& error) {
    error.clear();
    info = ObjInfo {};
    try {
        if (!is_model_artifact(path)) throw std::runtime_error("The OBJ/GLB model is missing or exceeds 512 MB.");
        if (model_artifact_format(path) == "obj") return load_obj(path.string().c_str(), &mesh, info, error);
        const auto doc = glb_description(path);
        TexturedMesh textured;
        std::vector<std::array<float, 4>> vertex_colors;
        if (!load_assimp_textured_model(path.string(), textured, &error, &vertex_colors)) return false;
        if (textured.vertices.empty() || textured.vertices.size() > 6000000 || textured.indices.size() > 2000000)
            throw std::runtime_error("GLB model exceeds the editable mesh limit.");
        indexed_triangle_set its;
        its.vertices.reserve(textured.vertices.size());
        its.indices.reserve(textured.indices.size());
        for (const auto& v : textured.vertices) {
            if (!std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); }))
                throw std::runtime_error("GLB contains non-finite geometry.");
            its.vertices.emplace_back(v[0] * 1000.f, -v[2] * 1000.f, v[1] * 1000.f);
        }
        for (const auto& f : textured.indices) {
            for (int v : f) if (v < 0 || size_t(v) >= its.vertices.size()) throw std::runtime_error("GLB triangle index is invalid.");
            its.indices.emplace_back(f[0], f[1], f[2]);
        }
        struct Pixels { std::vector<unsigned char> data; int width {0}, height {0}; };
        std::vector<Pixels> images(textured.textures.size());
        for (size_t i = 0; i < images.size(); ++i)
            if (!decode_texture_to_pixels(textured.textures[i], images[i].data, images[i].width, images[i].height) ||
                uint64_t(images[i].width) * images[i].height > 64ull * 1024 * 1024)
                throw std::runtime_error("GLB color texture cannot be decoded.");
        info = ObjInfo {};
        info.vertex_colors.assign(its.vertices.size(), RGBA {1.f, 1.f, 1.f, 1.f});
        std::vector<unsigned char> assigned(its.vertices.size(), 0);
        for (size_t fi = 0; fi < textured.indices.size(); ++fi) {
            const int material = fi < textured.material_ids.size() ? textured.material_ids[fi] : -1;
            const int texture = material >= 0 && size_t(material) < textured.material_texture_map.size()
                ? textured.material_texture_map[material] : -1;
            const auto factor = material >= 0 && size_t(material) < textured.material_colors.size()
                ? textured.material_colors[material] : std::array<float, 4>{1.f, 1.f, 1.f, 1.f};
            int wrap_s = 10497, wrap_t = 10497;
            nlohmann::json transform = nlohmann::json::object();
            if (material >= 0 && doc.contains("materials") && size_t(material) < doc["materials"].size()) {
                const auto& pbr = doc["materials"][material].value("pbrMetallicRoughness", nlohmann::json::object());
                if (pbr.contains("baseColorTexture")) {
                    if (texture < 0) throw std::runtime_error("The GLB color texture is missing or cannot be read.");
                    const auto& ti = pbr["baseColorTexture"];
                    transform = ti.value("extensions", nlohmann::json::object()).value("KHR_texture_transform", nlohmann::json::object());
                    const auto& td = doc.at("textures").at(ti.at("index").get<size_t>());
                    if (td.contains("sampler")) {
                        const auto& sampler = doc.at("samplers").at(td.at("sampler").get<size_t>());
                        wrap_s = sampler.value("wrapS", 10497); wrap_t = sampler.value("wrapT", 10497);
                    }
                }
            }
            for (int vi : textured.indices[fi]) {
                if (assigned[vi]) continue;
                assigned[vi] = 1;
                RGBA color {1.f, 1.f, 1.f, 1.f};
                if (texture >= 0) {
                    if (size_t(texture) >= images.size() || size_t(vi) >= textured.uvs.size())
                        throw std::runtime_error("GLB texture coordinates are missing.");
                    const auto& image = images[texture];
                    auto uv = textured.uvs[vi];
                    const auto scale = transform.value("scale", std::array<float, 2>{1.f, 1.f});
                    const auto offset = transform.value("offset", std::array<float, 2>{0.f, 0.f});
                    const float angle = transform.value("rotation", 0.f);
                    const float u = uv[0] * scale[0], v = uv[1] * scale[1];
                    uv = {offset[0] + std::cos(angle) * u - std::sin(angle) * v,
                          offset[1] + std::sin(angle) * u + std::cos(angle) * v};
                    if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) throw std::runtime_error("GLB UV is invalid.");
                    const int x = std::min(image.width - 1, int(wrap(uv[0], wrap_s) * image.width));
                    const int y = std::min(image.height - 1, int(wrap(uv[1], wrap_t) * image.height));
                    const auto* pixel = image.data.data() + (size_t(y) * image.width + x) * 3;
                    for (size_t c = 0; c < 3; ++c) color[c] = linear(pixel[2 - c] / 255.f);
                }
                for (size_t c = 0; c < 3; ++c) {
                    const float vertex = vertex_colors.size() == its.vertices.size() ? vertex_colors[vi][c] : 1.f;
                    if (!std::isfinite(vertex) || !std::isfinite(factor[c])) throw std::runtime_error("GLB color is invalid.");
                    color[c] = srgb(color[c] * factor[c] * vertex);
                }
                info.vertex_colors[vi] = color;
            }
        }
        mesh = TriangleMesh(std::move(its));
        if (mesh.volume() < 0) mesh.flip_triangles();
        return !mesh.empty();
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool write_model_artifact(const boost::filesystem::path& path, const indexed_triangle_set& mesh,
                          const std::vector<RGBA>& colors, std::string& error) {
    boost::filesystem::path temporary = path; temporary += ".partial";
    bool owned = false;
    try {
        if (mesh.vertices.empty() || mesh.indices.empty() || colors.size() != mesh.vertices.size())
            throw std::runtime_error("The edited model has incomplete geometry or colors.");
        if (boost::filesystem::exists(path) || boost::filesystem::exists(temporary))
            throw std::runtime_error("The output already exists; choose a new model version.");
        if (model_artifact_format(path).empty()) throw std::runtime_error("Choose OBJ or GLB for the model.");
        if (!path.parent_path().empty()) boost::filesystem::create_directories(path.parent_path());
        for (const auto& v : mesh.vertices) for (int c = 0; c < 3; ++c)
            if (!std::isfinite(v[c])) throw std::runtime_error("The edited model contains invalid vertices.");
        for (const auto& color : colors) for (float c : color)
            if (!std::isfinite(c) || c < 0.f || c > 1.f) throw std::runtime_error("The edited model contains invalid colors.");
        for (const auto& f : mesh.indices) for (int c = 0; c < 3; ++c)
            if (f[c] < 0 || size_t(f[c]) >= mesh.vertices.size()) throw std::runtime_error("The edited mesh has invalid indices.");
        boost::filesystem::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Unable to create the model file.");
        owned = true;
        output.imbue(std::locale::classic());
        if (model_artifact_format(path) == "obj") {
            output << "# Orca AI model: Z-up millimetres, sRGB vertex colors\n" << std::setprecision(9);
            for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                const auto& v = mesh.vertices[i]; const auto& c = colors[i];
                output << "v " << v.x() << ' ' << v.y() << ' ' << v.z() << ' ' << c[0] << ' ' << c[1] << ' ' << c[2] << '\n';
            }
            for (const auto& f : mesh.indices) output << "f " << f[0] + 1 << ' ' << f[1] + 1 << ' ' << f[2] + 1 << '\n';
        } else {
            std::vector<unsigned char> binary;
            std::array<float, 3> minimum {INFINITY, INFINITY, INFINITY}, maximum {-INFINITY, -INFINITY, -INFINITY};
            binary.reserve(mesh.vertices.size() * 24 + mesh.indices.size() * 12);
            for (const auto& v : mesh.vertices) {
                const std::array<float, 3> p {v.x() * .001f, v.z() * .001f, -v.y() * .001f};
                for (size_t c = 0; c < 3; ++c) { append_float(binary, p[c]); minimum[c] = std::min(minimum[c], p[c]); maximum[c] = std::max(maximum[c], p[c]); }
            }
            const size_t colors_offset = binary.size();
            for (const auto& color : colors) for (size_t c = 0; c < 3; ++c) append_float(binary, linear(color[c]));
            const size_t indices_offset = binary.size();
            for (const auto& face : mesh.indices) for (int c = 0; c < 3; ++c) append_u32(binary, uint32_t(face[c]));
            using nlohmann::json;
            json doc = {
                {"asset", {{"version", "2.0"}, {"generator", "Orca AI local model editor"}}},
                {"scene", 0}, {"scenes", {{{"nodes", {0}}}}}, {"nodes", {{{"mesh", 0}}}},
                {"buffers", {{{"byteLength", binary.size()}}}},
                {"bufferViews", {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", colors_offset}},
                                 {{"buffer", 0}, {"byteOffset", colors_offset}, {"byteLength", indices_offset - colors_offset}},
                                 {{"buffer", 0}, {"byteOffset", indices_offset}, {"byteLength", binary.size() - indices_offset}}}},
                {"accessors", {{{"bufferView", 0}, {"componentType", 5126}, {"count", mesh.vertices.size()}, {"type", "VEC3"}, {"min", minimum}, {"max", maximum}},
                               {{"bufferView", 1}, {"componentType", 5126}, {"count", colors.size()}, {"type", "VEC3"}},
                               {{"bufferView", 2}, {"componentType", 5125}, {"count", mesh.indices.size() * 3}, {"type", "SCALAR"}}}},
                {"meshes", {{{"primitives", {{{"attributes", {{"POSITION", 0}, {"COLOR_0", 1}}}, {"indices", 2}, {"mode", 4}, {"material", 0}}}}}}},
                {"materials", {{{"doubleSided", true}, {"pbrMetallicRoughness", {{"baseColorFactor", {1., 1., 1., 1.}}, {"metallicFactor", 0.}, {"roughnessFactor", 1.}}}}}}
            };
            std::string encoded = doc.dump();
            while (encoded.size() % 4) encoded += ' ';
            const size_t size = 28 + encoded.size() + binary.size();
            if (size > max_bytes) throw std::runtime_error("The edited GLB exceeds 512 MB.");
            write_u32(output, 0x46546c67); write_u32(output, 2); write_u32(output, uint32_t(size));
            write_u32(output, uint32_t(encoded.size())); write_u32(output, 0x4e4f534a); output.write(encoded.data(), encoded.size());
            write_u32(output, uint32_t(binary.size())); write_u32(output, 0x004e4942); output.write(reinterpret_cast<const char*>(binary.data()), binary.size());
        }
        output.close();
        if (!output || boost::filesystem::file_size(temporary) > max_bytes) throw std::runtime_error("Unable to write the complete model.");
        boost::filesystem::rename(temporary, path);
        return true;
    } catch (const std::exception& e) {
        if (owned) { boost::system::error_code ignored; boost::filesystem::remove(temporary, ignored); }
        error = e.what(); return false;
    }
}
} // namespace Slic3r::AI
