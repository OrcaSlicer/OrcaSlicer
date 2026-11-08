#pragma once

#include <string>
#include <vector>

#include "Point.hpp"
#include "BoundingBox.hpp"

namespace Slic3r {

// A probed bed height map as the printer firmware currently holds it: Z offsets on a regular
// grid spanning [min, max] in printer coordinates. Adaptive meshes (KAMP, Klipper's ADAPTIVE=1)
// cover only the print area, so the grid is often a small rectangle of the bed; whatever lies
// outside it is unknown to us (the firmware clamps to the edge value there).
struct BedMesh
{
    std::string profile_name;
    // Human-readable origin of the data, e.g. "Moonraker", "RepRapFirmware heightmap.csv",
    // "OctoPrint Bed Level Visualizer". Shown in the legend.
    std::string source;
    // Free-text detail worth showing next to the source, e.g. when a stored copy was captured.
    std::string note;
    // False when the printer is not compensating with this mesh: a saved Klipper profile that is
    // not loaded (Klipper drops the active mesh on restart), an RRF height map file that is not
    // loaded, or a copy stored by an OctoPrint plugin.
    bool loaded{ true };
    // Corners of the grid in printer coordinates.
    Vec2d min{ 0.0, 0.0 };
    Vec2d max{ 0.0, 0.0 };
    // Raw probe results: rows run along Y (row 0 at min.y), columns along X (column 0 at min.x).
    // A node the firmware did not probe (outside a delta's radius, a UBL cell left at ".") is
    // NaN: it is skipped by the statistics and not drawn.
    std::vector<std::vector<float>> probed;
    // The firmware-interpolated matrix it actually compensates with, same layout, finer pitch.
    // May be empty when the firmware reports only probed points.
    std::vector<std::vector<float>> mesh;

    bool empty() const { return probed.empty() || probed.front().empty(); }
    // Prefer the interpolated matrix for drawing; fall back to the probed one.
    const std::vector<std::vector<float>>& surface() const { return mesh.empty() ? probed : mesh; }

    size_t probed_rows() const { return probed.size(); }
    size_t probed_cols() const { return probed.empty() ? 0 : probed.front().size(); }
    // Nodes that were actually probed (NaN nodes excluded).
    size_t probed_count() const;
    static bool is_probed(float z) { return z == z; }

    float z_min() const;
    float z_max() const;
    // Peak to peak: how far the bed deviates from flat, which is what the reader wants to know.
    float z_range() const { return empty() ? 0.f : z_max() - z_min(); }
    // Beyond this much out of flat, mesh compensation is papering over a mechanical problem
    // (tramming, a warped plate) and the user is told so.
    static constexpr float FLATNESS_WARNING_MM = 0.2f;
    bool out_of_flat() const { return z_range() > FLATNESS_WARNING_MM; }
    BoundingBoxf bounding_box() const { return BoundingBoxf(min, max); }

    // Printer coordinates of grid node (row, col) of a matrix with the given dimensions.
    Vec2d node_position(size_t row, size_t col, size_t rows, size_t cols) const;

    // Fraction of the bed's X and Y span the mesh covers.
    Vec2d coverage(const BoundingBoxf& bed) const;
    // Heuristic: an adaptive mesh spans well under the bed on at least one axis, whereas a full
    // mesh only loses the probe offset at the edges. The threshold is 70% of the bed span.
    bool is_partial(const BoundingBoxf& bed) const;

    // Parse the reply to Moonraker's `GET /printer/objects/query?bed_mesh`. Accepts the whole
    // response envelope or just the `bed_mesh` object. With no active mesh it falls back to the
    // saved profile named "default", else the first saved profile, with `loaded` false. Returns
    // true with an empty mesh when there is neither; false with `error` set on a malformed reply
    // or an API error.
    static bool from_moonraker_json(const std::string& body, BedMesh& out, std::string& error);

    // Parse a RepRapFirmware `heightmap.csv` (the v2 layout written by RRF 3, and the v1 layout
    // of RRF 2). Rows run from min Y upwards, as in the file. Points RRF did not probe are
    // written as 0 and stay 0 here. `loaded` is left true; the caller knows whether the file
    // is the one the firmware has loaded.
    static bool from_rrf_heightmap_csv(const std::string& text, BedMesh& out, std::string& error);

    // Parse the copy of the last mesh that OctoPrint's Bed Level Visualizer plugin stores in its
    // settings (the `plugins.bedlevelvisualizer` object of `GET /api/settings`, or the whole
    // reply). The plugin's browser front end writes `stored_mesh`, `stored_mesh_x`,
    // `stored_mesh_y` and `mesh_timestamp` when it renders a mesh, so the copy is only as fresh
    // as the last time the web UI processed one. Positions are the plugin's own bed bounds,
    // rounded to whole mm. `loaded` is false. Returns true with an empty mesh when the plugin
    // is present but has stored nothing yet; false with `error` set when it is not installed
    // or the data is malformed.
    static bool from_bedlevelvisualizer_json(const std::string& body, BedMesh& out, std::string& error);

    // Parse the message the Bed Level Visualizer plugin pushes on OctoPrint's socket after it has
    // collected a mesh: {"mesh": [[...]], "bed": {"type", "x_min", "x_max", "y_min", "y_max"}},
    // either bare or wrapped as {"plugin": "bedlevelvisualizer", "data": {...}}. Positions come
    // from the plugin's bed settings. `loaded` is true: the firmware just reported this mesh.
    static bool from_bedlevelvisualizer_message(const std::string& body, BedMesh& out, std::string& error);

    // Parse the grid Marlin prints for `M420 V` ("Bilinear Leveling Grid:", "Mesh Bed Level
    // data:") or `G29 T` (UBL "Bed Topography Report"), given the terminal lines with any
    // "Recv:" prefix already removed. Marlin prints no positions for a bilinear grid, so the
    // grid is stretched over `assumed_bounds` (the printer profile's bed) and `note` says so; a
    // UBL report carries corner coordinates and those are used instead. A subdivided
    // (Catmull-Rom) grid following the probed one becomes `mesh`. Returns false with `error`
    // set when the lines hold no grid.
    static bool from_marlin_report(const std::vector<std::string>& lines, const BoundingBoxf& assumed_bounds, BedMesh& out, std::string& error);
};

} // namespace Slic3r
