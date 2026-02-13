#ifndef MARCHINGSQUARES_HPP
#define MARCHINGSQUARES_HPP

#include "Execution/ExecutionTBB.hpp"
#include <type_traits>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cassert>

// Marching squares
//
// This algorithm generates 2D contour rings for a 2D scalar field (height
// map). See https://en.wikipedia.org/wiki/Marching_squares for details.
//
// This algorithm uses square cells (tiles) with corner tags that are set
// depending on which vertices are inside an ROI (higher than the selected
// isovalue threshold). We label the vertices counter-clockwise, and for ease
// of extraction from the m_tags bitmap we put them into a four bit bitmap in
// little-endian row-column order <cbda>;
//
//            ^ u (up)         ----> column "c" direction
//            |
//         a ----- d
//         |       |           |
//    l <--|       |--> r      | increasing row "r" direction
// (left)  |       |  (right)  \/
//         b ----- c
//             |
//             \/ d (down)
//
// The m_tags bitmap has the top-left "a" corner of each cell packed
// into uint32_t values stored in little-endian order, with the "a" tag for
// the first cell (r=0,c=0) in the LSB bit0 of the first 32bit word of the
// first row. The grid has a one cell border round the raster area that is
// always clear so lines never extend out of the grid.
//
// From this grid of points we can get the <cbda> tags bitmap for the four
// corners of each cell with "a" in the LSB bit0 and "c" in bit3. Note the
// bits b,c,d are also the "a" bits of the cell's next-colum/row neighbour
// cells. Cells in the right-most column get their cleared right-side border
// <c_b_> tags by wrapping around the grid and getting the cleared values
// from the left-side border. Note the grid bitmap always includes at least
// one extra cleared boarder bit for the wrap-around of the last tag in the
// last cell. Cells in the bottom row get their cleared bottom-side border
// <cb__> tags by leaving them clear because their grid index exceeds the
// grid size.
//
// These tag bits are then translated into up/down/left/right cell-exit
// directions indicating the cell edge(s) that any lines will cross and exit
// the cell. As you walk counter-clock-wise around the cell corners in the
// order a->b->c->d, any edge from a unset-corner-tag to an set-corner-tag is
// where a line will cross, exiting the cell into the next cell.
//
// Note any set-to-unset-transition edge is where a line will enter a cell,
// but for marching the lines through the cells we only need to keep the next
// cell exit directions. The exit directions are stored as 4bit <urdl>
// left/down/right/up flags packed into uint32_t values in m_dirs, in
// counter-clockwise little-endian order.
//
// To build the line-rings we scan through the m_dirs directions for the
// cells until we find one with a set-edge. We then start a line and "march"
// it through the cells following the directions in each cell, recording each
// edge the line crosses that will become a point in the ring.
//
// Note that the outside-edge of the grid extends one cell past the raster
// grid, and this outer edge is always cleared. This means no line will cross
// out of the grid, and they will all eventually connect back to their
// starting cell to become a ring around an island of set tag bits, possibly
// containing rings around lakes of unset tag bits within them.
//
// As we march through the cells and build the lines, we clear the direction
// bits to indicate the edge-transition has been processed. After completing
// a ring we return to scanning for the next non-zeroed starting cell for the
// next ring. Eventually all rings will be found and completed, and the
// m_dirs map will be completely cleared.
//
// Note there are two ambigous cases when the diagonally opposite corner tags
// are set. In these cases two lines will enter and exit the cell. To avoid
// messy lines, we consistently choose the exit direction based on what edge
// the line entered the cell from. This also means when we start scanning for
// the next ring we should start checking at the same cell the last ring
// started from, because it could have a second ring passing through it.
//
namespace marchsq {

// Marks a square or point in grid or raster coordintes.
struct Coord
{
    long r = 0, c = 0;

    Coord() = default;
    explicit Coord(long s) : r(s), c(s) {}
    Coord(long _r, long _c) : r(_r), c(_c) {}

