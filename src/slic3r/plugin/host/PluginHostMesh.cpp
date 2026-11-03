#include "PluginHostBindings.hpp"
#include "PluginHostMesh.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/cast.h>
#include "slic3r/plugin/PluginBindingUtils.hpp"

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"
#include <cstddef>
#include <pybind11/numpy.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include <pybind11/detail/common.h>
#include <pybind11/detail/descr.h>

namespace py = pybind11;

namespace Slic3r {
namespace {

// Zero-copy export of its.vertices / its.indices relies on these Eigen
// row-vectors being tightly packed (no padding between the 3 components).
static_assert(sizeof(stl_vertex) == 3 * sizeof(float),
              "stl_vertex must be a packed float[3] for zero-copy numpy export");
static_assert(sizeof(stl_triangle_vertex_indices) == 3 * sizeof(std::int32_t),
              "triangle index must be a packed int32[3] for zero-copy numpy export");

} // namespace

void host_bindings::register_mesh(py::module_& host)
{
    // The raw libslic3r TriangleMesh, bound with a shared_ptr holder:
    // ModelVolume.mesh() hands out the volume's own shared_ptr, so the Python
    // object pins this snapshot even if the volume's mesh is later replaced on
    // the main thread. The zero-copy views below use the Python object as their
    // array base, which keeps the buffer alive for each array's lifetime.
    //
    // IMMUTABLE BY RULE: handed-out meshes are copy-on-write snapshots SHARED
    // across threads (a Print's model snapshot and the live GUI model share the
    // same instance), reached through a const_pointer_cast that only serves the
    // holder type. Bind only const/read-only methods here. A future mutable-mesh
    // API must operate on plugin-owned copies handed back via
    // ModelVolume::set_mesh — never mutate a mesh obtained from the graph.
    py::class_<TriangleMesh, std::shared_ptr<TriangleMesh>>(host, "TriangleMesh",
        "Immutable snapshot of a ModelVolume's mesh in local (untransformed) coordinates, mm.")
        .def("vertex_count", [](const TriangleMesh& mesh) { return mesh.its.vertices.size(); },
             "Number of vertices; vertex(index) takes 0 to vertex_count() - 1.")
        .def("triangle_count", [](const TriangleMesh& mesh) { return mesh.its.indices.size(); },
             "Number of triangles; triangle(index) takes 0 to triangle_count() - 1.")
        .def("facets_count", [](const TriangleMesh& mesh) { return mesh.its.indices.size(); },
             "Number of triangles, the same as triangle_count().")
        .def("is_empty", [](const TriangleMesh& mesh) { return mesh.its.indices.empty(); },
             "True when the mesh has no triangles.")
        .def("vertices", [](py::object self) {
            const TriangleMesh& mesh = self.cast<const TriangleMesh&>();
            return with_numpy([&] {
                const std::vector<stl_vertex>& vertices = mesh.its.vertices;
                return py::object(make_readonly_rows<float, 3>(
                    self, vertices.empty() ? nullptr : vertices.front().data(),
                    static_cast<py::ssize_t>(vertices.size())));
            });
        }, "Read-only zero-copy (N, 3) float32 ndarray of vertex positions (local mm). Requires numpy.")
        .def("triangles", [](py::object self) {
            const TriangleMesh& mesh = self.cast<const TriangleMesh&>();
            return with_numpy([&] {
                const std::vector<stl_triangle_vertex_indices>& indices = mesh.its.indices;
                return py::object(make_readonly_rows<std::int32_t, 3>(
                    self, indices.empty() ? nullptr : indices.front().data(),
                    static_cast<py::ssize_t>(indices.size())));
            });
        }, "Read-only zero-copy (M, 3) int32 ndarray of triangle vertex indices (0-based rows of vertices()). "
           "Requires numpy.")
        .def("face_normals", [](const TriangleMesh& mesh) {
            return with_numpy([&] {
                std::vector<Vec3f> normals = its_face_normals(mesh.its);
                py::array_t<float> array({ static_cast<py::ssize_t>(normals.size()), py::ssize_t(3) });
                if (!normals.empty()) {
                    auto view = array.mutable_unchecked<2>();
                    for (size_t i = 0; i < normals.size(); ++i) {
                        view(i, 0) = normals[i].x();
                        view(i, 1) = normals[i].y();
                        view(i, 2) = normals[i].z();
                    }
                }
                return py::object(std::move(array));
            });
        }, "Per-triangle normalized normals as an (M, 3) float32 ndarray (copy). Requires numpy.")
        .def("vertex", [](const TriangleMesh& mesh, size_t index) {
            const std::vector<stl_vertex>& vertices = mesh.its.vertices;
            if (index >= vertices.size())
                throw py::index_error("vertex index out of range");
            const stl_vertex& vertex = vertices[index];
            return py::make_tuple(vertex.x(), vertex.y(), vertex.z());
        }, py::arg("index"),
           "Vertex at index, 0-based, as an (x, y, z) tuple of floats in local mm; raises IndexError when out of "
           "range. Needs no numpy; vertices() returns them all at once.")
        .def("triangle", [](const TriangleMesh& mesh, size_t index) {
            const std::vector<stl_triangle_vertex_indices>& indices = mesh.its.indices;
            if (index >= indices.size())
                throw py::index_error("triangle index out of range");
            const stl_triangle_vertex_indices& triangle = indices[index];
            return py::make_tuple(triangle[0], triangle[1], triangle[2]);
        }, py::arg("index"),
           "Triangle at index, 0-based, as a tuple (i0, i1, i2) of 0-based vertex indices, ordered so the right-hand "
           "rule gives its normal; raises IndexError when out of range. Needs no numpy; triangles() returns them all.")
        .def("volume", [](const TriangleMesh& mesh) { return mesh.stats().volume; },
             "Volume in mm^3 that the mesh encloses; -1 when it was never computed. Meaningful only for a closed "
             "mesh (see is_manifold()).")
        .def("bounding_box", [](const TriangleMesh& mesh) { return bbox_from_stats(mesh.stats()); },
             "Bounding box of the mesh's vertices in its local frame, in mm.")
        .def("is_manifold", [](const TriangleMesh& mesh) { return mesh.stats().manifold(); },
             "True when the mesh has no open edges, that is, it is closed.");
}

} // namespace Slic3r
