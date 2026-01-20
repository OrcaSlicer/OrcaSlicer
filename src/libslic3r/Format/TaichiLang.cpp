#include "TaichiLang.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace Slic3r {

enum class TaichiApplyMode
{
    ApplyToExisting,
    AppendObjects
};

static std::string escape_quotes(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"')
            out += "\\\"";
        else
            out += c;
    }
    return out;
}

static bool parse_double(const std::string& token, double& out)
{
    // Allow both std::from_chars (C++17) fallback to stringstream.
    // from_chars for double is not universally available on all MSVC/STL combos used in older toolchains.
    std::stringstream ss(token);
    ss.imbue(std::locale::classic());
    ss >> out;
    return ss && ss.eof();
}

static std::string trim(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b]))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace((unsigned char)s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

static std::vector<std::string> tokenize(const std::string& line)
{
    std::vector<std::string> tokens;
    std::string cur;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (!in_quotes && c == '#')
            break;

        if (c == '"') {
            in_quotes = !in_quotes;
            cur += c;
            continue;
        }

        if (!in_quotes && std::isspace((unsigned char)c)) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            continue;
        }

        cur += c;
    }

    if (!cur.empty())
        tokens.push_back(cur);

    return tokens;
}

static bool parse_quoted_string(const std::string& token, std::string& out)
{
    if (token.size() < 2 || token.front() != '"' || token.back() != '"')
        return false;
    out = token.substr(1, token.size() - 2);
    // Unescape minimal: \" -> "
    std::string unescaped;
    unescaped.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\\' && (i + 1) < out.size() && out[i + 1] == '"') {
            unescaped += '"';
            ++i;
        } else {
            unescaped += out[i];
        }
    }
    out = unescaped;
    return true;
}

static bool parse_vec3(const std::string& token, Vec3d& out)
{
    // Expect [x,y,z]
    std::string t = trim(token);
    if (t.size() < 5 || t.front() != '[' || t.back() != ']')
        return false;
    t = t.substr(1, t.size() - 2);

    double vals[3] = {0, 0, 0};
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        size_t comma = (i < 2) ? t.find(',', start) : std::string::npos;
        std::string part = (comma == std::string::npos) ? t.substr(start) : t.substr(start, comma - start);
        part = trim(part);
        if (!parse_double(part, vals[i]))
            return false;
        if (comma == std::string::npos)
            start = t.size();
        else
            start = comma + 1;
    }

    out = Vec3d(vals[0], vals[1], vals[2]);
    return true;
}

static bool parse_vec3i32(const std::string& token, Vec3i32& out)
{
    std::string t = trim(token);
    if (t.size() < 5 || t.front() != '[' || t.back() != ']')
        return false;
    t = t.substr(1, t.size() - 2);

    int vals[3] = {0, 0, 0};
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        size_t comma = (i < 2) ? t.find(',', start) : std::string::npos;
        std::string part = (comma == std::string::npos) ? t.substr(start) : t.substr(start, comma - start);
        part = trim(part);

        std::stringstream ss(part);
        ss.imbue(std::locale::classic());
        ss >> vals[i];
        if (!ss || !ss.eof())
            return false;

        if (comma == std::string::npos)
            start = t.size();
        else
            start = comma + 1;
    }

    out = Vec3i32(vals[0], vals[1], vals[2]);
    return true;
}

