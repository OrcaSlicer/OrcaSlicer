#include "DynamicCompositeObjects.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {

constexpr double EPS = 1e-9;

// Travel moves longer than this (mm) split a layer into separate chunks.
// Kept small so the legs of a narrow letter "H" (gap of a few mm) separate;
// chunks of the same island are re-merged by footprint below.
constexpr double SPLIT_TRAVEL_MM = 4.0;
// Chunks whose extruded footprints are closer than this (mm) belong to the
// same island. A tower's wall ring and its infill overlap, so a small
// threshold groups them; separate towers stay apart.
constexpr double MERGE_BBOX_MM = 2.0;
// Islands whose centroids move less than this (mm) between two consecutive
// layers are considered the same tower.
constexpr double MATCH_CENTROID_MM = 8.0;
// Input larger than this is left untouched.
constexpr size_t MAX_INPUT_BYTES = 256u * 1024u * 1024u;
// The total extruded length of the reordered G-code must agree with the
// original within this tolerance, otherwise the reordering is discarded.
constexpr double E_VERIFY_TOLERANCE = 0.1;

// A single G-code line, referencing into the original string.
struct LineInfo {
    size_t offset = 0;
    size_t len    = 0;
    bool   is_marker = false;  // ";LAYER_CHANGE" / "; CHANGE_LAYER" / ";CPLAYER_CHANGE"
    bool   is_gcode  = false;  // G0/G1/G2/G3/G92
    bool   has_x = false, has_y = false, has_z = false, has_e = false;
    double x = 0.0, y = 0.0, z = 0.0, e = 0.0;
    bool   extruding = false;    // move with positive E
    bool   t_change  = false;    // "Tn" tool change
    bool   is_g92    = false;    // "G92" (E counter re-base)
    bool   e_abs_after = true;   // state after an M82/M83/G90/G91 line
    bool   e_mode_line = false;  // the line itself is an E-mode switch
    bool   layer_z_comment = false; // ";Z:" / "; Z_HEIGHT:" comment
    double z_value = 0.0;        // layer z carried by layer_z_comment
    double f_value = 0.0;        // F parameter of the line (0 if absent)
};

// Token of a G-code line, copied into a NUL-terminated buffer for strtod.
struct Token {
    char        code = 0;
    double      value = 0.0;
    std::string text;  // full token text, e.g. "X120.3"
};

static void parse_tokens(std::string_view line, std::vector<Token>& tokens)
{
    tokens.clear();
    size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
            ++pos;
        if (pos >= line.size())
            break;
        size_t end = pos;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t')
            ++end;
        Token tok;
        tok.text.assign(line.data() + pos, end - pos);
        pos = end;
        if (tok.text.empty())
            continue;
        tok.code = tok.text[0];
        if (tok.text.size() > 1) {
            char buf[64];
            if (tok.text.size() < sizeof(buf)) {
                std::memcpy(buf, tok.text.data(), tok.text.size());
                buf[tok.text.size()] = 0;
                char* num_end = nullptr;
                const double v = std::strtod(buf + 1, &num_end);
                if (num_end != buf + 1)
                    tok.value = v;
            }
        }
        tokens.push_back(std::move(tok));
    }
}

