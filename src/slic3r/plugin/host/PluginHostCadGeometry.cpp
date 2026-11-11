#include "PluginHostCad.hpp"
#include "libslic3r/CAD/CadShape.hpp"
#include "slic3r/GUI/CAD/CadFaceSelection.hpp"
#include "slic3r/GUI/CAD/CadShapeSelection.hpp"

#include <Standard_Failure.hxx>
#include <pybind11/cast.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <memory>
#include <optional>
#include <vector>

namespace py = pybind11;

namespace Slic3r {

void register_cad_geometry(py::module_& cad)
{
    py::register_exception<Standard_Failure>(cad, "GeometryError");
    py::enum_<CadShapeType>(cad, "ShapeType")
        .value("Compound", CadShapeType::Compound).value("CompSolid", CadShapeType::CompSolid)
        .value("Solid", CadShapeType::Solid).value("Shell", CadShapeType::Shell)
        .value("Face", CadShapeType::Face).value("Wire", CadShapeType::Wire)
        .value("Edge", CadShapeType::Edge).value("Vertex", CadShapeType::Vertex);
    py::class_<CadFrame>(cad, "Frame")
        .def_readonly("origin", &CadFrame::origin).def_readonly("x_axis", &CadFrame::x_axis)
        .def_readonly("y_axis", &CadFrame::y_axis).def_readonly("normal", &CadFrame::normal);
    py::class_<CadSurfaceSample>(cad, "SurfaceSample")
        .def_readonly("position", &CadSurfaceSample::position)
        .def_readonly("du", &CadSurfaceSample::du).def_readonly("dv", &CadSurfaceSample::dv)
        .def_readonly("frame", &CadSurfaceSample::frame).def_readonly("inside_trim", &CadSurfaceSample::inside_trim);
    py::class_<CadCurveSample>(cad, "CurveSample")
        .def_readonly("position", &CadCurveSample::position)
        .def_readonly("derivative", &CadCurveSample::derivative).def_readonly("tangent", &CadCurveSample::tangent);
    py::class_<CadBoundaryEdge>(cad, "BoundaryEdge")
        .def_readonly("shape", &CadBoundaryEdge::shape).def_readonly("is_seam", &CadBoundaryEdge::is_seam)
        .def_readonly("is_degenerate", &CadBoundaryEdge::is_degenerate);
    py::class_<CadBoundaryLoop>(cad, "BoundaryLoop")
        .def_readonly("is_outer", &CadBoundaryLoop::is_outer).def_readonly("is_complete", &CadBoundaryLoop::is_complete)
        .def_readonly("wire", &CadBoundaryLoop::wire).def_readonly("edges", &CadBoundaryLoop::edges);
    py::class_<CadShape, std::shared_ptr<CadShape>>(cad, "Shape")
        .def_property_readonly("type", &CadShape::type).def_property_readonly("orientation", &CadShape::orientation)
        .def("children", &CadShape::children).def("subshapes", &CadShape::subshapes, py::arg("type"))
        .def_property_readonly("vertex_point", &CadShape::vertex_point)
        .def_property_readonly("curve_type", &CadShape::curve_type)
        .def_property_readonly("parameter_range", &CadShape::parameter_range)
        .def_property_readonly("is_degenerate", &CadShape::is_degenerate)
        .def("evaluate_curve", &CadShape::evaluate_curve, py::arg("parameter"))
        .def("sample_curve", &CadShape::sample_curve, py::arg("count") = 33)
        .def_property_readonly("surface_type", &CadShape::surface_type)
        .def_property_readonly("uv_bounds", &CadShape::uv_bounds)
        .def_property_readonly("periodic", &CadShape::periodic)
        .def("evaluate_surface", &CadShape::evaluate_surface, py::arg("u"), py::arg("v"))
        .def_property_readonly("local_frame", &CadShape::local_frame)
        .def_property_readonly("area", &CadShape::area).def_property_readonly("centroid", &CadShape::face_centroid)
        .def("boundary_loops", &CadShape::boundary_loops);
    py::class_<GUI::CadFaceSelection>(cad, "SelectedFace")
        .def_readonly("body_index", &GUI::CadFaceSelection::body_index)
        .def_readonly("face_index", &GUI::CadFaceSelection::face_index)
        .def_readonly("selection_revision", &GUI::CadFaceSelection::selection_revision)
        .def_readonly("centroid", &GUI::CadFaceSelection::centroid).def_readonly("normal", &GUI::CadFaceSelection::normal)
        .def_readonly("area", &GUI::CadFaceSelection::area).def_readonly("is_planar", &GUI::CadFaceSelection::is_planar)
        .def_readonly("body_to_design", &GUI::CadFaceSelection::body_to_design)
        .def_readonly("shape", &GUI::CadFaceSelection::shape)
        .def_property_readonly("local_frame", [](const GUI::CadFaceSelection& f) {
            return f.shape ? f.shape->local_frame() : std::nullopt;
        })
        .def_property_readonly("boundary_loops", [](const GUI::CadFaceSelection& f) {
            return f.shape ? f.shape->boundary_loops() : std::vector<CadBoundaryLoop>{};
        });
    py::class_<GUI::CadShapeSelection>(cad, "SelectedShape")
        .def_readonly("kind", &GUI::CadShapeSelection::kind)
        .def_readonly("body_index", &GUI::CadShapeSelection::body_index)
        .def_readonly("face_index", &GUI::CadShapeSelection::face_index)
        .def_readonly("edge_index", &GUI::CadShapeSelection::edge_index)
        .def_readonly("vertex_index", &GUI::CadShapeSelection::vertex_index)
        .def_readonly("edge_indices", &GUI::CadShapeSelection::edge_indices)
        .def_readonly("selection_revision", &GUI::CadShapeSelection::selection_revision)
        .def_readonly("body_to_design", &GUI::CadShapeSelection::body_to_design)
        .def_readonly("shape", &GUI::CadShapeSelection::shape);
}

} // namespace Slic3r
