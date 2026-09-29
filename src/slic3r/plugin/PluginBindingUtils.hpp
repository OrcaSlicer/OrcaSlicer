#pragma once
#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include "libslic3r/Config.hpp"   // ConfigBase
#include "libslic3r/Point.hpp"    // Point/Point3 packing asserts, Vec3d, Transform3d
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// Point/Point3 must be tightly packed for zero-copy views. coord_t = int64_t.
static_assert(sizeof(Point)  == 2 * sizeof(coord_t), "Point must be 2 packed coord_t");
static_assert(sizeof(Point3) == 3 * sizeof(coord_t), "Point3 must be 3 packed coord_t");

// Run a builder that constructs numpy objects, translating the "numpy missing"
// ImportError into an actionable message (plugins must declare numpy as a dep).
template<typename Builder>
pybind11::object with_numpy(Builder&& build)
{
    namespace py = pybind11;
    try {
        return std::forward<Builder>(build)();
    } catch (py::error_already_set& err) {
        if (err.matches(PyExc_ImportError))
            throw py::import_error("numpy is required to access geometry/mesh arrays; "
                                   "add dependencies = [\"numpy\"] to your plugin metadata");
        throw;
    }
}

// Zero-copy, read-only (rows, N) numpy view over `data`, whose lifetime is tied
// to `base` (the array's base object). T is the element scalar (coord_t = int64
// for slicing coords, float for mesh vertices). rows == 0 / null data yields a
// fresh empty (0, N) array with no base.
template<typename T, int N>
pybind11::array make_readonly_rows(pybind11::handle base, const T* data, pybind11::ssize_t rows)
{
    namespace py = pybind11;
    if (rows == 0 || data == nullptr) {
        py::array_t<T> empty(std::vector<py::ssize_t>{ 0, (py::ssize_t) N });
        // Mark the fresh empty array read-only so every return path of this
        // helper yields a read-only view.
        empty.attr("setflags")(py::arg("write") = false);
        return std::move(empty);
    }
    py::array_t<T> arr(
        { rows, (py::ssize_t) N },
        { (py::ssize_t)(N * sizeof(T)), (py::ssize_t) sizeof(T) },
        data, base);
    // A base-carrying array is writable by default in pybind11; force read-only.
    arr.attr("setflags")(py::arg("write") = false);
    return std::move(arr);
}

// Zero-copy, WRITABLE (rows, N) numpy view over `data`, lifetime tied to `base`.
// Twin of make_readonly_rows: a base-carrying pybind array is writable by default,
// so we simply do not clear the write flag. Writing through the view mutates the
// underlying C++ buffer in place. rows == 0 / null data yields a fresh empty (0, N)
// array (writable, no base).
template<typename T, int N>
pybind11::array make_writable_rows(pybind11::handle base, T* data, pybind11::ssize_t rows)
{
    namespace py = pybind11;
    if (rows == 0 || data == nullptr)
        return py::array_t<T>(std::vector<py::ssize_t>{ 0, (py::ssize_t) N });
    return py::array_t<T>(
        { rows, (py::ssize_t) N },
        { (py::ssize_t)(N * sizeof(T)), (py::ssize_t) sizeof(T) },
        data, base);
}

// Serialize one config key to a Python string, or None if the key is absent.
// Works on any ConfigBase (resolved DynamicPrintConfig snapshots,
// PrintObjectConfig, PrintRegionConfig, preset configs).
inline pybind11::object config_value_or_none(const ConfigBase& config, const std::string& key)
{
    if (!config.has(key))
        return pybind11::none();
    return pybind11::cast(config.opt_serialize(key));
}

// Plugins receive 3D vectors as plain Python tuples (x, y, z) so the API stays
// Pythonic and free of an Eigen/numpy runtime dependency.
inline pybind11::tuple vec3_to_tuple(const Vec3d& v)
{
    return pybind11::make_tuple(v.x(), v.y(), v.z());
}

// 4x4 row-major float64 copy of an affine transform. Eigen stores column-major,
// so fill element-wise to produce correct C-order data. Requires numpy.
inline pybind11::object mat4_to_numpy(const Transform3d& transform)
{
    namespace py = pybind11;
    return with_numpy([&] {
        py::array_t<double> array({ py::ssize_t(4), py::ssize_t(4) });
        auto                view   = array.mutable_unchecked<2>();
        const auto&         matrix = transform.matrix();
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                view(i, j) = matrix(i, j);
        return py::object(std::move(array));
    });
}

} // namespace Slic3r