static void classify_line(const std::string& gcode, size_t off, size_t len, LineInfo& li)
{
    li.offset = off;
    li.len    = len;
    std::string_view line(gcode.data() + off, len);
    if (line.empty())
        return;
    if (line[0] == ';' || line[0] == '(' || line[0] == '%') {
        // ";LAYER_CHANGE" (Marlin-compatible tag table) and "; CHANGE_LAYER"
        // (BBL tag table, prefixed with a space) are both used by the G-code
        // processor's layer detection; ";CPLAYER_CHANGE" is the color-print
        // variant.
        if (line.size() >= 13 &&
            (line.substr(0, 13) == ";LAYER_CHANGE" || line.substr(0, 15) == ";CPLAYER_CHANGE"))
            li.is_marker = true;
        else if (line.size() >= 14 && line.substr(0, 14) == "; CHANGE_LAYER")
            li.is_marker = true;
        else if (line.size() >= 3 && line[1] == 'Z' && line[2] == ':') {
            char buf[64];
            const size_t n = std::min<size_t>(line.size() - 3, sizeof(buf) - 1);
            std::memcpy(buf, line.data() + 3, n);
            buf[n] = 0;
            li.layer_z_comment = true;
            li.z_value = std::strtod(buf, nullptr);
        } else if (line.size() >= 12 && line.substr(0, 11) == "; Z_HEIGHT: ") {
            char buf[64];
            const size_t n = std::min<size_t>(line.size() - 11, sizeof(buf) - 1);
            std::memcpy(buf, line.data() + 11, n);
            buf[n] = 0;
            li.layer_z_comment = true;
            li.z_value = std::strtod(buf, nullptr);
        }
        return;
    }

    std::vector<Token> tokens;
    parse_tokens(line, tokens);
    if (tokens.empty())
        return;
    const std::string& cmd = tokens[0].text;
    const bool is_move = (cmd == "G0" || cmd == "G1" || cmd == "G2" || cmd == "G3");
    const bool is_g92  = (cmd == "G92");
    if (!is_move && !is_g92) {
        if (cmd == "M82" || cmd == "G90") {
            li.e_mode_line = true;
            li.e_abs_after = true;
        } else if (cmd == "M83" || cmd == "G91") {
            li.e_mode_line = true;
            li.e_abs_after = false;
        } else if (cmd.size() >= 2 && cmd[0] == 'T' && cmd[1] >= '0' && cmd[1] <= '9') {
            // A "Tn" line selects an extruder; treat it as a tool change only
            // when n is a plausible extruder index. Sentinels such as "T65535"
            // (AMS filament pull-back in machine end G-code) are not tool
            // changes.
            char* end = nullptr;
            const long tool = std::strtol(cmd.c_str() + 1, &end, 10);
            li.t_change = (end != cmd.c_str() + 1 && tool >= 0 && tool < 256);
        }
        return;
    }
    li.is_gcode = true;
    li.is_g92 = is_g92;
    for (size_t i = 1; i < tokens.size(); ++i) {
        const Token& tok = tokens[i];
        switch (tok.code) {
        case 'X': li.x = tok.value; li.has_x = true; break;
        case 'Y': li.y = tok.value; li.has_y = true; break;
        case 'Z': li.z = tok.value; li.has_z = true; break;
        case 'E': li.e = tok.value; li.has_e = true; break;
        case 'F': li.f_value = tok.value; break;
        default: break;
        }
    }
    if (is_move)
        li.extruding = (li.has_e && li.e > 1e-6);
}

// A run of lines of one layer belonging to one island.
struct Chunk {
    size_t first_line = 0;      // first line of the chunk in the file
    size_t last_line = 0;       // last line of the chunk (inclusive)
    size_t first_ext_line = 0;  // first extruding line of the chunk
    double first_abs_e = 0.0;   // absolute E at the first line of the chunk
    double cx = 0.0, cy = 0.0;  // centroid of the extruded positions
    double min_x = 0.0, min_y = 0.0, max_x = 0.0, max_y = 0.0;  // bbox of the extruded positions
    double z = 0.0;             // layer print z
    double z_feed = 300.0;      // feed rate used for synthesized Z moves
    double ext_mass = 0.0;      // total segment length feeding the centroid
    bool   has_extrusion = false;
};

struct Island {
    std::vector<size_t> chunks; // chunk indices, in file order
    double cx = 0.0, cy = 0.0;
    double min_x = 0.0, min_y = 0.0, max_x = 0.0, max_y = 0.0;  // bbox of the merged chunks
};

struct ChunkRef {
    size_t layer = 0;  // index into the layers vector
    size_t chunk = 0;  // index into layers[layer].chunks
};

struct LayerInfo {
    size_t first_line = 0;   // line index of the layer's ";LAYER_CHANGE"
    size_t last_line = 0;    // last line of the layer (inclusive)
    double z = 0.0;
    std::vector<Chunk> chunks;
    std::vector<Island> islands;
};

