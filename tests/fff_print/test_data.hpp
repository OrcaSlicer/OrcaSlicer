#ifndef SLIC3R_TEST_DATA_HPP
#define SLIC3R_TEST_DATA_HPP

#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <unordered_map>

namespace Slic3r { namespace Test {

constexpr double MM_PER_MIN = 60.0;

/// Enumeration of test meshes
enum class TestMesh {
    A,
    L,
    V,
    _40x10,
    cube_20x20x20,
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

// Necessary for <c++17
struct TestMeshHash {
    std::size_t operator()(TestMesh tm) const {
        return static_cast<std::size_t>(tm);
    }
};

/// Mesh enumeration to name mapping
extern const std::unordered_map<TestMesh, const char*, TestMeshHash> mesh_names;

/// Port of Slic3r::Test::mesh
/// Basic cubes/boxes should call TriangleMesh::make_cube() directly and rescale/translate it
TriangleMesh mesh(TestMesh m);

TriangleMesh mesh(TestMesh m, Vec3d translate, Vec3d scale = Vec3d(1.0, 1.0, 1.0));
TriangleMesh mesh(TestMesh m, Vec3d translate, double scale = 1.0);

/// Templated function to see if two values are equivalent (+/- epsilon)
template <typename T>
bool _equiv(const T& a, const T& b) { return std::abs(a - b) < EPSILON; }

template <typename T>
bool _equiv(const T& a, const T& b, double epsilon) { return abs(a - b) < epsilon; }

Slic3r::Model model(const std::string& model_name, TriangleMesh&& _mesh);
void init_print(std::vector<TriangleMesh> 		  &&meshes, Slic3r::Print &print, Slic3r::Model& model, const DynamicPrintConfig &config_in, bool comments = false);
void init_print(std::initializer_list<TestMesh> 	meshes, Slic3r::Print &print, Slic3r::Model& model, const Slic3r::DynamicPrintConfig &config_in = Slic3r::DynamicPrintConfig::full_print_config(), bool comments = false);
void init_print(std::initializer_list<TriangleMesh> meshes, Slic3r::Print &print, Slic3r::Model& model, const Slic3r::DynamicPrintConfig &config_in = Slic3r::DynamicPrintConfig::full_print_config(), bool comments = false);
void init_print(std::initializer_list<TestMesh> 	meshes, Slic3r::Print &print, Slic3r::Model& model, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items, bool comments = false);
void init_print(std::initializer_list<TriangleMesh> meshes, Slic3r::Print &print, Slic3r::Model& model, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items, bool comments = false);

void init_and_process_print(std::initializer_list<TestMesh> 	meshes, Slic3r::Print &print, const DynamicPrintConfig& config, bool comments = false);
void init_and_process_print(std::initializer_list<TriangleMesh> meshes, Slic3r::Print &print, const DynamicPrintConfig& config, bool comments = false);
void init_and_process_print(std::initializer_list<TestMesh> 	meshes, Slic3r::Print &print, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items, bool comments = false);
void init_and_process_print(std::initializer_list<TriangleMesh> meshes, Slic3r::Print &print, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items, bool comments = false);

std::string gcode(Print& print);

std::string slice(std::initializer_list<TestMesh> meshes, const DynamicPrintConfig &config, bool comments = false);
std::string slice(std::initializer_list<TriangleMesh> meshes, const DynamicPrintConfig &config, bool comments = false);
std::string slice(std::initializer_list<TestMesh> meshes, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items, bool comments = false);
std::string slice(std::initializer_list<TriangleMesh> meshes, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> config_items, bool comments = false);

} } // namespace Slic3r::Test


#endif // SLIC3R_TEST_DATA_HPP