    size_t seq(const Coord& res) const { return r * res.c + c; }
    Coord& operator+=(const Coord& b)
    {
        r += b.r;
        c += b.c;
        return *this;
    }
    Coord operator+(const Coord& b) const
    {
        Coord a = *this;
        a += b;
        return a;
    }
    bool operator==(const Coord& o) const { return (r == o.r) && (c == o.c); }
    bool operator!=(const Coord& o) const { return !(*this == o); }
};

inline std::ostream& operator<<(std::ostream& os, const Coord& o) { return os << "(r=" << o.r << ",c=" << o.c << ")"; }

// Closed ring of cell coordinates
using Ring = std::vector<Coord>;

inline std::ostream& operator<<(std::ostream& os, const Ring& r)
{
    os << "[" << r.size() << "]:";
    for (Coord c : r)
        os << " " << c;
    return os << "\n";
}

// Specialize this struct to register a raster type for MarchingSquares.
template<class T, class Enable = void> struct _RasterTraits
{
    // The type of pixel cell in the raster
    using ValueType = typename T::ValueType;

    // Value at a given position
    static ValueType get(const T& raster, size_t row, size_t col);

    // Number of rows and cols of the raster
    static size_t rows(const T& raster);
    static size_t cols(const T& raster);
};

// Specialize this to use parellel loops within the algorithm
template<class ExecutionPolicy, class Enable = void> struct _Loop
{
    template<class It, class Fn> static void for_each_idx(It from, It to, Fn&& fn)
    {
        for (auto it = from; it < to; ++it)
            fn(*it, size_t(it - from));
    }
};

// Add Specialization for using ExecutionTBB for parallel loops.
using namespace Slic3r;
template<> struct _Loop<ExecutionTBB>
{
    template<class It, class Fn> static void for_each_idx(It from, It to, Fn&& fn)
    {
        execution::for_each(
            ex_tbb, size_t(0), size_t(to - from), [&from, &fn](size_t i) { fn(from[i], i); }, execution::max_concurrency(ex_tbb));
    }
};

namespace __impl {

template<class T> using RasterTraits = _RasterTraits<std::decay_t<T>>;
template<class T> using TRasterValue = typename RasterTraits<T>::ValueType;

template<class T> size_t rows(const T& raster) { return RasterTraits<T>::rows(raster); }

template<class T> size_t cols(const T& raster) { return RasterTraits<T>::cols(raster); }

template<class T> TRasterValue<T> isoval(const T& rst, const Coord& crd) { return RasterTraits<T>::get(rst, crd.r, crd.c); }

template<class ExecutionPolicy, class It, class Fn> void for_each_idx(ExecutionPolicy&& policy, It from, It to, Fn&& fn)
{
    _Loop<ExecutionPolicy>::for_each_idx(from, to, fn);
}

template<class E> constexpr std::underlying_type_t<E> _t(E e) noexcept { return static_cast<std::underlying_type_t<E>>(e); }

// A set of next direction flags for a cell to indicate what sides(s)
// any yet-to-be-marched line(s) will cross to leave the cell.
enum class Dir : uint8_t {
    none      = 0b0000,
    left      = 0b0001, /* exit through a-b edge */
    down      = 0b0010, /* exit through b-c edge */
    right     = 0b0100, /* exit through c-d edge */
    leftright = left | right,
    up        = 0b1000, /* exit through d-a edge */
    updown    = up | down,
    all       = 0b1111
};

inline std::ostream& operator<<(std::ostream& os, const Dir& d) { return os << ".<v#>x##^#X#####"[_t(d)]; }

// This maps square tag column/row order <cbda> bitmaps to the next
// direction(s) a line will exit the cell. The directions ensure the
// line-rings circle counter-clock-wise around the clusters of set tags.
static const constexpr std::array<uint32_t, 16> NEXT_CCW = [] {
    auto map = decltype(NEXT_CCW){};
    for (uint32_t cbda = 0; cbda < 16; cbda++) {
        const uint32_t dcba = (cbda & 0b0001) | ((cbda >> 1) & 0b0110) | ((cbda << 2) & 0b1000);
        const uint32_t adcb = (dcba >> 1) | ((dcba << 3) & 0b1000);
        map[cbda]           = ~dcba & adcb;
    }
    return map;
}();

// Step a point in a direction, optionally by n steps.
inline void step(Coord& crd, const Dir d, const long n = 1)
{
    switch (d) {
    case Dir::left: crd.c -= n; break;
    case Dir::down: crd.r += n; break;
    case Dir::right: crd.c += n; break;
    case Dir::up: crd.r -= n; break;
    }
}

template<class Rst> class Grid
{
    const Rst*            m_rst = nullptr;
    Coord                 m_window, m_rastsize, m_gridsize;
    size_t                m_gridlen; // The number of cells in the grid.
    std::vector<uint32_t> m_tags;    // bit-packed squaretags for each corner.
    std::vector<uint32_t> m_dirs;    // bit-packed next directions for each cell.

    // Convert a grid coordinate point into raster coordinates.
    inline Coord rastercoord(const Coord& crd) const
    {
        // Note the -1 offset for the grid border around the raster area.
        return {(crd.r - 1) * m_window.r, (crd.c - 1) * m_window.c};
    }

    // Get the 4 corners of a grid coordinate cell in raster coordinates.
    Coord bl(const Coord& crd) const { return tl(crd) + Coord{m_window.r, 0}; }
    Coord br(const Coord& crd) const { return tl(crd) + Coord{m_window.r, m_window.c}; }
    Coord tr(const Coord& crd) const { return tl(crd) + Coord{0, m_window.c}; }
    Coord tl(const Coord& crd) const { return rastercoord(crd); }

    // Test if a raster coordinate point is within the raster area.
    inline bool is_within(const Coord& crd) const { return crd.r >= 0 && crd.r < m_rastsize.r && crd.c >= 0 && crd.c < m_rastsize.c; }

    // Get a block of 32 tags for a block index from the raster isovals.
    uint32_t get_tags_block32(const size_t bidx, const TRasterValue<Rst> v) const
    {
        Coord    gcrd = coord(bidx * 32);  // position in grid coordinates of the block.
        Coord    rcrd = rastercoord(gcrd); // position in raster coordinates.
        uint32_t tags = 0;
        // Set a bit for each corner that has osoval > v.
        for (uint32_t b = 1; b; b <<= 1) {
            if (is_within(rcrd) && isoval(*m_rst, rcrd) > v)
                tags |= b;
            gcrd.c += 1;
            rcrd.c += m_window.c;
            // If we hit the end of the row, start on the next row.
            if (gcrd.c >= m_gridsize.c) {
                gcrd = Coord(gcrd.r + 1, 0);
                rcrd = rastercoord(gcrd);
            }
        }
        return tags;
    }

    // Get a block of 8 directions for a block index from the tags.
    uint32_t get_dirs_block8(const size_t bidx) const
    {
        size_t   gidx = bidx * 8;
        uint32_t dirs = 0;

        // Get the next 9 top-row tags at this grid index into the bottom 16
        // bits, and the 9 bottom-row tags into the top 16 bits.
        uint32_t tags9 = get_tags9(gidx) | (get_tags9(gidx + m_gridsize.c) << 16);
        // Skip generating dirs if the tags are all 1's or all 0's.
        if ((tags9 != 0) && (tags9 != 0x01ff01ff)) {
            for (auto s = 0; s < 32; s += 4) {
                uint8_t tags = (tags9 & 0b11) | ((tags9 >> 14) & 0b1100);
                dirs |= NEXT_CCW[tags] << s;
                tags9 >>= 1;
            }
        }
        return dirs;
    }

    // Get the next 9 corner tags on a row for building a dirs block at a grid index.
    uint32_t get_tags9(size_t gidx) const
    {
        uint32_t tags = 0;         // the tags value.
        size_t   i    = gidx / 32; // the tags block index
        int      o    = gidx % 32; // the tags block offset

        if (gidx < m_gridlen) {
            // get the next 9 tags in the row.
            tags = (m_tags[i] >> o) & 0x1ff;
            // Some of the tags are in the next tags block.
            if ((o > (32 - 9)) && ((i + 1) < m_tags.size())) {
                tags |= (m_tags[i + 1] << (32 - o)) & 0x1ff;
            }
        }
        return tags;
    }

    // Clear directions in a cell's <urdl> dirs for a grid index.
    void clr_dirs(const size_t gidx, const Dir d)
    {
        assert(gidx < m_gridlen);
        size_t i = gidx / 8;   // the dirs block index
        int    o = (gidx % 8); // the dirs block offset

        m_dirs[i] &= ~(static_cast<uint32_t>(d) << (o * 4));
    }

    // Get directions in a cell's <uldr> dirs from the m_dirs store.
    Dir get_dirs(const size_t gidx) const
    {
        assert(gidx < m_gridlen);
        size_t i = gidx / 8;   // the dirs block index
        int    o = (gidx % 8); // the dirs block offset

        return Dir((m_dirs[i] >> (o * 4)) & 0b1111);
    }

    // Get a cell coordinate from a sequential grid index.
    Coord coord(size_t i) const { return {long(i) / m_gridsize.c, long(i) % m_gridsize.c}; }

    // Get a sequential index from a cell coordinate.
    size_t seq(const Coord& crd) const { return crd.seq(m_gridsize); }

    // Step a sequential grid index in a direction.
    size_t stepidx(const size_t idx, const Dir d) const
    {
        assert(idx < m_gridlen);
        switch (d) {
        case Dir::left: return idx - 1;
        case Dir::down: return idx + m_gridsize.c;
        case Dir::right: return idx + 1;
        case Dir::up: return idx - m_gridsize.c;
        default: return idx;
        }
    }

    // Search for a new starting square.
    size_t search_start_cell(size_t gidx = 0) const
    {
        size_t i = gidx / 8; // The dirs block index.
        int    o = gidx % 8; // The dirs block offset.

        // Skip cells without any unvisited edges.
        while (i < m_dirs.size()) {
            if (!m_dirs[i]) {
                // Whole block is clear, advance to the next block;
                i++;
                o = 0;
            } else {
                // Block not clear, find the next non-zero tags in the block. Note
                // all dirs before gidx are cleared, so any set bits must be in
                // block offsets >= o, not before.
                for (uint32_t m = 0b1111 << (o * 4); !(m_dirs[i] & m); m <<= 4)
                    o++;
                break;
            }
        }
        return i * 8 + o;
    }

    // Get the next direction for a cell index after the prev direction.
    Dir next_dir(size_t idx, Dir prev = Dir::all) const
    {
        Dir next = get_dirs(idx);

        // Treat ambiguous cases as two separate regions in one square. If
        // there are two possible next directions, pick based on the prev
        // direction. If prev=all we are starting a new line so pick the one
        // that leads "forwards" (right or down) in the search.
        switch (next) {
        case Dir::leftright:
            // We must be coming from up, down, or starting a new line.
            assert(prev == Dir::up || prev == Dir::down || prev == Dir::all);
            return (prev == Dir::up) ? Dir::left : Dir::right;
        case Dir::updown:
            // We must be coming from left, right, or starting a new line.
            assert(prev == Dir::left || prev == Dir::right || prev == Dir::all);
            return (prev == Dir::right) ? Dir::up : Dir::down;
        default:
            // Next must be a single direction or none to stop.
            assert(next == Dir::none || next == Dir::left || next == Dir::down || next == Dir::right || next == Dir::up);
            return next;
        }
    }

    struct CellIt
    {
        using iterator_category = std::random_access_iterator_tag;
        using value_type        = TRasterValue<Rst>;
        using pointer           = TRasterValue<Rst>*;
        using reference         = TRasterValue<Rst>&;
        using difference_type   = long;

        Coord      crd;
        Dir        dir  = Dir::none;
        const Rst* grid = nullptr;

        inline TRasterValue<Rst>  operator*() const { return isoval(*grid, crd); }
        inline TRasterValue<Rst>& operator[](long n) const { return *((*this) + n); }
        inline CellIt&            operator++()
        {
            step(crd, dir);
            return *this;
        }
        inline CellIt operator++(int)
        {
            CellIt o = *this;
            ++(*this);
            return o;
        }
        inline CellIt& operator--()
        {
            step(crd, dir, -1);
            return *this;
        }
        inline CellIt operator--(int)
        {
            CellIt o = *this;
            --(*this);
            return o;
        }
        inline CellIt& operator+=(long n)
        {
            step(crd, dir, n);
            return *this;
        }
        inline CellIt& operator-=(long n)
        {
            step(crd, dir, -n);
            return *this;
        }
        inline CellIt operator+(long n) const
        {
            CellIt o = *this;
            o += n;
            return o;
        }
        inline CellIt operator-(long n) const
        {
            CellIt o = *this;
            o -= n;
            return o;
        }
        inline long operator-(const CellIt& o) const
        {
            switch (dir) {
            case Dir::left: return o.crd.c - crd.c;
            case Dir::down: return crd.r - o.crd.r;
            case Dir::right: return crd.c - o.crd.c;
            case Dir::up: return o.crd.r - crd.r;
            default: return 0;
            }
        }
        inline bool operator==(const CellIt& o) const { return crd == o.crd; }
        inline bool operator!=(const CellIt& o) const { return crd != o.crd; }
        inline bool operator<(const CellIt& o) const { return (*this - o) < 0; }
        inline bool operator>(const CellIt& o) const { return (*this - o) > 0; }
        inline bool operator<=(const CellIt& o) const { return (*this - o) <= 0; }
        inline bool operator>=(const CellIt& o) const { return (*this - o) >= 0; }
    };

    // Two cell iterators representing an edge of a square. This is then
    // used for binary search for the first active pixel on the edge.
    struct Edge
    {
        CellIt from, to;
    };

    Edge _edge(const Coord& ringvertex) const
    {
        size_t idx  = ringvertex.r;
        Coord  cell = coord(idx);
        Dir    d    = Dir(ringvertex.c);

        switch (d) {
        case Dir::left: return {{tl(cell), Dir::down, m_rst}, {bl(cell)}};
        case Dir::down: return {{bl(cell), Dir::right, m_rst}, {br(cell)}};
        case Dir::right: return {{br(cell), Dir::up, m_rst}, {tr(cell)}};
        case Dir::up: return {{tr(cell), Dir::left, m_rst}, {tl(cell)}};
        default: assert(false);
        }
        return {};
    }

    Edge edge(const Coord& ringvertex) const
    {
        Edge e   = _edge(ringvertex);
        e.to.dir = e.from.dir;
        ++e.to;

        e.from.crd.r = std::clamp(e.from.crd.r, 0l, m_rastsize.r - 1);
        e.from.crd.c = std::clamp(e.from.crd.c, 0l, m_rastsize.c - 1);
        e.to.crd.r   = std::clamp(e.to.crd.r, 0l, m_rastsize.r);
        e.to.crd.c   = std::clamp(e.to.crd.c, 0l, m_rastsize.c);
        return e;
    }

    void interpolate_edge(Coord& ecrd, TRasterValue<Rst> isoval) const
    {
        // The ecrd must have a grid index in r and a direction in c.
        assert((0 <= ecrd.r) && (ecrd.r < m_gridlen));
        assert((ecrd.c == long(Dir::left)) || (ecrd.c == long(Dir::down)) || (ecrd.c == long(Dir::right)) || (ecrd.c == long(Dir::up)));
        Edge e = edge(ecrd);
        ecrd   = std::lower_bound(e.from, e.to, isoval).crd;
        // Shift bottom and right side points "out" by one to account for
        // raster pixel width. Note "dir" is the direction of interpolation
        // along the cell edge, not the next move direction.
        if (e.from.dir == Dir::up)
            ecrd.r += 1;
        else if (e.from.dir == Dir::left)
            ecrd.c += 1;
    }

public:
    explicit Grid(const Rst& rst, const Coord& window)
        : m_rst{&rst}
        , m_window{window}
        , m_rastsize{static_cast<long>(rows(rst)), static_cast<long>(cols(rst))}
        , m_gridsize{2 + m_rastsize.r / m_window.r, 2 + m_rastsize.c / m_window.c}
        , m_gridlen{static_cast<size_t>(m_gridsize.r * m_gridsize.c)}
        , m_tags(m_gridlen / 32 + 1, 0) // 1 bit per tag means 32 per uint32_t.
        , m_dirs(m_gridlen / 8 + 1, 0)  // 4 bits per cell means 32/4=8 per uint32_t.
    {}

    // Go through the cells getting their tags and dirs.
    template<class ExecutionPolicy> void tag_grid(ExecutionPolicy&& policy, TRasterValue<Rst> isoval)
    {
        // Get all the tags. parallel?
        for_each_idx(std::forward<ExecutionPolicy>(policy), m_tags.begin(), m_tags.end(),
                     [this, isoval](uint32_t& tag_block, size_t bidx) { tag_block = get_tags_block32(bidx, isoval); });
        // streamtags(std::cerr);
        //  Get all the dirs. parallel?
        for_each_idx(std::forward<ExecutionPolicy>(policy), m_dirs.begin(), m_dirs.end(),
                     [this](uint32_t& dirs_block, size_t bidx) { dirs_block = get_dirs_block8(bidx); });
        // streamdirs(std::cerr);
    }

    // Scan for the rings on the tagged grid. Each ring vertex uses the Coord
    // to store the sequential cell index (idx in r) and next direction (Dir
    // in c) for the next point of the ring. This info can be used later to
    // calculate the exact raster coordinate of the point.
    std::vector<Ring> scan_rings()
    {
        std::vector<Ring> rings;
        size_t            startidx = 0;
        while ((startidx = search_start_cell(startidx)) < m_gridlen) {
            Ring ring;

            size_t idx  = startidx;
            Dir    next = next_dir(idx);
            do {
                // We should never touch a cell with no remaining directions
                // until we get back to the start cell.
                assert(next != Dir::none);
                Coord ringvertex{long(idx), long(next)};
                ring.emplace_back(ringvertex);
                clr_dirs(idx, next);
                idx  = stepidx(idx, next);
                next = next_dir(idx, next);
                assert(idx < m_gridlen);
            } while (idx != startidx);
            // The start cell on returning should either have its directions
            // cleared or have the second ambiguous direction.
            assert(next == Dir::none || next == Dir::left || next == Dir::up);
            if (ring.size() > 1)
                rings.emplace_back(ring);
        }
        return rings;
    }

    // Calculate the exact raster position from the cells which store the
    // sequential index of the square and the next direction
    template<class ExecutionPolicy> void interpolate_rings(ExecutionPolicy&& policy, std::vector<Ring>& rings, TRasterValue<Rst> isov)
    {
        for_each_idx(std::forward<ExecutionPolicy>(policy), rings.begin(), rings.end(), [this, isov](Ring& ring, size_t) {
            for (Coord& e : ring)
                interpolate_edge(e, isov);
        });
    }

    std::ostream& streamtags(std::ostream& os)
    {
        os << "   :";
        for (auto c = 0; c < m_gridsize.c; c++)
            os << (c % 10);
        os << "\n";
        for (auto r = 0; r < m_gridsize.r; r++) {
            os << std::setw(3) << r << ":";
            for (auto c = 0; c < m_gridsize.c; c++)
                os << ((get_tags9(seq(Coord(r, c))) & 1) ? "H" : ".");
            os << "\n";
        }
        return os;
    }

    std::ostream& streamdirs(std::ostream& os)
    {
        os << "   :";
        for (auto c = 0; c < m_gridsize.c; c++)
            os << (c % 10);
        os << "\n";
        for (auto r = 0; r < m_gridsize.r; r++) {
            os << std::setw(3) << r << ":";
            for (auto c = 0; c < m_gridsize.c; c++)
                os << get_dirs(seq(Coord(r, c)));
            os << std::endl;
        }
        return os;
    }
};

template<class Raster, class ExecutionPolicy>
std::vector<marchsq::Ring> execute_with_policy(ExecutionPolicy&&    policy,
                                               const Raster&        raster,
                                               TRasterValue<Raster> isoval,
                                               Coord                windowsize = {})
{
    if (!rows(raster) || !cols(raster))
        return {};

    size_t ratio = cols(raster) / rows(raster);

    if (!windowsize.r)
        windowsize.r = 2;
    if (!windowsize.c)
        windowsize.c = std::max(2l, long(windowsize.r * ratio));

    Grid<Raster> grid{raster, windowsize};

    grid.tag_grid(std::forward<ExecutionPolicy>(policy), isoval);
    std::vector<marchsq::Ring> rings = grid.scan_rings();
    grid.interpolate_rings(std::forward<ExecutionPolicy>(policy), rings, isoval);

    return rings;
}

template<class Raster> std::vector<marchsq::Ring> execute(const Raster& raster, TRasterValue<Raster> isoval, Coord windowsize = {})
{
    return execute_with_policy(nullptr, raster, isoval, windowsize);
}

} // namespace __impl

using __impl::execute_with_policy;
using __impl::execute;

} // namespace marchsq

#endif // MARCHINGSQUARES_HPP