struct Phase {
    size_t layer_lo = 0, layer_hi = 0; // inclusive layer range
    // For every island (of the phase start layer), one chunk per layer, in
    // layer order.
    std::vector<std::vector<ChunkRef>> island_chunks;
    double z_extent = 0.0;
};

// State of the emitted stream, kept in sync with the lines written so far.
struct EmitState {
    bool   absolute_e = true;
    double cur_z = 0.0;
    double cur_e = 0.0;
    double extruded = 0.0;  // sum of positive E deltas
};

static double point_dist(double ax, double ay, double bx, double by)
{
    const double dx = ax - bx;
    const double dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

// Distance between two axis-aligned boxes; 0 when they touch or overlap.
static double bbox_dist(double aminx, double aminy, double amaxx, double amaxy,
                        double bminx, double bminy, double bmaxx, double bmaxy)
{
    const double dx = std::max(0.0, std::max(aminx - bmaxx, bminx - amaxx));
    const double dy = std::max(0.0, std::max(aminy - bmaxy, bminy - amaxy));
    return std::sqrt(dx * dx + dy * dy);
}

static double island_dist(const Island& a, const Island& b)
{
    return point_dist(a.cx, a.cy, b.cx, b.cy);
}

// Greedy one-to-one assignment of the islands of layer `a` to those of layer
// `b`. Returns false when the island structure is not stable.
static bool islands_match(const std::vector<Island>& a, const std::vector<Island>& b,
                          std::vector<size_t>& perm)
{
    const size_t k = a.size();
    perm.assign(k, std::numeric_limits<size_t>::max());
    std::vector<bool> used_a(k, false), used_b(k, false);
    for (size_t t = 0; t < k; ++t) {
        double best = MATCH_CENTROID_MM;
        size_t best_a = k, best_b = k;
        for (size_t ia = 0; ia < k; ++ia) {
            if (used_a[ia])
                continue;
            for (size_t ib = 0; ib < k; ++ib) {
                if (used_b[ib])
                    continue;
                const double d = island_dist(a[ia], b[ib]);
                if (d < best) {
                    best = d;
                    best_a = ia;
                    best_b = ib;
                }
            }
        }
        if (best_a == k)
            return false;
        used_a[best_a] = true;
        used_b[best_b] = true;
        perm[best_b] = best_a;
    }
    return true;
}

// Emit one line verbatim, keeping the E state of the emitted stream in sync.
static void emit_line(const std::string& gcode, const LineInfo& li,
                      EmitState& st, std::string& out)
{
    if (li.e_mode_line) {
        st.absolute_e = li.e_abs_after;
        out.append(gcode.data() + li.offset, li.len);
        out += '\n';
        return;
    }
    if (li.is_g92 && li.has_e) {
        // Re-base the E counter without extruding anything.
        st.cur_e = li.e;
        out.append(gcode.data() + li.offset, li.len);
        out += '\n';
        return;
    }
    out.append(gcode.data() + li.offset, li.len);
    out += '\n';
    if (li.has_e) {
        const double delta = st.absolute_e ? li.e - st.cur_e : li.e;
        if (delta > 0.0)
            st.extruded += delta;
        st.cur_e = st.absolute_e ? li.e : st.cur_e + li.e;
    }
    if (li.has_z)
        st.cur_z = li.z;
}

// Emit a travel line with the Z parameter stripped: the Z value belongs to the
// original file position and would lower the nozzle mid-travel when the chunk
// is emitted in a different Z context. The synthesized Z moves around the
// chunk keep the head at a safe height.
static void emit_travel_line_no_z(const std::string& gcode, const LineInfo& li,
                                  EmitState& st, std::string& out)
{
    const char* p = gcode.data() + li.offset;
    const char* end = p + li.len;
    bool first = true;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t'))
            ++p;
        if (p >= end)
            break;
        const char* tok = p;
        while (p < end && *p != ' ' && *p != '\t')
            ++p;
        if (*tok == 'Z')
            continue;  // drop the context-dependent Z token
        if (!first)
            out += ' ';
        first = false;
        out.append(tok, p - tok);
    }
    out += '\n';
    if (li.has_e) {
        const double delta = st.absolute_e ? li.e - st.cur_e : li.e;
        if (delta > 0.0)
            st.extruded += delta;
        st.cur_e = st.absolute_e ? li.e : st.cur_e + li.e;
    }
    // st.cur_z intentionally unchanged: the move runs at the current height.
}

