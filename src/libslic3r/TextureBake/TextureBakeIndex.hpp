#pragma once

// Vertex welding for the texture bake pipeline. The pipeline works on non-indexed triangle soup, so
// a shared point exists once per incident triangle with float noise between the copies; welding maps
// each quantised position to one integer id.
//
// The three grids below are deliberately not unified - changing one at a call site changes
// watertightness. 100 um matches the precision files are written with; 10 um keeps small fillet
// vertices distinct (they merge at 100 um, giving needle artifacts after displacement) while still
// absorbing float noise; 1 um is what collapse positioning needs.

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

// Open-addressing table over flat arrays: no allocation per lookup, exact integer key comparison.
// Values must be non-negative; -1 is the empty sentinel and what get() returns on a miss.
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
        return m_val[slot(grid_round(double(x) * m_quant), grid_round(double(y) * m_quant),
                          grid_round(double(z) * m_quant))];
    }
    int get(const Vec3f &p) { return get(p.x(), p.y(), p.z()); }

    // The value already stored for this position's grid cell; if there is none, store `value` and
    // return it. inserted() then says which of the two happened.
    int get_or_set(float x, float y, float z, int value)
    {
        const int64_t qx = grid_round(double(x) * m_quant);
        const int64_t qy = grid_round(double(y) * m_quant);
        const int64_t qz = grid_round(double(z) * m_quant);
        const size_t  i  = slot(qx, qy, qz);
        if (m_val[i] != -1) {
            m_inserted = false;
            return m_val[i];
        }
        m_qx[i] = qx; m_qy[i] = qy; m_qz[i] = qz;
        m_val[i]   = value;
        m_inserted = true;
        if (++m_size > size_t(double(m_cap) * 0.7))
            grow();
        return value;
    }
    int get_or_set(const Vec3f &p, int value) { return get_or_set(p.x(), p.y(), p.z(), value); }

    // The same table as a set of integer tuples (edge marking, midpoint cache). Quantisation is
    // bypassed: routing ids through the float overloads loses precision above 2^24.
    int get_key(int64_t a, int64_t b, int64_t c) { return m_val[slot(a, b, c)]; }
    int get_or_set_key(int64_t a, int64_t b, int64_t c, int value)
    {
        const size_t i = slot(a, b, c);
        if (m_val[i] != -1) {
            m_inserted = false;
            return m_val[i];
        }
        m_qx[i] = a; m_qy[i] = b; m_qz[i] = c;
        m_val[i]   = value;
        m_inserted = true;
        if (++m_size > size_t(double(m_cap) * 0.7))
            grow();
        return value;
    }

private:
    void alloc(size_t cap)
    {
        m_cap  = cap;
        m_mask = cap - 1;
        m_qx.assign(cap, 0);
        m_qy.assign(cap, 0);
        m_qz.assign(cap, 0);
        m_val.assign(cap, -1);
    }

    size_t slot(int64_t qx, int64_t qy, int64_t qz) const
    {
        uint32_t h = uint32_t(int32_t(qx) * int32_t(0x9E3779B1)) ^
                     uint32_t(int32_t(qy) * int32_t(0x85EBCA77)) ^
                     uint32_t(int32_t(qz) * int32_t(0xC2B2AE3D));
        h ^= h >> 15;
        size_t i = size_t(h) & m_mask;
        // Equality is checked against the stored 64-bit keys, so truncating to 32 bits for the hash
        // costs collisions at worst, never a wrong answer.
        while (m_val[i] != -1) {
            if (m_qx[i] == qx && m_qy[i] == qy && m_qz[i] == qz)
                return i;
            i = (i + 1) & m_mask;
        }
        return i;
    }

    void grow()
    {
        std::vector<int64_t> oqx = std::move(m_qx), oqy = std::move(m_qy), oqz = std::move(m_qz);
        std::vector<int>     oval = std::move(m_val);
        const size_t         ocap = m_cap;
        alloc(ocap * 2);
        for (size_t i = 0; i < ocap; ++i) {
            if (oval[i] == -1)
                continue;
            const size_t s = slot(oqx[i], oqy[i], oqz[i]);
            m_qx[s] = oqx[i]; m_qy[s] = oqy[i]; m_qz[s] = oqz[i];
            m_val[s] = oval[i];
        }
    }

    double               m_quant;
    size_t               m_cap = 0, m_mask = 0, m_size = 0;
    bool                 m_inserted = false;
    std::vector<int64_t> m_qx, m_qy, m_qz;
    std::vector<int>     m_val;
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