std::string model_to_taichi_lang(const Model& model)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss.setf(std::ios::fixed);
    ss.precision(6);

    ss << "# orcaslicer_taichi v1\n";
    ss << "# Note: this is a round-trip format. Large meshes will produce large text.\n";

    size_t nonnull_objects = 0;
    for (const ModelObject* obj : model.objects)
        if (obj != nullptr)
            ++nonnull_objects;

    ss << "model_objects " << nonnull_objects << "\n\n";

    for (size_t obj_idx = 0; obj_idx < model.objects.size(); ++obj_idx) {
        const ModelObject* obj = model.objects[obj_idx];
        if (obj == nullptr)
            continue;

        const BoundingBoxf3 bbox = obj->bounding_box_exact();

        ss << "object " << obj_idx << " \"" << escape_quotes(obj->name) << "\" {\n";
        ss << "  bbox_min [" << bbox.min(0) << "," << bbox.min(1) << "," << bbox.min(2) << "]\n";
        ss << "  bbox_max [" << bbox.max(0) << "," << bbox.max(1) << "," << bbox.max(2) << "]\n";
        ss << "  instances " << obj->instances.size() << "\n";

        for (size_t inst_idx = 0; inst_idx < obj->instances.size(); ++inst_idx) {
            const ModelInstance* inst = obj->instances[inst_idx];
            if (inst == nullptr)
                continue;

            const Vec3d off = inst->get_offset();
            const Vec3d rot = inst->get_rotation();
            const Vec3d scl = inst->get_scaling_factor();
            const Vec3d mir = inst->get_mirror();

            ss << "  instance " << inst_idx;
            ss << " offset [" << off.x() << "," << off.y() << "," << off.z() << "]";
            ss << " rotation [" << rot.x() << "," << rot.y() << "," << rot.z() << "]";
            ss << " scale [" << scl.x() << "," << scl.y() << "," << scl.z() << "]";
            ss << " mirror [" << mir.x() << "," << mir.y() << "," << mir.z() << "]";
            ss << "\n";
        }

        // Export raw mesh (sum of model volumes, without instance transform).
        // This makes Sync From Orca much more faithful for testing.
        {
            const TriangleMesh raw = obj->raw_mesh();
            if (!raw.its.vertices.empty() && !raw.its.indices.empty()) {
                ss << "  mesh_begin\n";
                ss << "  mesh_vertices " << raw.its.vertices.size() << "\n";
                for (const Vec3f& v : raw.its.vertices)
                    ss << "  v [" << v.x() << "," << v.y() << "," << v.z() << "]\n";
                ss << "  mesh_faces " << raw.its.indices.size() << "\n";
                for (const Vec3i32& f : raw.its.indices)
                    ss << "  f [" << f.x() << "," << f.y() << "," << f.z() << "]\n";
                ss << "  mesh_end\n";
            }
        }

        ss << "}\n\n";
    }

    return ss.str();
}