// Emit a chunk at its position in the reordered sequence, keeping the emitted
// Z/E state consistent with the file position.
static void emit_chunk(const std::string& gcode, const std::vector<LineInfo>& lines,
                       const Chunk& ch, EmitState& st, std::string& out)
{
    // Raise to the layer z before the XY travel so the head travel never runs
    // below the layer being approached.
    if (st.cur_z < ch.z - EPS) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "G1 Z%.5g F%.5g\n", ch.z, ch.z_feed);
        out += buf;
        st.cur_z = ch.z;
    }
    if (st.absolute_e && std::abs(st.cur_e - ch.first_abs_e) > 1e-6) {
        // The chunk's E values are absolute positions relative to the E
        // counter of the original file position. Re-base the counter so the
        // verbatim values keep their exact deltas (a per-line renumbering
        // would accumulate rounding error over the whole print).
        char buf[64];
        std::snprintf(buf, sizeof(buf), "G92 E%.6f\n", ch.first_abs_e);
        out += buf;
        st.cur_e = ch.first_abs_e;
    }
    // Head lines. Z-only moves are dropped: their absolute values refer to the
    // original file position and could lower the nozzle onto a taller island.
    // The synthesized Z moves above/below cover them. XY travels carrying a Z
    // parameter have the Z stripped for the same reason: the travel runs at
    // the current (synthesized) height and the nozzle only lowers once it is
    // above the destination island.
    for (size_t i = ch.first_line; i < ch.first_ext_line; ++i) {
        const LineInfo& li = lines[i];
        if (li.is_gcode && li.has_z && !li.has_x && !li.has_y && !li.has_e)
            continue;
        if (li.is_gcode && li.has_z && (li.has_x || li.has_y) && !li.has_e) {
            emit_travel_line_no_z(gcode, li, st, out);
            continue;
        }
        emit_line(gcode, li, st, out);
    }
    // Lower to the layer z only once positioned above the island.
    if (std::abs(st.cur_z - ch.z) > EPS) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "G1 Z%.5g F%.5g\n", ch.z, ch.z_feed);
        out += buf;
        st.cur_z = ch.z;
    }
    for (size_t i = ch.first_ext_line; i <= ch.last_line; ++i)
        emit_line(gcode, lines[i], st, out);
}

} // namespace

