#pragma once

// Vertex welding for the texture bake pipeline. The pipeline works on non-indexed triangle soup, so
// a shared point exists once per incident triangle with float noise between the copies; welding maps
// each quantised position to one integer id.
//
// The three grids below are deliberately not unified - changing one at a call site changes
// watertightness. 100 um matches the precision files are written with; 10 um keeps small fillet
// vertices distinct (they merge at 100 um, giving needle artifacts after displacement) while still
// absorbing float noise; 1 um is what collapse positioning needs.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../Point.hpp"

namespace Slic3r {
namespace TextureBake {

static constexpr double WELD_GRID_EXPORT     = 1e4; // 100 um
static constexpr double WELD_GRID_GEOMETRY   = 1e5; // 10 um
static constexpr double WELD_GRID_DECIMATION = 1e6; // 1 um

// Round half toward positive infinity. Quantised coordinates hit exact halves often enough that the
// tie rule matters.
inline int64_t grid_round(double v) { return int64_t(std::floor(v + 0.5)); }

// Open-addressing table, linear probing: no allocation per lookup, exact integer key comparison.
// Values must be non-negative; -1 is the empty sentinel and what get() returns on a miss.
//
// Key and value live together in one 32-byte cell. They used to be four parallel arrays, which made a
// single probe touch four cache lines - and probing this table was 13% of a whole bake, because every
// stage welds the full soup through it.
class QuantizedPointMap
{
public:
    explicit QuantizedPointMap(double quant, size_t expected = 256) : m_quant(quant)
    {
        size_t       cap    = 16;
        const size_t target = std::max<size_t>(16, size_t(std::ceil(double(expected) / 0.6)));
        while (cap < target)
            cap *= 2;
        alloc(cap);
    }

    size_t size() const { return m_size; }
    // Whether the last get_or_set() inserted rather than found.
    bool   inserted() const { return m_inserted; }

    int get(float x, float y, float z)
    {
        return m_cells[slot(grid_round(double(x) * m_quant), grid_round(double(y) * m_quant),
                            grid_round(double(z) * m_quant))].val;
    }
    int get(const Vec3f &p) { return get(p.x(), p.y(), p.z()); }

    // The value already stored for this position's grid cell; if there is none, store `value` and
    // return it. inserted() then says which of the two happened.
    int get_or_set(float x, float y, float z, int value)
    {
        return get_or_set_key(grid_round(double(x) * m_quant), grid_round(double(y) * m_quant),
                              grid_round(double(z) * m_quant), value);
    }
    int get_or_set(const Vec3f &p, int value) { return get_or_set(p.x(), p.y(), p.z(), value); }

    // The same table as a set of integer tuples (edge marking, midpoint cache). Quantisation is
    // bypassed: routing ids through the float overloads loses precision above 2^24.
    int get_key(int64_t a, int64_t b, int64_t c) { return m_cells[slot(a, b, c)].val; }
    int get_or_set_key(int64_t a, int64_t b, int64_t c, int value)
    {
        const size_t i = slot(a, b, c);
        Cell        &cell = m_cells[i];
        if (cell.val != -1) {
            m_inserted = false;
            return cell.val;
        }
        cell.qx = a; cell.qy = b; cell.qz = c;
        cell.val   = value;
        m_inserted = true;
        if (++m_size > size_t(double(m_cap) * 0.7))
            grow();
        return value;
    }

private:
    struct Cell
    {
        int64_t qx = 0, qy = 0, qz = 0;
        int32_t val = -1;
    };

    void alloc(size_t cap)
    {
        m_cap  = cap;
        m_mask = cap - 1;
        m_cells.assign(cap, Cell{});
    }

    size_t slot(int64_t qx, int64_t qy, int64_t qz) const
    {
        // Unsigned multiplies: the signed versions overflowed on nearly every key, which is undefined
        // behaviour. The resulting bits are identical on every target OrcaSlicer builds for.
        //
        // A stronger 64-bit finalizer was tried and measured no faster - the probing that shows up in a
        // profile is subdivide's parallel mark count, spread over every core, not long probe chains.
        uint32_t h = (uint32_t(qx) * 0x9E3779B1u) ^ (uint32_t(qy) * 0x85EBCA77u) ^ (uint32_t(qz) * 0xC2B2AE3Du);
        h ^= h >> 15;
        size_t i = size_t(h) & m_mask;
        // Equality is checked against the stored 64-bit keys, so truncating to 32 bits for the hash
        // costs collisions at worst, never a wrong answer.
        while (m_cells[i].val != -1) {
            const Cell &c = m_cells[i];
            if (c.qx == qx && c.qy == qy && c.qz == qz)
                return i;
            i = (i + 1) & m_mask;
        }
        return i;
    }

    void grow()
    {
        std::vector<Cell> old = std::move(m_cells);
        alloc(m_cap * 2);
        for (const Cell &c : old)
            if (c.val != -1)
                m_cells[slot(c.qx, c.qy, c.qz)] = c;
    }

    double            m_quant;
    size_t            m_cap = 0, m_mask = 0, m_size = 0;
    bool              m_inserted = false;
    std::vector<Cell> m_cells;
};

// Three consecutive entries per triangle. The indexers turn this into shared vertices where a stage
// needs adjacency.
struct TriSoup
{
    std::vector<Vec3f> pos;
    std::vector<Vec3f> nrm;            // parallel to pos
    std::vector<float> exclude_weight; // parallel to pos; empty when nothing is excluded

    size_t triangle_count() const { return pos.size() / 3; }
    bool   empty() const { return pos.empty(); }
};

// Assign each vertex the sequential id of its quantised position, first occurrence winning.
struct WeldResult
{
    std::vector<int> vertex_id;
    int              unique_count = 0;
};
WeldResult weld_vertices(const std::vector<Vec3f> &positions, double quant);

} // namespace TextureBake
} // namespace Slic3r
