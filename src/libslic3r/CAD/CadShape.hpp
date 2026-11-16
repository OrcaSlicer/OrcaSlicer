#pragma once

#include <TopoDS_Shape.hxx>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

enum class CadShapeType { Compound, CompSolid, Solid, Shell, Face, Wire, Edge, Vertex };
using CadPoint = std::array<double, 3>;

struct CadFrame
{
    CadPoint origin, x_axis, y_axis, normal;
};

struct CadSurfaceSample
{
    CadPoint position, du, dv;
    std::optional<CadFrame> frame;
    bool inside_trim{false};
};

struct CadCurveSample
{
    CadPoint position, derivative;
    std::optional<CadPoint> tangent;
};

struct CadBoundaryLoop;

// Owned, read-only OCCT geometry. copy_of() detaches both topology and geometry from the
// live document. Descendants share that copy and its lock, never a document or GUI pointer.
// No method exports a mutable OCCT handle to Python.
class CadShape
{
public:
    static std::shared_ptr<CadShape> copy_of(const TopoDS_Shape& shape);
    CadShapeType type() const;
    std::string orientation() const;
    std::vector<std::shared_ptr<CadShape>> children() const;
    std::vector<std::shared_ptr<CadShape>> subshapes(CadShapeType type) const;

    CadPoint vertex_point() const;
    std::string curve_type() const;
    std::array<double, 2> parameter_range() const;
    bool is_degenerate() const;
    CadCurveSample evaluate_curve(double parameter) const;
    std::vector<CadPoint> sample_curve(int count = 33) const;

    std::string surface_type() const;
    std::array<double, 4> uv_bounds() const;
    std::array<bool, 2> periodic() const;
    CadSurfaceSample evaluate_surface(double u, double v) const;
    std::optional<CadFrame> local_frame() const;
    double area() const;
    CadPoint face_centroid() const;
    std::vector<CadBoundaryLoop> boundary_loops() const;

private:
    struct Context;
    CadShape(std::shared_ptr<Context> context, TopoDS_Shape shape);
    std::shared_ptr<CadShape> view(const TopoDS_Shape& shape) const;
    void require(CadShapeType type) const;
    std::shared_ptr<Context> m_context;
    TopoDS_Shape m_shape;
};

struct CadBoundaryEdge
{
    std::shared_ptr<CadShape> shape; // oriented occurrence; a seam may occur twice
    bool is_seam{false};
    bool is_degenerate{false};
};

struct CadBoundaryLoop
{
    bool is_outer{false};
    bool is_complete{false}; // malformed wires may not admit a complete connected traversal
    std::shared_ptr<CadShape> wire;
    std::vector<CadBoundaryEdge> edges;
};

} // namespace Slic3r
