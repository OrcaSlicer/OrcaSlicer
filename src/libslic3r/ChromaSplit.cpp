#include "ChromaSplit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ChromaStack.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "TriangleMesh.hpp"
#include "libslic3r.h"

namespace Slic3r {

namespace {

// The height map on the grid the pieces are built from.
//
// `top` is in *relief levels*: how many printed layers above the base that cell's column reaches,
// 0 for a column that is bare base and relief_layers() for one that goes all the way up. A cell
// outside the picture's own silhouette - which only happens when the plate is not a rectangle -
// carries OUTSIDE instead, and is not part of any piece.
//
// The relief level, rather than the printed layer count the solve stores, because it is the one
// quantity that means the same thing both ways up: face up a column of r relief levels stands r
// layers proud of the base, face down the same column is a recess cut r layers short of the plate's
// own depth. The two orientations then differ only in where level zero sits and which way is up,
// which is what lets one mesher serve both.
//
// The corner coordinates are held rather than computed per cell so that every face that meets at a
// corner is built from the same two floats. Rounding one of them differently in two places is
// exactly how a mesh that is watertight on paper comes out with a crack in it.
struct GridField
{
    static constexpr int OUTSIDE = -1;

    int                nx = 0;
    int                ny = 0;
    std::vector<int>   top; // nx * ny, row-major, row 0 at the top of the picture
    // Where each cell's material starts, when it does not start at the piece's own floor. Empty for
    // every piece whose floor is one plane across the whole plate, which is every piece of the two
    // arrangements that keep the bands at one height; FlatStack is the one that fills it in, because
    // there a band sits at a different depth under every column.
    std::vector<int>   base;
    std::vector<float> xs;  // nx + 1 corner coordinates, increasing
    std::vector<float> ys;  // ny + 1 corner coordinates, decreasing - row 0 is at the picture's top
};

// One band's solid.
//
// The piece is a union of cell columns: cell (ix, iy) holds material from the band's floor up to
// whichever is lower, the column's own top or the top of the band. So one of the two ends is flat
// across the whole piece and the other is the terraced surface the picture makes, and which is which
// is the only thing that changes between face up and face down - hence `mirror`, which turns the
// finished piece over about z = `mirror_z` and reverses its winding with it.
//
// Levels are integers and heights come from them, never the other way round: `level_offset` is the
// printed layer the band's level zero sits at, `layer_height` is what one level is worth, and the
// band runs from level `k_lo` to level `k_hi`.
//
// `k_floor` is where the piece's bottom face goes, and it is one level *below* `k_lo` for the band
// that carries the solid part of the plate - the base face up, the backing face down - which prints
// in the bottom band's filament and is therefore part of its piece. That extra level is what keeps a
// column of bare base in the piece at all: it reaches no further than `k_lo`, so without somewhere
// below `k_lo` to stand it would be no column, and the darkest parts of the picture would come back
// as holes through the plate. It also has to be a level of its own rather than a lower floor, or the
// wall between a bare column and a raised one would start at the bottom of the base instead of at
// the top of it, which is a wall through the middle of the solid.
struct PieceSpec
{
    int    k_floor      = 0;
    int    k_lo         = 0;
    int    k_hi         = 0;
    int    level_offset = 0;
    double layer_height = 0.;
    double floor_z      = 0.;
    bool   mirror       = false;
    double mirror_z     = 0.;
};

// Where a corner's vertex at one level is pinched apart, and for whom.
//
// Two cells that meet only along a diagonal - the one above-left of a corner and the one
// below-right, with the other two empty - are a real thing for a solved picture to contain: dither
// scatters single texels of height, and a checkerboard of them touches corner to corner all over.
// The surface of that solid is not a manifold along the line where they meet: four walls run up it
// instead of two, so the edge belongs to four triangles and a boolean refuses the mesh.
//
// What is done about it is to give each of the two columns its own copy of the corner's vertices
// over the stretch where the pinch lasts. The edges then belong to two triangles each, because the
// two columns' walls no longer name the same vertex pair, and the two surfaces are separate sheets
// that happen to touch. The alternative - nudging the corners apart so they do not touch at all -
// would move the printed geometry to flatter a downstream algorithm, which is not this code's
// decision to make.
//
// Only the levels *above* the bottom of the pinch are copied. At the bottom level the surface is
// still one sheet (something fills at least one of the other two quadrants below there, or that
// level is the band's floor), and the vertical edges leaving it are already distinct by their upper
// end, so copying it would tear a seam that is not torn.
struct CornerPinch
{
    int lo    = 0;  // the last level the two columns are still one solid at
    int hi    = 0;  // the first level they are not pinched at any more
    int owner[2] = { -1, -1 }; // the two cells, as indices into the grid, that get a copy each
    bool valid = false;
};

class PieceBuilder
{
public:
    PieceBuilder(const GridField &grid, const PieceSpec &spec) : m_grid(grid), m_spec(spec) {}

