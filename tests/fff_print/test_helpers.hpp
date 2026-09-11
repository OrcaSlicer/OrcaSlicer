#ifndef SLIC3R_TEST_HELPERS_HPP
#define SLIC3R_TEST_HELPERS_HPP

#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r { namespace Test {

constexpr double MM_PER_MIN = 60.0;

// True when `a` and `b` are within EPSILON.
template <typename T>
bool _equiv(const T& a, const T& b) { return std::abs(a - b) < EPSILON; }

// True when `a` and `b` are within `epsilon`.
template <typename T>
bool _equiv(const T& a, const T& b, double epsilon) { return abs(a - b) < epsilon; }

// Named reusable test meshes, resolved by mesh().
enum class TestMesh {
    A,
    L,
    V,
    _40x10,
    sphere_50mm,
    bridge,
    bridge_with_hole,
    cube_with_concave_hole,
    cube_with_hole,
    gt2_teeth,
    ipadstand,
    overhang,
    pyramid,
    sloping_hole,
    slopy_cube,
    small_dorito,
    step,
    two_hollow_squares
};

// Hash for TestMesh (std::hash lacks scoped-enum support before C++17).
struct TestMeshHash {
    std::size_t operator()(TestMesh tm) const {
        return static_cast<std::size_t>(tm);
    }
};

// TestMesh value to name mapping.
extern const std::unordered_map<TestMesh, const char*, TestMeshHash> mesh_names;

// Geometry for the named test fixture `m`, optionally translated and scaled.
TriangleMesh mesh(TestMesh m);
TriangleMesh mesh(TestMesh m, Vec3d translate, Vec3d scale = Vec3d(1.0, 1.0, 1.0));
TriangleMesh mesh(TestMesh m, Vec3d translate, double scale = 1.0);

// An equal-sided cube, `size` mm on each edge.
inline TriangleMesh cube(double size) { return make_cube(size, size, size); }

// A Model holding one object built from `mesh`.
Slic3r::Model model(const std::string& model_name, TriangleMesh&& _mesh);

// Single-nozzle, `filaments`-filament config from defaults; `extra` is applied last.
DynamicPrintConfig multifilament_config(unsigned int filaments,
    std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {});

// Apply `meshes` and config to `print`/`model`; optional per-object overrides, auto-arranged unless `arrange` is false.
void init_print(std::vector<TriangleMesh> &&meshes, Slic3r::Print &print, Slic3r::Model &model, const DynamicPrintConfig &config_in,
    const std::vector<std::vector<Slic3r::ConfigBase::SetDeserializeItem>> *per_object_overrides = nullptr, bool arrange = true);
void init_print(std::initializer_list<TestMesh> meshes, Slic3r::Print &print, Slic3r::Model &model, const Slic3r::DynamicPrintConfig &config_in = Slic3r::DynamicPrintConfig::full_print_config());
void init_print(std::initializer_list<TriangleMesh> meshes, Slic3r::Print &print, Slic3r::Model &model, const Slic3r::DynamicPrintConfig &config_in = Slic3r::DynamicPrintConfig::full_print_config());
void init_print(std::initializer_list<TestMesh> meshes, Slic3r::Print &print, Slic3r::Model &model, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items);
void init_print(std::initializer_list<TriangleMesh> meshes, Slic3r::Print &print, Slic3r::Model &model, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items);

// init_print followed by process(), leaving a sliced `print` to inspect.
void init_and_process_print(std::initializer_list<TestMesh> meshes, Slic3r::Print &print, const DynamicPrintConfig& config);
void init_and_process_print(std::initializer_list<TriangleMesh> meshes, Slic3r::Print &print, const DynamicPrintConfig& config);
void init_and_process_print(std::initializer_list<TestMesh> meshes, Slic3r::Print &print, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items);
void init_and_process_print(std::initializer_list<TriangleMesh> meshes, Slic3r::Print &print, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items);

// Process `print` and return its exported G-code.
std::string gcode(Print& print);

// Build, slice, and return the G-code for `meshes` under the given config.
std::string slice(std::initializer_list<TestMesh> meshes, const DynamicPrintConfig &config);
std::string slice(std::initializer_list<TriangleMesh> meshes, const DynamicPrintConfig &config);
std::string slice(std::initializer_list<TestMesh> meshes, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items);
std::string slice(std::initializer_list<TriangleMesh> meshes, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items);

// Slice `meshes`, applying per_object_overrides[i] to object i first (empty entry = none).
std::string slice_with_object_overrides(std::initializer_list<TriangleMesh> meshes, const DynamicPrintConfig &config,
    const std::vector<std::vector<Slic3r::ConfigBase::SetDeserializeItem>> &per_object_overrides);

// Slice two auto-arranged 20mm cubes (the arranger positions them).
std::string slice_two_cubes_arranged(std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items);

// Place two 20mm cubes `gap` mm apart edge-to-edge, not auto-arranged (the caller controls spacing).
void place_two_cubes_apart(double gap, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items,
    Slic3r::Print &print, Slic3r::Model &model);
// Slice two 20mm cubes `gap` mm apart (not auto-arranged) and return the G-code.
std::string slice_two_cubes_apart(double gap, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items);

// Place two instances of one 20mm cube `gap` mm apart edge-to-edge.
void place_two_cube_instances_apart(double gap, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items,
    Slic3r::Print &print, Slic3r::Model &model);

// Distinct layer Z heights carrying an extrusion of the given `role` (e.g. "skirt").
std::set<double> layers_with_role(const std::string &gcode, const std::string &role);

// Highest Z reached by any move in the G-code.
double max_z(const std::string &gcode);

// Count of contiguous extrusion blocks of `role` (each uninterrupted run counts once).
int role_passes(const std::string &gcode, const std::string &role);

// The `roles` in the order their extrusion blocks first appear, consecutive repeats collapsed.
std::vector<std::string> role_sequence(const std::string &gcode, const std::vector<std::string> &roles);

} } // namespace Slic3r::Test

#endif // SLIC3R_TEST_HELPERS_HPP
