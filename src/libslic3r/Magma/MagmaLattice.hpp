#ifndef slic3r_Magma_MagmaLattice_hpp_
#define slic3r_Magma_MagmaLattice_hpp_

#include "../Point.hpp"
#include "../BoundingBox.hpp"
#include "MagmaCell.hpp"

#include <utility>
#include <vector>

namespace Slic3r {
namespace magma {

// ============================================================================
// MagmaLattice — per-cell-shape topology + coordinate strategy
// ============================================================================
//
// Companion to MagmaGeometry: where MagmaGeometry holds the scalar shape
// formulas (areas, opening diameter, ...), MagmaLattice holds the *spatial*
// behavior — how cells are identified, where their corners and centers sit, who
// their neighbors are, and how lattice coordinates map to world coordinates.
//
// Every Magma infill pattern supplies one implementation (TriangleLattice today;
// square / tri-hex later). The tube map / solver / greedy consume cells through
// this interface as opaque CellIds, which is what lets a new pattern drop in
// without re-typing the scheduling code.
//
// All world coordinates are in mm (unscaled). Variable-arity returns use
// std::vector because cell corner / neighbor counts differ per shape.
class MagmaLattice {
public:
    virtual ~MagmaLattice() = default;

    // ------------------------------------------------------------------
    // Topology
    // ------------------------------------------------------------------

    // Adjacent cells sharing an edge.
    virtual std::vector<CellId> neighbors(const CellId &cell) const = 0;

    // Triangle parity (△ vs ▽). Shapes without an up/down distinction return false.
    virtual bool is_up(const CellId &cell) const = 0;

    // Edge-sharing neighbor count (3 triangle / 4 square / 6 hex).
    virtual int max_neighbors() const = 0;

    // ------------------------------------------------------------------
    // Cell geometry
    // ------------------------------------------------------------------

    // Cell containing a world point.
    virtual CellId cell_at(double px, double py) const = 0;

    // Corners of a cell in world coordinates (variable count per shape).
    virtual std::vector<Vec2d> cell_corners(const CellId &cell) const = 0;

    // Center (centroid) of a cell in world coordinates.
    virtual Vec2d cell_center(const CellId &cell) const = 0;

    // All unique cells whose footprint touches the bounding box.
    virtual std::vector<CellId> enumerate_cells(const BoundingBox &bbox) const = 0;

    // ------------------------------------------------------------------
    // Coordinate transforms
    // ------------------------------------------------------------------

    virtual Vec2d to_world(double lx, double ly) const = 0;
    virtual std::pair<double, double> to_lattice(double px, double py) const = 0;

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------

    virtual double cell_spacing() const = 0;
    virtual double edge_length() const = 0;
    virtual double offset_x() const = 0;
    virtual double offset_y() const = 0;
};

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaLattice_hpp_