    indexed_triangle_set build();

private:
    // The levels this cell holds material between, as a half-open span [lo, hi): equal means it holds
    // none. A cell off the grid, or outside the picture, holds none, which is what makes the walls at
    // the picture's own edge come out of the same rule as the walls around a hole.
    //
    // With no floor field the floor is the piece's own and every span starts there, which is the same
    // reading the one-plane arrangements always had: "present" is "reaches past the floor".
    void cell_span(int ix, int iy, int &lo, int &hi) const
    {
        lo = hi = m_spec.k_floor;
        if (ix < 0 || iy < 0 || ix >= m_grid.nx || iy >= m_grid.ny)
            return;
        const size_t at = size_t(iy) * size_t(m_grid.nx) + size_t(ix);
        const int    t  = m_grid.top[at];
        if (t == GridField::OUTSIDE)
            return;
        hi = std::clamp(t, m_spec.k_lo, m_spec.k_hi);
        if (!m_grid.base.empty())
            lo = std::min(std::clamp(m_grid.base[at], m_spec.k_lo, m_spec.k_hi), hi);
    }

    // The level the cell's material reaches, for everything that only asks how tall a column is.
    int cell_top(int ix, int iy) const
    {
        int lo = 0, hi = 0;
        cell_span(ix, iy, lo, hi);
        return hi;
    }

    // Face down the turn is taken here rather than on the finished vertices, so that a height is
    // rounded to a float once instead of twice. Mirroring a float that has already been rounded
    // leaves the plate's own surfaces a few nanometres off the numbers they are supposed to be at,
    // which is not enough to see and quite enough to stop two pieces meeting exactly.
    float level_z(int level) const
    {
        const double z = level <= m_spec.k_floor ? m_spec.floor_z
                                                 : double(m_spec.level_offset + level) * m_spec.layer_height;
        return float(m_spec.mirror ? m_spec.mirror_z - z : z);
    }

    CornerPinch pinch_at(int i, int j) const;

    // The vertex at corner (i, j) and `level`, as the face belonging to cell (ox, oy) sees it.
    uint32_t vertex(int i, int j, int level, int ox, int oy);

    void quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
    {
        m_its.indices.emplace_back(int32_t(a), int32_t(b), int32_t(c));
        m_its.indices.emplace_back(int32_t(a), int32_t(c), int32_t(d));
    }

