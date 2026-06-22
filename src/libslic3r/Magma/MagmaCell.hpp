#ifndef slic3r_Magma_MagmaCell_hpp_
#define slic3r_Magma_MagmaCell_hpp_

#include <cstdint>
#include <functional>

namespace Slic3r {
namespace magma {

// ============================================================================
// CellId — generic lattice cell identity ("fat CellId")
// ============================================================================
//
// One shape-agnostic cell id used by the tube map / solver / greedy across all
// Magma lattices (triangle, square, tri-hex, ...). It carries up to three
// integer coordinates plus a small `kind` tag for lattices that need to
// distinguish multiple cell types at the same coordinate (e.g. tri-hex's
// triangle vs hexagon cells). Shape-specific meaning of (a, b, c, kind) and all
// geometry (neighbors, corners, parity) live in the MagmaLattice impl; this
// struct is pure data + ordering/hashing so it can key unordered/ordered
// containers.
//
// For the equilateral triangle lattice the fields map directly onto the old
// TriangleCell: (a, b, c) are the three triangle axes and `kind` is unused (0).
struct CellId {
    int     a = 0, b = 0, c = 0;
    uint8_t kind = 0;

    CellId() = default;
    CellId(int a_, int b_, int c_, uint8_t kind_ = 0)
        : a(a_), b(b_), c(c_), kind(kind_) {}

    bool operator==(const CellId &o) const {
        return a == o.a && b == o.b && c == o.c && kind == o.kind;
    }

    bool operator!=(const CellId &o) const {
        return !(*this == o);
    }

    // Lexicographic over (a, b, c, kind).
    bool operator<(const CellId &o) const {
        if (a != o.a) return a < o.a;
        if (b != o.b) return b < o.b;
        if (c != o.c) return c < o.c;
        return kind < o.kind;
    }
};

// Hash functor for CellId, suitable for use in unordered containers.
// Ports the TriangleCell bit-mixing and folds `kind` into the mix so cells that
// differ only by kind don't collide.
struct CellIdHash {
    size_t operator()(const CellId &c) const {
        // Combine all coordinates with bit mixing
        size_t h = std::hash<int>()(c.a);
        h ^= std::hash<int>()(c.b) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(c.c) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(static_cast<int>(c.kind)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

} // namespace magma
} // namespace Slic3r

#endif // slic3r_Magma_MagmaCell_hpp_
