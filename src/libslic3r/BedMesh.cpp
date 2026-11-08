#include "BedMesh.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

using json = nlohmann::json;

static void matrix_range(const std::vector<std::vector<float>>& m, float& lo, float& hi)
{
    for (const auto& row : m)
        for (float v : row) {
            if (!BedMesh::is_probed(v))
                continue;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
}

size_t BedMesh::probed_count() const
{
    size_t n = 0;
    for (const auto& row : probed)
        for (float v : row)
            n += is_probed(v) ? 1 : 0;
    return n;
}

float BedMesh::z_min() const
{
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    matrix_range(probed, lo, hi);
    matrix_range(mesh, lo, hi);
    return lo <= hi ? lo : 0.f;
}

float BedMesh::z_max() const
{
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    matrix_range(probed, lo, hi);
    matrix_range(mesh, lo, hi);
    return lo <= hi ? hi : 0.f;
}

Vec2d BedMesh::node_position(size_t row, size_t col, size_t rows, size_t cols) const
{
    const double x = cols > 1 ? min.x() + (max.x() - min.x()) * double(col) / double(cols - 1) : min.x();
    const double y = rows > 1 ? min.y() + (max.y() - min.y()) * double(row) / double(rows - 1) : min.y();
    return { x, y };
}

Vec2d BedMesh::coverage(const BoundingBoxf& bed) const
{
    if (empty() || !bed.defined)
        return { 0.0, 0.0 };
    const Vec2d bed_size = bed.size();
    const Vec2d size     = max - min;
    return { bed_size.x() > 0.0 ? std::clamp(size.x() / bed_size.x(), 0.0, 1.0) : 0.0,
             bed_size.y() > 0.0 ? std::clamp(size.y() / bed_size.y(), 0.0, 1.0) : 0.0 };
}

bool BedMesh::is_partial(const BoundingBoxf& bed) const
{
    const Vec2d c = coverage(bed);
    return c.x() < 0.7 || c.y() < 0.7;
}

// A number, or a string holding one (OctoPrint's plugin stores what Marlin printed).
static bool json_number(const json& v, float& out)
{
    if (v.is_number()) {
        out = v.get<float>();
        return true;
    }
    if (v.is_string()) {
        try {
            size_t      consumed = 0;
            const auto& s        = v.get_ref<const std::string&>();
            out                  = std::stof(s, &consumed);
            return consumed > 0;
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

// Reads a matrix (list of rows). An absent key, an empty list or a list holding only an empty
// row all mean "no data" and leave `dst` empty. Ragged rows are an error.
static bool read_matrix(const json& obj, const char* key, std::vector<std::vector<float>>& dst, std::string& error)
{
    dst.clear();
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_array())
        return true;
    for (const json& row : *it) {
        if (!row.is_array()) {
            error = std::string(key) + ": row is not an array";
            return false;
        }
        std::vector<float> values;
        values.reserve(row.size());
        for (const json& v : row) {
            float f;
            if (!json_number(v, f)) {
                error = std::string(key) + ": value is not a number";
                return false;
            }
            values.push_back(f);
        }
        dst.push_back(std::move(values));
    }
    if (!dst.empty() && dst.front().empty())
        dst.clear();
    for (const auto& row : dst)
        if (row.size() != dst.front().size()) {
            error = std::string(key) + ": rows have different lengths";
            dst.clear();
            return false;
        }
    return true;
}

static bool read_point(const json& obj, const char* key, Vec2d& dst, std::string& error)
{
    auto it = obj.find(key);
    if (it == obj.end())
        return true;
    if (!it->is_array() || it->size() < 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) {
        error = std::string(key) + ": expected [x, y]";
        return false;
    }
    dst = { (*it)[0].get<double>(), (*it)[1].get<double>() };
    return true;
}

bool BedMesh::from_moonraker_json(const std::string& body, BedMesh& out, std::string& error)
{
    json root;
    try {
        root = json::parse(body);
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
    if (!root.is_object()) {
        error = "reply is not a JSON object";
        return false;
    }
    if (auto err = root.find("error"); err != root.end()) {
        error = err->is_object() ? err->value("message", std::string("unknown error")) : err->dump();
        return false;
    }

    // Unwrap {"result": {"status": {"bed_mesh": {...}}}} down to the bed_mesh object.
    const json* bm = &root;
    for (const char* key : { "result", "status", "bed_mesh" })
        if (auto it = bm->find(key); it != bm->end() && it->is_object())
            bm = &*it;
    if (bm->find("probed_matrix") == bm->end() && bm->find("mesh_matrix") == bm->end()) {
        error = "reply carries no bed_mesh object";
        return false;
    }

    BedMesh m;
    m.source       = "Moonraker";
    m.profile_name = bm->value("profile_name", std::string());
    if (!read_point(*bm, "mesh_min", m.min, error) || !read_point(*bm, "mesh_max", m.max, error))
        return false;
    if (!read_matrix(*bm, "probed_matrix", m.probed, error) || !read_matrix(*bm, "mesh_matrix", m.mesh, error))
        return false;
    // A firmware that reports only the interpolated matrix still gives us something to draw.
    if (m.probed.empty() && !m.mesh.empty())
        m.probed = m.mesh;
    // No active mesh: fall back to a saved profile so a mesh that only needs loading still shows.
    if (m.probed.empty()) {
        if (auto profiles = bm->find("profiles"); profiles != bm->end() && profiles->is_object() && !profiles->empty()) {
            auto it = profiles->find("default");
            if (it == profiles->end())
                it = profiles->begin();
            const json& profile = it.value();
            if (profile.is_object() && read_matrix(profile, "points", m.probed, error) && !m.probed.empty()) {
                auto params = profile.find("mesh_params");
                if (params == profile.end() || !params->is_object()) {
                    error = "saved profile has no mesh_params";
                    return false;
                }
                for (const char* key : { "min_x", "max_x", "min_y", "max_y" })
                    if (!params->contains(key) || !(*params)[key].is_number()) {
                        error = std::string("saved profile mesh_params lacks ") + key;
                        return false;
                    }
                m.min          = { (*params)["min_x"].get<double>(), (*params)["min_y"].get<double>() };
                m.max          = { (*params)["max_x"].get<double>(), (*params)["max_y"].get<double>() };
                m.profile_name = it.key();
                m.loaded       = false;
            }
            else if (!error.empty())
                return false;
        }
    }
    if (!m.empty() && (m.max.x() <= m.min.x() || m.max.y() <= m.min.y())) {
        error = "mesh_min/mesh_max do not describe a rectangle";
        return false;
    }
    if (m.empty()) {
        // Nothing loaded: Klipper reports [[]] with a zero-size rectangle.
        m.mesh.clear();
        m.min = m.max = Vec2d(0.0, 0.0);
    }
    out = std::move(m);
    return true;
}

// Splits a CSV line on commas, trimming each field.
static std::vector<std::string> csv_fields(const std::string& line)
{
    std::vector<std::string> out;
    std::stringstream        ss(line);
    std::string              field;
    while (std::getline(ss, field, ',')) {
        boost::algorithm::trim(field);
        out.push_back(field);
    }
    return out;
}

bool BedMesh::from_rrf_heightmap_csv(const std::string& text, BedMesh& out, std::string& error)
{
    // Line 1: "RepRapFirmware height map file v2, min error ..." (v1 says "v1").
    // Line 2: column names. Line 3: their values. Then num1 rows of num0 values.
    //   v2: axis0,axis1,min0,max0,min1,max1,radius,spacing0,spacing1,num0,num1
    //   v1: xmin,xmax,ymin,ymax,radius,xspacing,yspacing,xnum,ynum
    std::vector<std::string> lines;
    {
        std::stringstream ss(text);
        std::string       line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!boost::algorithm::trim_copy(line).empty())
                lines.push_back(line);
        }
    }
    if (lines.size() < 3 || !boost::algorithm::istarts_with(lines[0], "RepRapFirmware height map")) {
        error = "not a RepRapFirmware height map file";
        return false;
    }
    const std::vector<std::string> names  = csv_fields(lines[1]);
    const std::vector<std::string> values = csv_fields(lines[2]);
    if (names.size() != values.size() || names.empty()) {
        error = "height map header and values do not match";
        return false;
    }
    std::map<std::string, std::string> header;
    for (size_t i = 0; i < names.size(); ++i)
        header[names[i]] = values[i];

    auto get = [&](const char* v2, const char* v1, double& dst) -> bool {
        auto it = header.find(v2);
        if (it == header.end())
            it = header.find(v1);
        if (it == header.end())
            return false;
        try {
            dst = std::stod(it->second);
        } catch (const std::exception&) {
            return false;
        }
        return true;
    };

    BedMesh m;
    m.source = "RepRapFirmware heightmap.csv";
    double min_x, max_x, min_y, max_y, num_x, num_y;
    if (!get("min0", "xmin", min_x) || !get("max0", "xmax", max_x) || !get("min1", "ymin", min_y) || !get("max1", "ymax", max_y) ||
        !get("num0", "xnum", num_x) || !get("num1", "ynum", num_y)) {
        error = "height map header lacks the grid bounds or counts";
        return false;
    }
    if (auto it = header.find("axis0"); it != header.end() && it->second != "X") {
        error = "height map is not over the X/Y axes (axis0 = " + it->second + ")";
        return false;
    }
    // RRF probes at min + i * spacing and writes the configured max, which the spacing need not
    // divide; the grid's real extent is what the spacing says.
    double spacing_x = 0.0, spacing_y = 0.0, radius = -1.0;
    get("spacing0", "xspacing", spacing_x);
    get("spacing1", "yspacing", spacing_y);
    get("radius", "radius", radius);
    const size_t cols = size_t(std::lround(num_x)), rows = size_t(std::lround(num_y));
    if (spacing_x > 0.0 && cols > 1)
        max_x = min_x + spacing_x * double(cols - 1);
    if (spacing_y > 0.0 && rows > 1)
        max_y = min_y + spacing_y * double(rows - 1);
    m.min = { min_x, min_y };
    m.max = { max_x, max_y };
    if (cols == 0 || rows == 0 || lines.size() < 3 + rows) {
        error = "height map has fewer rows than its header announces";
        return false;
    }
    for (size_t r = 0; r < rows; ++r) {
        const std::vector<std::string> fields = csv_fields(lines[3 + r]);
        if (fields.size() != cols) {
            error = "height map row " + std::to_string(r + 1) + " has " + std::to_string(fields.size()) + " values, expected " + std::to_string(cols);
            return false;
        }
        std::vector<float> row;
        row.reserve(cols);
        for (const std::string& f : fields) {
            try {
                row.push_back(std::stof(f));
            } catch (const std::exception&) {
                error = "height map holds a non-numeric value: " + f;
                return false;
            }
        }
        m.probed.push_back(std::move(row));
    }
    if (m.max.x() <= m.min.x() || m.max.y() <= m.min.y()) {
        error = "height map bounds do not describe a rectangle";
        return false;
    }
    // A delta probes a circle about the origin; RRF writes the rectangle's corners outside it as 0.
    if (radius > 0.0)
        for (size_t r = 0; r < rows; ++r)
            for (size_t c = 0; c < cols; ++c)
                if (m.node_position(r, c, rows, cols).norm() > radius + 1e-6)
                    m.probed[r][c] = std::numeric_limits<float>::quiet_NaN();
    out = std::move(m);
    return true;
}

bool BedMesh::from_bedlevelvisualizer_json(const std::string& body, BedMesh& out, std::string& error)
{
    json root;
    try {
        root = json::parse(body);
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
    if (!root.is_object()) {
        error = "reply is not a JSON object";
        return false;
    }
    const json* plugin = &root;
    if (auto plugins = root.find("plugins"); plugins != root.end() && plugins->is_object()) {
        auto it = plugins->find("bedlevelvisualizer");
        if (it == plugins->end() || !it->is_object()) {
            error = "the Bed Level Visualizer plugin is not installed (no plugins.bedlevelvisualizer in /api/settings)";
            return false;
        }
        plugin = &*it;
    }
    else if (root.find("stored_mesh") == root.end()) {
        error = "reply carries no Bed Level Visualizer settings";
        return false;
    }

    BedMesh m;
    m.source = "OctoPrint Bed Level Visualizer";
    m.loaded = false;
    if (!read_matrix(*plugin, "stored_mesh", m.probed, error))
        return false;
    if (m.probed.empty()) {
        out = std::move(m);
        return true;
    }

    auto read_axis = [&](const char* key, std::vector<double>& dst) -> bool {
        dst.clear();
        auto it = plugin->find(key);
        if (it == plugin->end() || !it->is_array())
            return false;
        for (const json& v : *it) {
            float f;
            if (!json_number(v, f))
                return false;
            dst.push_back(f);
        }
        return !dst.empty();
    };
    std::vector<double> xs, ys;
    if (!read_axis("stored_mesh_x", xs) || !read_axis("stored_mesh_y", ys)) {
        error = "stored_mesh_x/stored_mesh_y are missing: the plugin's web UI has not stored a mesh with positions";
        return false;
    }
    if (xs.size() != m.probed_cols() || ys.size() != m.probed_rows()) {
        error = "stored_mesh is " + std::to_string(m.probed_cols()) + " x " + std::to_string(m.probed_rows()) + " but the position arrays are " +
                std::to_string(xs.size()) + " x " + std::to_string(ys.size());
        return false;
    }
    // Normalise to rows from min Y upwards and columns from min X rightwards.
    if (xs.front() > xs.back()) {
        std::reverse(xs.begin(), xs.end());
        for (auto& row : m.probed)
            std::reverse(row.begin(), row.end());
    }
    if (ys.front() > ys.back()) {
        std::reverse(ys.begin(), ys.end());
        std::reverse(m.probed.begin(), m.probed.end());
    }
    m.min = { xs.front(), ys.front() };
    m.max = { xs.back(), ys.back() };
    if (xs.size() > 1 && m.max.x() <= m.min.x()) {
        error = "stored_mesh_x does not span a range";
        return false;
    }
    if (ys.size() > 1 && m.max.y() <= m.min.y()) {
        error = "stored_mesh_y does not span a range";
        return false;
    }
    const std::string stamp = plugin->value("mesh_timestamp", std::string());
    if (!stamp.empty())
        m.note = "captured " + stamp;
    out = std::move(m);
    return true;
}

bool BedMesh::from_bedlevelvisualizer_message(const std::string& body, BedMesh& out, std::string& error)
{
    json root;
    try {
        root = json::parse(body);
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
    const json* data = &root;
    if (root.is_object() && root.contains("data") && root["data"].is_object())
        data = &root["data"];
    if (!data->is_object() || data->find("mesh") == data->end()) {
        error = "plugin message carries no mesh";
        return false;
    }
    BedMesh m;
    m.source = "OctoPrint Bed Level Visualizer";
    if (!read_matrix(*data, "mesh", m.probed, error))
        return false;
    if (m.probed.empty()) {
        error = "plugin message holds an empty mesh";
        return false;
    }
    auto bed = data->find("bed");
    if (bed == data->end() || !bed->is_object()) {
        error = "plugin message carries no bed bounds";
        return false;
    }
    float x_min, x_max, y_min, y_max;
    if (!json_number(bed->value("x_min", json()), x_min) || !json_number(bed->value("x_max", json()), x_max) ||
        !json_number(bed->value("y_min", json()), y_min) || !json_number(bed->value("y_max", json()), y_max)) {
        error = "plugin message bed bounds are incomplete";
        return false;
    }
    if (x_max <= x_min || y_max <= y_min) {
        error = "plugin message bed bounds do not describe a rectangle";
        return false;
    }
    m.min  = { x_min, y_min };
    m.max  = { x_max, y_max };
    m.note = "positions from the plugin's bed settings";
    out    = std::move(m);
    return true;
}

namespace {

// Floats with an optional sign, as Marlin prints them; "." and "NAN" stand for unprobed points.
bool parse_grid_value(const std::string& token, float& out)
{
    if (token == "." || boost::algorithm::iequals(token, "nan")) {
        out = std::numeric_limits<float>::quiet_NaN();
        return true;
    }
    try {
        size_t consumed = 0;
        out             = std::stof(token, &consumed);
        return consumed == token.size();
    } catch (const std::exception&) {
        return false;
    }
}

// Splits a report line into tokens on whitespace, '|' (UBL draws a border with it) and '[' ']'
// (UBL brackets the point nearest the nozzle).
std::vector<std::string> grid_tokens(const std::string& line)
{
    std::vector<std::string> out;
    std::string              cur;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '|' || c == ',' || c == '[' || c == ']') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        }
        else
            cur.push_back(c);
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

bool is_integer(const std::string& s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

// A grid row: an optional leading row index, then at least two values. Returns the index or -1.
bool parse_grid_row(const std::string& line, std::vector<float>& values, int& index)
{
    values.clear();
    index = -1;
    std::vector<std::string> tokens = grid_tokens(line);
    if (tokens.empty())
        return false;
    size_t first = 0;
    if (is_integer(tokens[0]) && tokens.size() > 1) {
        index = std::stoi(tokens[0]);
        first = 1;
    }
    for (size_t i = first; i < tokens.size(); ++i) {
        float v;
        if (!parse_grid_value(tokens[i], v))
            return false;
        values.push_back(v);
    }
    return values.size() >= 2;
}

// "(  0,180)" pairs on UBL topography lines.
std::vector<Vec2d> corner_coordinates(const std::string& line)
{
    std::vector<Vec2d> out;
    size_t             pos = 0;
    while ((pos = line.find('(', pos)) != std::string::npos) {
        size_t comma = line.find(',', pos), close = line.find(')', pos);
        if (comma == std::string::npos || close == std::string::npos || comma > close)
            break;
        try {
            out.emplace_back(std::stod(line.substr(pos + 1, comma - pos - 1)), std::stod(line.substr(comma + 1, close - comma - 1)));
        } catch (const std::exception&) {
            break;
        }
        pos = close + 1;
    }
    return out;
}

} // namespace

bool BedMesh::from_marlin_report(const std::vector<std::string>& lines, const BoundingBoxf& assumed_bounds, BedMesh& out, std::string& error)
{
    // Walk the lines; a header starts a grid, rows follow (with a column-index line and, for
    // UBL, corner coordinates and a border around them), anything else ends it.
    std::vector<std::vector<float>> probed, subdivided;
    std::vector<int>                row_indices;
    std::vector<Vec2d>              corners;
    bool                            in_grid = false, grid_is_subdivided = false;
    std::vector<std::vector<float>> current;
    // Rows printed from max Y down (UBL draws the map like a picture) are flipped to min-Y-first.
    auto finish_grid = [&]() {
        if (current.empty())
            return;
        if (row_indices.size() > 1 && row_indices.front() >= 0 && row_indices.back() >= 0 && row_indices.front() > row_indices.back())
            std::reverse(current.begin(), current.end());
        (grid_is_subdivided ? subdivided : probed) = current;
        current.clear();
    };
    for (const std::string& raw : lines) {
        std::string line = raw;
        boost::algorithm::trim(line);
        if (boost::algorithm::icontains(line, "Leveling Grid") || boost::algorithm::icontains(line, "Mesh Bed Level data") ||
            boost::algorithm::icontains(line, "Bed Topography Report")) {
            // Close a grid in progress before starting the next.
            if (in_grid)
                finish_grid();
            row_indices.clear();
            in_grid            = true;
            grid_is_subdivided = boost::algorithm::icontains(line, "Subdivided");
            continue;
        }
        if (!in_grid)
            continue;
        if (line.empty())
            continue;
        std::vector<Vec2d> c = corner_coordinates(line);
        if (c.size() >= 2) {
            corners.insert(corners.end(), c.begin(), c.end());
            continue;
        }
        // The column-index line ("0 1 2 3") or a border line: skip. Checked before the row parse,
        // since integers would otherwise pass as a row of values.
        std::vector<std::string> tokens = grid_tokens(line);
        const bool all_ints = std::all_of(tokens.begin(), tokens.end(), [](const std::string& t) { return is_integer(t); });
        if (all_ints || line.find_first_not_of("-+|= ") == std::string::npos)
            continue;
        std::vector<float> values;
        int                index;
        if (parse_grid_row(line, values, index)) {
            if (!current.empty() && values.size() != current.front().size()) {
                error = "Marlin grid rows have different lengths";
                return false;
            }
            current.push_back(std::move(values));
            row_indices.push_back(index);
            continue;
        }
        // Anything else ends the grid, once it has rows: mesh bed leveling prints "3x3 mesh. Z
        // offset: 0.00000" and "Measured points:" between the header and the first row.
        if (current.empty())
            continue;
        finish_grid();
        in_grid = false;
    }
    finish_grid();
    if (probed.empty() && subdivided.empty()) {
        error = "no leveling grid found in the printer's reply";
        return false;
    }
    BedMesh m;
    m.source = "Marlin M420 V report";
    m.probed = probed.empty() ? subdivided : probed;
    m.mesh   = probed.empty() ? std::vector<std::vector<float>>() : subdivided;
    if (!m.mesh.empty() && m.mesh.front().size() < m.probed.front().size())
        m.mesh.clear();
    if (corners.size() >= 2) {
        BoundingBoxf bb;
        for (const Vec2d& p : corners)
            bb.merge(p);
        m.min  = bb.min;
        m.max  = bb.max;
        m.note = "positions from the topography report";
    }
    else if (assumed_bounds.defined && assumed_bounds.size().x() > 0.0 && assumed_bounds.size().y() > 0.0) {
        m.min  = assumed_bounds.min;
        m.max  = assumed_bounds.max;
        m.note = "positions assumed from the printer profile's bed, Marlin does not report them";
    }
    else {
        error = "Marlin reports no probe positions and the printer profile has no bed size to assume";
        return false;
    }
    out = std::move(m);
    return true;
}

} // namespace Slic3r