    const GridField     &m_grid;
    const PieceSpec     &m_spec;
    indexed_triangle_set m_its;
    // Corner, level and which copy, to the vertex that already stands there.
    std::unordered_map<uint64_t, uint32_t> m_vertices;
};

CornerPinch PieceBuilder::pinch_at(int i, int j) const
{
    // The four cells round the corner, in a cycle: the corner is at the top-left of the last of
    // them, and the loop runs the other way round because row 0 of the grid is the top of the
    // picture. Which way round does not matter here; that they are in a cycle does, since a pinch is
    // two cells opposite each other in it.
    const int cx[4] = { i, i - 1, i - 1, i };
    const int cy[4] = { j - 1, j - 1, j, j };

    int t[4];
    for (int k = 0; k < 4; ++k)
        t[k] = cell_top(cx[k], cy[k]);

    CornerPinch out;
    for (int d = 0; d < 2; ++d) {
        // One diagonal is the pair that might be pinched, the other is the pair that has to be out
        // of the way for it to be: the pinch lasts from the higher of those two up to the lower of
        // the pair itself.
        const int lo = std::max(t[d + 1], t[(d + 3) % 4]);
        const int hi = std::min(t[d], t[d + 2]);
        if (lo >= hi)
            continue;
        out.valid    = true;
        out.lo       = lo;
        out.hi       = hi;
        out.owner[0] = cy[d] * m_grid.nx + cx[d];
        out.owner[1] = cy[d + 2] * m_grid.nx + cx[d + 2];
        break; // only one of the two diagonals can be the pinched one, since each needs the other down
    }
    return out;
}

uint32_t PieceBuilder::vertex(int i, int j, int level, int ox, int oy)
{
    uint32_t          side  = 0;
    const CornerPinch pinch = pinch_at(i, j);
    if (pinch.valid && level > pinch.lo && level <= pinch.hi) {
        const int owner = oy * m_grid.nx + ox;
        if (owner == pinch.owner[0])
            side = 1;
        else if (owner == pinch.owner[1])
            side = 2;
    }

    const uint64_t corner = uint64_t(j) * uint64_t(m_grid.nx + 1) + uint64_t(i);
    const uint64_t key    = (uint64_t(uint32_t(level - m_spec.k_floor)) << 34) | (uint64_t(side) << 32) | corner;
    const auto     found  = m_vertices.find(key);
    if (found != m_vertices.end())
        return found->second;

    const uint32_t index = uint32_t(m_its.vertices.size());
    m_its.vertices.emplace_back(m_grid.xs[size_t(i)], m_grid.ys[size_t(j)], level_z(level));
    m_vertices.emplace(key, index);
    return index;
}

indexed_triangle_set PieceBuilder::build()
{
    m_its.vertices.clear();
    m_its.indices.clear();
    if (m_spec.k_hi <= m_spec.k_lo || m_grid.nx <= 0 || m_grid.ny <= 0)
        return std::move(m_its);

    const int k_floor = m_spec.k_floor;

    for (int iy = 0; iy < m_grid.ny; ++iy)
        for (int ix = 0; ix < m_grid.nx; ++ix) {
            int f = 0, t = 0;
            cell_span(ix, iy, f, t);
            if (t <= f)
                continue; // the picture does not reach this band here

            // The floor, seen from underneath, so its corners run the opposite way round from the
            // top's. At this column's own floor rather than the piece's, which are the same plane in
            // every arrangement but FlatStack.
            quad(vertex(ix, iy, f, ix, iy), vertex(ix + 1, iy, f, ix, iy),
                 vertex(ix + 1, iy + 1, f, ix, iy), vertex(ix, iy + 1, f, ix, iy));

            // The terrace this column makes, at whatever level it stopped at. A column that carries
            // on up into the next band stops at the top of this one, and that face is then the one
            // the next piece's floor sits on.
            quad(vertex(ix, iy, t, ix, iy), vertex(ix, iy + 1, t, ix, iy), vertex(ix + 1, iy + 1, t, ix, iy),
                 vertex(ix + 1, iy, t, ix, iy));

            // The four walls. Each is built by the taller of the two cells it stands between, so it
            // is built once; a cell at the picture's edge, or beside a hole, reads its missing
            // neighbour as the floor and walls itself in.
            //
            // The corners are named so that (second - first) x up points away from this cell, which
            // is what makes every wall face outward. One quad per level rather than one for the
            // whole wall: a vertical edge that spans a single level is the thing that lets the
            // corners above pair up, since every wall that meets it is cut at the same heights.
            const int nx_[4]     = { ix + 1, ix - 1, ix, ix };
            const int ny_[4]     = { iy, iy, iy - 1, iy + 1 };
            const int first_i[4] = { ix + 1, ix, ix + 1, ix };
            const int first_j[4] = { iy + 1, iy, iy, iy + 1 };
            const int last_i[4]  = { ix + 1, ix, ix, ix + 1 };
            const int last_j[4]  = { iy, iy + 1, iy, iy + 1 };
            const auto wall = [&](int s, int from, int to) {
                for (int level = from; level < to; ++level)
                    quad(vertex(first_i[s], first_j[s], level, ix, iy),
                         vertex(last_i[s], last_j[s], level, ix, iy),
                         vertex(last_i[s], last_j[s], level + 1, ix, iy),
                         vertex(first_i[s], first_j[s], level + 1, ix, iy));
            };
            for (int s = 0; s < 4; ++s) {
                int fn = 0, tn = 0;
                cell_span(nx_[s], ny_[s], fn, tn);
                // Whatever of this column the neighbour is not backing onto, which with one floor
                // across the piece is only ever the part standing above it. The two stretches cannot
                // overlap, so each level of each wall is built once, by whichever of the two cells
                // has material there.
                wall(s, f, std::min(t, fn));
                wall(s, std::max(f, tn), t);
            }
        }

    if (m_spec.mirror)
        // Face down: the piece was built with its levels counted down into the plate and level_z()
        // turned every one of them over, which leaves the triangles wound the wrong way round - a
        // reflection reverses them. Swapping two corners of each puts the outward side back out.
        // Building it this way up and turning it over is what keeps there from being a second mesher
        // to hold in step with this one.
        for (Vec3i32 &t : m_its.indices)
            std::swap(t.y(), t.z());
    return std::move(m_its);
}

// The grid, the height map resampled onto it, and the picture's silhouette cut out of it.
GridField build_grid(const ChromaStack &stack, const ChromaSolve &solve, const ChromaSplitOptions &options)
{
    GridField grid;
    const int base   = std::max(0, stack.base_layers);
    const int relief = stack.relief_layers();

    // Coarsened until the grid fits the budget, by whole texels: a cell is a texel or a square block
    // of them, and the block's middle texel is the one that decides its height. Averaging the block
    // would put the terrace between two layer boundaries, which is the one thing the quantization
    // exists to prevent.
    const int max_cells = std::max(1, options.max_cells);
    int       step      = 1;
    while (size_t((solve.width + step - 1) / step) * size_t((solve.height + step - 1) / step) > size_t(max_cells))
        ++step;

    grid.nx = (solve.width + step - 1) / step;
    grid.ny = (solve.height + step - 1) / step;
    grid.top.resize(size_t(grid.nx) * size_t(grid.ny));
    for (int iy = 0; iy < grid.ny; ++iy) {
        const int sy = std::min(iy * step + step / 2, solve.height - 1);
        for (int ix = 0; ix < grid.nx; ++ix) {
            const int sx    = std::min(ix * step + step / 2, solve.width - 1);
            const int level = int(solve.levels[size_t(sy) * size_t(solve.width) + size_t(sx)]);
            grid.top[size_t(iy) * size_t(grid.nx) + size_t(ix)] = std::clamp(level - base, 0, relief);
        }
    }

    // The same rectangle chroma_plate_mesh() puts the picture on, and the same clamp on its sides, so
    // that a piece lands exactly where the plate it came out of does.
    const double w = double(std::max(0.1f, stack.plate.width_mm));
    const double h = double(std::max(0.1f, stack.plate.height_mm));
    grid.xs.resize(size_t(grid.nx) + 1);
    grid.ys.resize(size_t(grid.ny) + 1);
    for (int i = 0; i <= grid.nx; ++i)
        grid.xs[size_t(i)] = float(-0.5 * w + w * double(i) / double(grid.nx));
    // Row 0 of the height map is the top of the picture: the texture's v runs down the image while y
    // runs up the plate (see box_plane_xy() in TextureDisplacement.cpp, which is what a ChromaStack
    // layer samples through). Getting this the wrong way round would hand back a picture flipped
    // against the plate the preview drew.
    for (int j = 0; j <= grid.ny; ++j)
        grid.ys[size_t(j)] = float(0.5 * h - h * double(j) / double(grid.ny));

    // A plate that is not a rectangle - an oval, or a silhouette cut from an SVG - carries a picture
    // that is not one either, and the cells outside it are not part of the plate. The cut lands on
    // cell boundaries, so the silhouette's edge comes out stepped at the grid's own resolution;
    // there is no finer answer available while the piece is built from cells.
    if (stack.plate.frame.shape != ChromaFrame::Shape::Rectangle) {
        const std::vector<Vec2f> loop = chroma_frame_outline(stack.plate, 0.f);
        if (loop.size() >= 3) {
            Polygon outline;
            outline.points.reserve(loop.size());
            for (const Vec2f &p : loop)
                outline.points.emplace_back(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
            for (int iy = 0; iy < grid.ny; ++iy)
                for (int ix = 0; ix < grid.nx; ++ix) {
                    const float cx = 0.5f * (grid.xs[size_t(ix)] + grid.xs[size_t(ix) + 1]);
                    const float cy = 0.5f * (grid.ys[size_t(iy)] + grid.ys[size_t(iy) + 1]);
                    if (!outline.contains(Point(scaled<coord_t>(cx), scaled<coord_t>(cy))))
                        grid.top[size_t(iy) * size_t(grid.nx) + size_t(ix)] = GridField::OUTSIDE;
                }
        }
    }
    return grid;
}

} // namespace

ChromaSplitPieces chroma_split_bands(const ChromaStack &stack, const ChromaSolve &solve,
                                     const ChromaSplitOptions &options)
{
    ChromaSplitPieces out;
    if (!stack.plate.generated || stack.bands.empty() || stack.layer_height_mm <= 0.f)
        return out;
    if (solve.empty() || solve.levels.size() != size_t(solve.width) * size_t(solve.height))
        return out;

    const GridField grid = build_grid(stack, solve, options);
    if (grid.nx <= 0 || grid.ny <= 0)
        return out;

    out.grid_width  = grid.nx;
    out.grid_height = grid.ny;
    out.cell_x_mm   = grid.xs[1] - grid.xs[0];
    out.cell_y_mm   = grid.ys[0] - grid.ys[1];
    out.frame_omitted = stack.plate.frame.width_mm > 0.f;

    // Where each band starts and stops, in relief levels. A band of no layers is a band that occupies
    // nothing, and the piece it gets is empty rather than flat.
    const size_t     n = stack.bands.size();
    std::vector<int> k_lo(n, 0), k_hi(n, 0);
    int              at = 0;
    int              base_band = -1;
    for (size_t i = 0; i < n; ++i) {
        k_lo[i] = at;
        at += std::max(0, stack.bands[i].layers);
        k_hi[i] = at;
        if (base_band < 0 && k_hi[i] > k_lo[i])
            base_band = int(i);
    }
    const int    relief = at;
    const double layer  = double(stack.layer_height_mm);
    const bool   down   = stack.plate.orientation == ChromaOrientation::FaceDown;
    const bool   flat_stack = options.flat_mode == ChromaFlatMode::FlatStack;
    // Both of the other two put the fill behind the picture; only where the bands go differs.
    const bool   behind     = options.flat_mode != ChromaFlatMode::ClearInFront;

    // The plate's full thickness, and what is left of it once the relief has taken its share. Face up
    // the plate is as thick as its layers make it, so both are exact multiples of the layer height;
    // face down the thickness is a dimension of the plate the picture brought with it and the backing
    // is whatever remains. Either way the plate occupies z in [0, thickness], with the picture's face
    // at z = 0 face down and at z = thickness face up.
    const double thickness = down ? double(std::max(0.2f, stack.plate.base_mm)) :
                                    double(std::max(0, stack.base_layers) + relief) * layer;
    const double backing   = std::max(0., thickness - double(relief) * layer);

    out.bands.resize(n);
    for (size_t i = 0; i < n; ++i) {
        PieceSpec spec;
        spec.k_lo         = k_lo[i];
        spec.k_hi         = k_hi[i];
        spec.k_floor      = k_lo[i];
        spec.layer_height = layer;
        if (flat_stack) {
            // Every column's own sequence slid up against the flat face. At a column of L layers this
            // band lies between L - min(L, k_hi) and L - k_lo layers under the face, so both ends of
            // it move with the column and the field carries a floor as well as a top. A column the
            // band does not reach gets a span of no height and no geometry, exactly as before.
            //
            // The band nearest the face is therefore a different band under different columns, which
            // is the whole of what this mode is: the colours a column shows are the ones the terraced
            // plate would have shown it, in the order it would have shown them.
            GridField field = grid;
            field.base.assign(field.top.size(), 0);
            for (size_t c = 0; c < field.top.size(); ++c) {
                const int lv = grid.top[c];
                if (lv == GridField::OUTSIDE)
                    continue;
                field.top[c]  = std::max(0, lv - k_lo[i]);
                field.base[c] = std::max(0, lv - std::min(lv, k_hi[i]));
            }
            spec.k_lo         = 0;
            spec.k_hi         = relief;
            spec.k_floor      = 0;
            spec.level_offset = 0;
            spec.floor_z      = 0.;
            spec.mirror       = !down;
            spec.mirror_z     = thickness;
            PieceBuilder flat_builder(field, spec);
            out.bands[i] = flat_builder.build();
            continue;
        }
        if (behind) {
            // Hung from the plate's own face, in the bands' own order: band zero is the one the light
            // meets first, and every band after it is further in. The terrace is then at the band's
            // far edge rather than its near one, which is the only thing the mesher needs told, since
            // the face it is measured from is the one it is hung from.
            //
            // No band carries the plate's solid part here. It belongs to the fill, which is the piece
            // it is continuous with: the fill reaches the face wherever the picture stops short, and
            // the backing is that same solid carried on behind the picture everywhere else.
            spec.level_offset = 0;
            spec.floor_z      = double(spec.k_lo) * layer;
            spec.mirror       = !down;
            spec.mirror_z     = thickness;
        } else if (down) {
            // Built in the plate's own depth: level zero at the underside of the backing, growing
            // down into the plate, and turned over at the end. The picture's face is then the first
            // layer printed and the backing is on top, which is what this mode prints, and the bands
            // come out in the reverse order down the plate without any of that being said twice.
            spec.level_offset = 0;
            spec.floor_z      = double(spec.k_lo) * layer;
            spec.mirror       = true;
            spec.mirror_z     = double(relief) * layer;
            if (int(i) == base_band && backing > 0.) {
                spec.k_floor = spec.k_lo - 1;
                spec.floor_z = -backing;
            }
        } else {
            spec.level_offset = std::max(0, stack.base_layers);
            spec.floor_z      = double(spec.level_offset + spec.k_lo) * layer;
            if (int(i) == base_band && spec.level_offset > 0) {
                spec.k_floor = spec.k_lo - 1;
                spec.floor_z = 0.;
            }
        }
        PieceBuilder builder(grid, spec);
        out.bands[i] = builder.build();
    }

    // ---- The fill ----
    //
    // The complement of the picture inside the relief, which is a height field like the picture: a
    // column of r levels leaves relief - r of empty space. Both arrangements fill the same space, and
    // differ only in which end of the plate it is against, so both read the same inverted field - and
    // because it is the same mesher reading the same corner coordinates, the terrace where the fill
    // meets a band is built from the same two floats the band built its own terrace from.
    //
    // Heights agree to the last bit as well as to the eye, which is what lets `bands` and `fill` be
    // handed to a boolean. The band puts a terrace at mirror_z - r*layer and the fill puts its own at
    // (relief - r)*layer: layer is a float and the level counts are small integers, so every one of
    // those products and differences is exact in double and the two land on the same float.
    GridField hollow = grid;
    for (int &t : hollow.top)
        if (t != GridField::OUTSIDE)
            t = relief - t;

    PieceSpec fill_spec;
    fill_spec.k_lo         = 0;
    fill_spec.k_hi         = relief;
    fill_spec.k_floor      = 0;
    fill_spec.level_offset = 0;
    fill_spec.floor_z      = 0.;
    fill_spec.layer_height = layer;
    if (behind) {
        // One solid from each column's own terrace to the plate's far face: the complement of the
        // picture, and the backing it runs into, which are the same body and not two.
        //
        // The floor is a level of its own below the complement's zero, exactly as the base band's is
        // in the other arrangement and for the same reason: a column where the picture reaches the
        // whole way still has the backing behind it, and without a level below zero to stand on it
        // would be no column at all, leaving a hole through the plate where the picture is deepest.
        // With no backing there is nothing behind such a column, and the extra level would be a
        // column of no height.
        if (backing > 0.)
            fill_spec.k_floor = -1;
        if (down) {
            // The void is above each terrace, so the piece is built downward from the complement and
            // turned over: the floor at -backing lands at the plate's far face, and a complement of
            // h lands at the terrace, (relief - h) layers up from the picture.
            fill_spec.floor_z  = -backing;
            fill_spec.mirror   = true;
            fill_spec.mirror_z = double(relief) * layer;
        } else {
            // Face up the far face is the bottom, so the piece stands on z = 0 and its terraced top
            // is the complement carried up over the backing - which is what level_offset is for, and
            // why face up needs no turning here.
            fill_spec.level_offset = std::max(0, stack.base_layers);
            fill_spec.floor_z      = 0.;
            fill_spec.mirror       = false;
        }
    } else if (down) {
        // The bands were built counting down into the plate and turned over at the end, which leaves
        // the void where it already is: against the build plate, growing up to the picture's face. So
        // the fill is built the right way up to begin with and needs no turning.
        fill_spec.mirror = false;
    } else {
        // Face up the void is at the top, so it is built hanging from the plate's own top face.
        fill_spec.mirror   = true;
        fill_spec.mirror_z = thickness;
    }
    PieceBuilder fill_builder(hollow, fill_spec);
    out.fill = fill_builder.build();
    return out;
}

} // namespace Slic3r