static bool apply_taichi_lang_to_model_impl(
    Model& model,
    const std::string& text,
    TaichiApplyMode mode,
    std::vector<size_t>& affected_object_indices,
    std::string& error)
{
    affected_object_indices.clear();
    error.clear();

    auto mark_changed = [&](size_t obj_idx) {
        if (std::find(affected_object_indices.begin(), affected_object_indices.end(), obj_idx) == affected_object_indices.end())
            affected_object_indices.push_back(obj_idx);
    };

    std::istringstream input(text);
    input.imbue(std::locale::classic());

    std::string line;
    size_t line_no = 0;
    // Object index as written in the Taichi text.
    std::optional<size_t> current_object_src;

    bool in_mesh = false;
    std::vector<Vec3f> mesh_vertices;
    std::vector<Vec3i32> mesh_faces;

    std::unordered_map<size_t, size_t> append_obj_map;
    append_obj_map.reserve(64);

    auto ensure_object = [&](size_t obj_idx) -> ModelObject* {
        if (mode == TaichiApplyMode::AppendObjects) {
            auto it = append_obj_map.find(obj_idx);
            if (it != append_obj_map.end())
                return model.objects[it->second];
            ModelObject* created = model.add_object();
            const size_t new_idx = model.objects.size() - 1;
            append_obj_map.emplace(obj_idx, new_idx);
            mark_changed(new_idx);
            return created;
        }

        while (model.objects.size() <= obj_idx)
            model.add_object();
        return model.objects[obj_idx];
    };

    auto ensure_instance = [&](ModelObject* obj, size_t inst_idx) -> ModelInstance* {
        while (obj->instances.size() <= inst_idx)
            obj->add_instance();
        return obj->instances[inst_idx];
    };

    while (std::getline(input, line)) {
        ++line_no;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#')
            continue;

        auto tokens = tokenize(t);
        if (tokens.empty())
            continue;

        if (tokens[0] == "object") {
            if (in_mesh) {
                error = "Parse error on line " + std::to_string(line_no) + ": unterminated mesh (missing mesh_end)";
                return false;
            }
            if (tokens.size() < 3) {
                error = "Parse error on line " + std::to_string(line_no) + ": expected: object <index> \"name\"";
                return false;
            }

            size_t obj_idx = 0;
            {
                std::stringstream ss(tokens[1]);
                ss >> obj_idx;
                if (!ss || !ss.eof()) {
                    error = "Parse error on line " + std::to_string(line_no) + ": invalid object index";
                    return false;
                }
            }

            ModelObject* obj = ensure_object(obj_idx);
            if (obj == nullptr) {
                error = "Parse error on line " + std::to_string(line_no) + ": object index invalid";
                return false;
            }

            std::string name;
            if (!parse_quoted_string(tokens[2], name)) {
                error = "Parse error on line " + std::to_string(line_no) + ": invalid quoted object name";
                return false;
            }

            if (obj->name != name) {
                obj->name = name;
                const size_t model_obj_idx = (mode == TaichiApplyMode::AppendObjects) ? append_obj_map.at(obj_idx) : obj_idx;
                mark_changed(model_obj_idx);
            }

            if (obj->instances.empty())
                obj->add_instance();

            current_object_src = obj_idx;
            continue;
        }

        if (tokens[0] == "}") {
            if (in_mesh) {
                error = "Parse error on line " + std::to_string(line_no) + ": unterminated mesh (missing mesh_end)";
                return false;
            }
            current_object_src.reset();
            continue;
        }

        if (tokens[0] == "mesh_begin") {
            if (!current_object_src.has_value()) {
                error = "Parse error on line " + std::to_string(line_no) + ": mesh_begin must be inside an object block";
                return false;
            }
            in_mesh = true;
            mesh_vertices.clear();
            mesh_faces.clear();
            continue;
        }

        if (tokens[0] == "mesh_end") {
            if (!current_object_src.has_value() || !in_mesh) {
                error = "Parse error on line " + std::to_string(line_no) + ": mesh_end without mesh_begin";
                return false;
            }

            const size_t src_obj_idx = *current_object_src;
            const size_t model_obj_idx = (mode == TaichiApplyMode::AppendObjects) ? append_obj_map.at(src_obj_idx) : src_obj_idx;
            ModelObject* obj = (mode == TaichiApplyMode::AppendObjects) ? model.objects[model_obj_idx] : ensure_object(model_obj_idx);
            if (obj == nullptr) {
                error = "Parse error on line " + std::to_string(line_no) + ": invalid current object";
                return false;
            }

            if (mesh_vertices.empty() || mesh_faces.empty()) {
                error = "Parse error on line " + std::to_string(line_no) + ": mesh has no vertices or faces";
                return false;
            }

            for (const Vec3i32& f : mesh_faces) {
                if (f.x() < 0 || f.y() < 0 || f.z() < 0 ||
                    (size_t)f.x() >= mesh_vertices.size() || (size_t)f.y() >= mesh_vertices.size() || (size_t)f.z() >= mesh_vertices.size()) {
                    error = "Parse error on line " + std::to_string(line_no) + ": mesh face index out of range";
                    return false;
                }
            }

            TriangleMesh mesh(std::move(mesh_vertices), std::move(mesh_faces));
            obj->clear_volumes();
            ModelVolume* vol = obj->add_volume(std::move(mesh));
            if (vol != nullptr)
                vol->name = obj->name;
            if (!obj->config.has("extruder") || obj->config.extruder() == 0)
                obj->config.set_key_value("extruder", new ConfigOptionInt(1));
            obj->invalidate_bounding_box();
            mark_changed(model_obj_idx);

            in_mesh = false;
            mesh_vertices.clear();
            mesh_faces.clear();
            continue;
        }

        if (in_mesh && tokens[0] == "v") {
            if (tokens.size() < 2) {
                error = "Parse error on line " + std::to_string(line_no) + ": expected: v [x,y,z]";
                return false;
            }
            Vec3d v;
            if (!parse_vec3(tokens[1], v)) {
                error = "Parse error on line " + std::to_string(line_no) + ": invalid mesh vertex vec3";
                return false;
            }
            mesh_vertices.emplace_back((float)v.x(), (float)v.y(), (float)v.z());
            continue;
        }

        if (in_mesh && tokens[0] == "f") {
            if (tokens.size() < 2) {
                error = "Parse error on line " + std::to_string(line_no) + ": expected: f [i,j,k]";
                return false;
            }
            Vec3i32 f;
            if (!parse_vec3i32(tokens[1], f)) {
                error = "Parse error on line " + std::to_string(line_no) + ": invalid mesh face vec3i";
                return false;
            }
            mesh_faces.emplace_back(f);
            continue;
        }

        if (tokens[0] == "instance") {
            if (in_mesh) {
                error = "Parse error on line " + std::to_string(line_no) + ": instance not allowed inside mesh block";
                return false;
            }
            if (!current_object_src.has_value()) {
                error = "Parse error on line " + std::to_string(line_no) + ": instance must be inside an object block";
                return false;
            }

            if (tokens.size() < 10) {
                error = "Parse error on line " + std::to_string(line_no) + ": expected: instance <idx> offset [x,y,z] rotation [x,y,z] scale [x,y,z] mirror [x,y,z]";
                return false;
            }

            size_t inst_idx = 0;
            {
                std::stringstream ss(tokens[1]);
                ss >> inst_idx;
                if (!ss || !ss.eof()) {
                    error = "Parse error on line " + std::to_string(line_no) + ": invalid instance index";
                    return false;
                }
            }

            const size_t src_obj_idx = *current_object_src;
            const size_t model_obj_idx = (mode == TaichiApplyMode::AppendObjects) ? append_obj_map.at(src_obj_idx) : src_obj_idx;
            ModelObject* obj = (mode == TaichiApplyMode::AppendObjects) ? model.objects[model_obj_idx] : ensure_object(model_obj_idx);
            if (obj == nullptr) {
                error = "Parse error on line " + std::to_string(line_no) + ": invalid current object";
                return false;
            }
            ModelInstance* inst = ensure_instance(obj, inst_idx);
            if (inst == nullptr) {
                error = "Parse error on line " + std::to_string(line_no) + ": failed to create instance";
                return false;
            }

            auto expect_key = [&](size_t idx, const char* key) -> bool {
                return idx < tokens.size() && tokens[idx] == key;
            };

            if (!expect_key(2, "offset") || !expect_key(4, "rotation") || !expect_key(6, "scale") || !expect_key(8, "mirror")) {
                error = "Parse error on line " + std::to_string(line_no) + ": expected keys offset/rotation/scale/mirror";
                return false;
            }

            Vec3d offset, rotation, scale, mirror;
            if (!parse_vec3(tokens[3], offset)) {
                error = "Parse error on line " + std::to_string(line_no) + ": invalid offset vec3";
                return false;
            }
            if (!parse_vec3(tokens[5], rotation)) {
                error = "Parse error on line " + std::to_string(line_no) + ": invalid rotation vec3";
                return false;
            }
            if (!parse_vec3(tokens[7], scale)) {
                error = "Parse error on line " + std::to_string(line_no) + ": invalid scale vec3";
                return false;
            }
            if (!parse_vec3(tokens[9], mirror)) {
                error = "Parse error on line " + std::to_string(line_no) + ": invalid mirror vec3";
                return false;
            }

            bool changed = false;

            if (inst->get_offset() != offset) {
                inst->set_offset(offset);
                changed = true;
            }
            if (inst->get_rotation() != rotation) {
                inst->set_rotation(rotation);
                changed = true;
            }
            if (inst->get_scaling_factor() != scale) {
                inst->set_scaling_factor(scale);
                changed = true;
            }
            if (inst->get_mirror() != mirror) {
                inst->set_mirror(mirror);
                changed = true;
            }

            if (changed)
                mark_changed(model_obj_idx);

            continue;
        }

        if (tokens[0] == "geometry") {
            if (in_mesh) {
                error = "Parse error on line " + std::to_string(line_no) + ": geometry not allowed inside mesh block";
                return false;
            }
            if (!current_object_src.has_value()) {
                error = "Parse error on line " + std::to_string(line_no) + ": geometry must be inside an object block";
                return false;
            }

            // Minimal compiler: generate a cube mesh.
            // Syntax: geometry cube [x,y,z]
            if (tokens.size() < 3 || tokens[1] != "cube") {
                error = "Parse error on line " + std::to_string(line_no) + ": expected: geometry cube [x,y,z]";
                return false;
            }

            Vec3d size;
            if (!parse_vec3(tokens[2], size)) {
                error = "Parse error on line " + std::to_string(line_no) + ": invalid cube size vec3";
                return false;
            }
            if (size.x() <= 0.0 || size.y() <= 0.0 || size.z() <= 0.0) {
                error = "Parse error on line " + std::to_string(line_no) + ": cube size must be > 0";
                return false;
            }

            const size_t src_obj_idx = *current_object_src;
            const size_t model_obj_idx = (mode == TaichiApplyMode::AppendObjects) ? append_obj_map.at(src_obj_idx) : src_obj_idx;
            ModelObject* obj = (mode == TaichiApplyMode::AppendObjects) ? model.objects[model_obj_idx] : ensure_object(model_obj_idx);
            if (obj == nullptr) {
                error = "Parse error on line " + std::to_string(line_no) + ": invalid current object";
                return false;
            }

            TriangleMesh mesh = make_cube(size.x(), size.y(), size.z());
            obj->clear_volumes();
            ModelVolume* vol = obj->add_volume(std::move(mesh));
            if (vol != nullptr)
                vol->name = obj->name;

            // Keep behavior aligned with importers: ensure extruder defaults to 1.
            if (!obj->config.has("extruder") || obj->config.extruder() == 0)
                obj->config.set_key_value("extruder", new ConfigOptionInt(1));

            obj->invalidate_bounding_box();
            mark_changed(model_obj_idx);
            continue;
        }

        // Ignore unknown lines for forward-compatibility.
    }

    return true;
}

bool apply_taichi_lang_to_model(Model& model, const std::string& text, std::vector<size_t>& changed_object_indices, std::string& error)
{
    return apply_taichi_lang_to_model_impl(model, text, TaichiApplyMode::ApplyToExisting, changed_object_indices, error);
}

bool append_taichi_lang_to_model(Model& model, const std::string& text, std::vector<size_t>& added_object_indices, std::string& error)
{
    // This returns indices of newly created objects.
    return apply_taichi_lang_to_model_impl(model, text, TaichiApplyMode::AppendObjects, added_object_indices, error);
}

} // namespace Slic3r