std::string dynamic_composite_objects_process(const std::string& gcode,
                                              double clearance_radius,
                                              double clearance_height)
{
    if (gcode.empty() || gcode.size() > MAX_INPUT_BYTES || clearance_radius <= 0.0 ||
        clearance_height <= 0.0)
        return gcode;

    // ---- Tokenize the file into lines.
    std::vector<LineInfo> lines;
    {
        size_t start = 0;
        while (start < gcode.size()) {
            size_t nl = gcode.find('\n', start);
            size_t end = (nl == std::string::npos) ? gcode.size() : nl;
            size_t len = end - start;
            if (len > 0 && gcode[end - 1] == '\r')
                --len;
            LineInfo li;
            classify_line(gcode, start, len, li);
            lines.push_back(std::move(li));
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
    }
    if (lines.empty())
        return gcode;

    // ---- Locate the layer markers.
    std::vector<size_t> layer_first_line;
    layer_first_line.reserve(64);
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].is_marker)
            layer_first_line.push_back(i);
    if (layer_first_line.size() < 3)
        return gcode; // need at least a few layers to regroup

    // ---- Split every layer into chunks (island candidates).
    std::vector<LayerInfo> layers;
    layers.reserve(layer_first_line.size());
    bool single_extruder = true;
    bool has_exclude_object = false;
    {
        double cur_x = 0.0, cur_y = 0.0;
        double e_abs = 0.0;       // absolute E of the original file
        bool   absolute_e = true;
        double last_z_feed = 300.0;

        for (size_t l = 0; l < layer_first_line.size(); ++l) {
            const size_t first = layer_first_line[l];
            const size_t last  = (l + 1 < layer_first_line.size()) ? layer_first_line[l + 1] - 1
                                                                   : lines.size() - 1;
            LayerInfo layer;
            layer.first_line = first;
            layer.last_line  = last;

            // Layer z from the ";Z:" / "; Z_HEIGHT:" comment.
            for (size_t i = first; i <= last && i < first + 5; ++i)
                if (lines[i].layer_z_comment) {
                    layer.z = lines[i].z_value;
                    break;
                }

            Chunk chunk;
            chunk.first_line = first;
            chunk.first_ext_line = first;
            chunk.first_abs_e = e_abs;
            chunk.z = layer.z;
            chunk.z_feed = last_z_feed;
            bool have_layer_z = (layer.z > EPS);

            for (size_t i = first; i <= last; ++i) {
                const LineInfo& li = lines[i];

                if (li.t_change)
                    single_extruder = false;
                if (!has_exclude_object && li.len >= 16 &&
                    std::string_view(gcode.data() + li.offset, li.len).find("EXCLUDE_OBJECT") != std::string_view::npos)
                    has_exclude_object = true;

                if (li.has_z && !have_layer_z) {
                    // Fallback for files without a ";Z:" comment: the first Z
                    // move of the layer (before any extrusion) is its print z.
                    layer.z = li.z;
                    chunk.z = li.z;
                    have_layer_z = true;
                }
                if (li.is_gcode && li.has_z)
                    chunk.z_feed = li.f_value > 0.0 ? li.f_value : chunk.z_feed;

                if (li.is_gcode && li.has_e) {
                    if (li.extruding && !chunk.has_extrusion) {
                        chunk.has_extrusion = true;
                        chunk.first_ext_line = i;
                        chunk.z = layer.z;
                        // An E-only line (e.g. the un-retract before a wall)
                        // carries no position; use the tracked nozzle position.
                        const double px = li.has_x ? li.x : cur_x;
                        const double py = li.has_y ? li.y : cur_y;
                        chunk.cx = px;
                        chunk.cy = py;
                        chunk.min_x = chunk.max_x = px;
                        chunk.min_y = chunk.max_y = py;
                        chunk.ext_mass = 1.0;
                    } else if (li.extruding) {
                        const double dx = li.x - cur_x;
                        const double dy = li.y - cur_y;
                        const double seg = std::sqrt(dx * dx + dy * dy);
                        chunk.cx += li.x * seg;
                        chunk.cy += li.y * seg;
                        chunk.ext_mass += seg;
                        chunk.min_x = std::min(chunk.min_x, li.x);
                        chunk.max_x = std::max(chunk.max_x, li.x);
                        chunk.min_y = std::min(chunk.min_y, li.y);
                        chunk.max_y = std::max(chunk.max_y, li.y);
                    }
                    e_abs = absolute_e ? li.e : e_abs + li.e;
                } else if (li.is_gcode && (li.has_x || li.has_y)) {
                    // A non-extruding XY move: a travel. Close the chunk when
                    // the move is long enough to cross between islands.
                    const double tx = li.has_x ? li.x : cur_x;
                    const double ty = li.has_y ? li.y : cur_y;
                    if (point_dist(cur_x, cur_y, tx, ty) > SPLIT_TRAVEL_MM && chunk.has_extrusion) {
                        chunk.last_line = i - 1;
                        if (chunk.ext_mass > 1e-6) {
                            chunk.cx /= chunk.ext_mass;
                            chunk.cy /= chunk.ext_mass;
                        }
                        layer.chunks.push_back(chunk);
                        Chunk next;
                        next.first_line = i;
                        next.first_ext_line = i;
                        next.first_abs_e = e_abs;
                        next.z = layer.z;
                        next.z_feed = last_z_feed;
                        chunk = next;
                    }
                }
                if (li.has_x)
                    cur_x = li.x;
                if (li.has_y)
                    cur_y = li.y;
                chunk.last_line = i;
            }
            if (chunk.ext_mass > 1e-6) {
                chunk.cx /= chunk.ext_mass;
                chunk.cy /= chunk.ext_mass;
            }
            if (chunk.has_extrusion) {
                layer.chunks.push_back(chunk);
            } else if (!layer.chunks.empty()) {
                // A trailing head-only chunk: extend the previous chunk so no
                // lines are lost.
                layer.chunks.back().last_line = chunk.last_line;
            }
            // A layer that produced no extrusion is emitted verbatim (it has
            // no chunks and never takes part in a phase).
            if (layer.z < EPS && !layer.chunks.empty())
                layer.z = layer.chunks.front().z;
            layers.push_back(std::move(layer));
        }
    }

    if (!single_extruder || has_exclude_object)
        return gcode;

    // ---- Merge chunks into islands: chunks whose extruded footprints touch
    // or nearly touch belong to the same body. A tower's wall ring and its
    // infill pattern are emitted as separate chunks (long infill travels
    // split them) but overlap, so bbox distance groups them into one island;
    // centroid distance alone would fragment a wide body into wall + infill
    // pieces and break the phase structure.
    for (LayerInfo& layer : layers) {
        for (size_t c = 0; c < layer.chunks.size(); ++c) {
            const Chunk& ch = layer.chunks[c];
            size_t best = std::numeric_limits<size_t>::max();
            double best_d = MERGE_BBOX_MM;
            for (size_t is = 0; is < layer.islands.size(); ++is) {
                const double d = bbox_dist(layer.islands[is].min_x, layer.islands[is].min_y,
                                           layer.islands[is].max_x, layer.islands[is].max_y,
                                           ch.min_x, ch.min_y, ch.max_x, ch.max_y);
                if (d < best_d) {
                    best_d = d;
                    best = is;
                }
            }
            if (best != std::numeric_limits<size_t>::max()) {
                Island& island = layer.islands[best];
                island.chunks.push_back(c);
                const double n = static_cast<double>(island.chunks.size());
                island.cx = island.cx * (n - 1.0) / n + ch.cx / n;
                island.cy = island.cy * (n - 1.0) / n + ch.cy / n;
                island.min_x = std::min(island.min_x, ch.min_x);
                island.max_x = std::max(island.max_x, ch.max_x);
                island.min_y = std::min(island.min_y, ch.min_y);
                island.max_y = std::max(island.max_y, ch.max_y);
            } else {
                Island island;
                island.chunks.push_back(c);
                island.cx = ch.cx;
                island.cy = ch.cy;
                island.min_x = ch.min_x;
                island.max_x = ch.max_x;
                island.min_y = ch.min_y;
                island.max_y = ch.max_y;
                layer.islands.push_back(std::move(island));
            }
        }
    }

    // ---- Detect the phases: runs of layers with a stable island structure.
    std::vector<Phase> phases;
    {
        size_t i = 0;
        while (i < layers.size()) {
            if (layers[i].islands.size() < 2) {
                ++i;
                continue;
            }
            size_t j = i + 1;
            std::vector<size_t> perm;
            while (j < layers.size() && layers[j].islands.size() == layers[i].islands.size() &&
                   islands_match(layers[j - 1].islands, layers[j].islands, perm))
                ++j;
            if (j - i < 2) {
                i = j;
                continue;
            }
            Phase phase;
            phase.layer_lo = i;
            phase.layer_hi = j - 1;
            phase.island_chunks.assign(layers[i].islands.size(), {});
            for (size_t is = 0; is < layers[i].islands.size(); ++is) {
                // Follow the match chain layer by layer.
                size_t island_idx = is;
                for (size_t l = i; l < j; ++l) {
                    const Island& island = layers[l].islands[island_idx];
                    for (size_t chunk : island.chunks)
                        phase.island_chunks[is].push_back({ l, chunk });
                    if (l + 1 < j) {
                        std::vector<size_t> p;
                        if (!islands_match(layers[l].islands, layers[l + 1].islands, p))
                            break;
                        island_idx = p[island_idx];
                    }
                }
            }
            double z_lo = layers[i].z;
            double z_hi = layers[i].z;
            for (size_t l = i; l < j; ++l) {
                z_lo = std::min(z_lo, layers[l].z);
                z_hi = std::max(z_hi, layers[l].z);
            }
            phase.z_extent = z_hi - z_lo;
            phases.push_back(std::move(phase));
            i = j;
        }
    }

    // ---- Disable the whole feature when any pair of towers is inside the
    // clearance region: closer to each other than the clearance radius while
    // the height difference reached between them exceeds the clearance height.
    for (const Phase& phase : phases) {
        if (phase.z_extent <= clearance_height)
            continue;
        const LayerInfo& layer = layers[phase.layer_lo];
        const size_t k = layer.islands.size();
        for (size_t a = 0; a < k; ++a)
            for (size_t b = a + 1; b < k; ++b)
                if (island_dist(layer.islands[a], layer.islands[b]) < clearance_radius)
                    return gcode;
    }

    // ---- Emit: grouped phases island-major, everything else verbatim.
    std::string out;
    out.reserve(gcode.size() + 1024);
    EmitState st;
    bool reordered = false;

    auto emit_range_verbatim = [&](size_t from, size_t to) {
        for (size_t i = from; i <= to; ++i) {
            const LineInfo& li = lines[i];
            if (li.e_mode_line)
                st.absolute_e = li.e_abs_after;
            out.append(gcode.data() + li.offset, li.len);
            out += '\n';
            if (li.has_z)
                st.cur_z = li.z;
            if (li.has_e) {
                const double delta = st.absolute_e ? li.e - st.cur_e : li.e;
                if (delta > 0.0)
                    st.extruded += delta;
                st.cur_e = st.absolute_e ? li.e : st.cur_e + li.e;
            }
        }
    };

    // Header: everything before the first layer.
    if (layer_first_line.front() > 0)
        emit_range_verbatim(0, layer_first_line.front() - 1);

    size_t layer_idx = 0;
    while (layer_idx < layers.size()) {
        // Find a phase starting at this layer.
        const Phase* phase = nullptr;
        for (const Phase& ph : phases)
            if (ph.layer_lo == layer_idx) {
                phase = &ph;
                break;
            }
        if (phase != nullptr) {
            // Emit the whole phase island-major; the chunk references are
            // already in layer order.
            for (size_t is = 0; is < phase->island_chunks.size(); ++is)
                for (const ChunkRef& ref : phase->island_chunks[is])
                    emit_chunk(gcode, lines, layers[ref.layer].chunks[ref.chunk], st, out);
            reordered = true;
            layer_idx = phase->layer_hi + 1;
            continue;
        }
        // Verbatim layer.
        const LayerInfo& layer = layers[layer_idx];
        emit_range_verbatim(layer.first_line, layer.last_line);
        ++layer_idx;
    }
    // Tail: everything after the last layer.
    if (!layers.empty() && layers.back().last_line + 1 < lines.size())
        emit_range_verbatim(layers.back().last_line + 1, lines.size() - 1);

    if (!reordered)
        return gcode;

    // ---- Verification: the total extruded length of the reordered G-code
    // must match the original.
    double orig_extruded = 0.0;
    {
        EmitState orig;
        for (const LineInfo& li : lines) {
            if (li.e_mode_line) {
                orig.absolute_e = li.e_abs_after;
                continue;
            }
            if (!li.has_e)
                continue;
            const double delta = orig.absolute_e ? li.e - orig.cur_e : li.e;
            if (delta > 0.0)
                orig.extruded += delta;
            orig.cur_e = orig.absolute_e ? li.e : orig.cur_e + li.e;
        }
        orig_extruded = orig.extruded;
    }
    if (std::abs(orig_extruded - st.extruded) > E_VERIFY_TOLERANCE)
        return gcode;

    return out;
}

} // namespace Slic3r
