#pragma once

// Orca: seam markers in the preview for outer walls that start on an overhang.
// The G-code text labels such a loop "Overhang wall", exactly like an inner one, so the
// seam detector of GCodeProcessor cannot tell it is an outer wall. The generator hands
// these loops to the processor beside the text; the processor then treats them as outer
// walls for seam detection only. The text and the G-code filters stay untouched.
// See docs/HLSD/overhang-seam-preview.md.

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// A G-code command position (after the plate offset, as written to the text) on the grid
// of the exported XYZ precision, with the filament. Compared exactly.
struct OverhangSeamKey
{
    int64_t x{0};
    int64_t y{0};
    int64_t z{0};
    int     filament{-1};

    static OverhangSeamKey from_gcode(double x, double y, double z, int filament);

    bool operator==(const OverhangSeamKey &rhs) const { return x == rhs.x && y == rhs.y && z == rhs.z && filament == rhs.filament; }
    bool operator!=(const OverhangSeamKey &rhs) const { return !(*this == rhs); }
    bool operator<(const OverhangSeamKey &rhs) const
    {
        return x != rhs.x ? x < rhs.x : y != rhs.y ? y < rhs.y : z != rhs.z ? z < rhs.z : filament < rhs.filament;
    }
};

// An outer wall loop whose first printing move is an overhang.
struct OverhangSeamLoop
{
    OverhangSeamKey start;     // start of the first printing move
    OverhangSeamKey end;       // end of the last printing move
    int             layer_num{0}; // 1-based, for the log and the warning
    std::string     object;       // object name, for the log and the warning
};

// Diagnostics of one export, kept in GCodeProcessorResult.
struct OverhangSeamStats
{
    // Missed or suspect loops of one object, for the warning.
    struct ProblemObject
    {
        std::string object;
        int         first_layer{0};
        size_t      count{0};
    };

    size_t      registered{0};     // loops received from the generator
    size_t      end_consistent{0}; // matched, and the candidate ended where the loop ends
    size_t      suspect{0};        // matched, but the end differs, loops merged or never ended
    size_t      missed{0};         // no move matched the loop start
    size_t      layer_tags_expected{0}; // layer tags written by the generator
    size_t      layer_tags_seen{0};     // layer tags read by the processor
    std::vector<ProblemObject> problem_objects; // in the order of their first problem
    // A path printed as "Overhang wall" from start to end: without the generator's hand-over,
    // as for a G-code file, it cannot be told an outer wall, so it has no seam marker.
    bool        overhang_only_paths{false};

    bool layer_tags_mismatch() const { return layer_tags_expected != layer_tags_seen; }
    // A warning is due only when the mechanism was used and found a discrepancy.
    bool needs_warning() const { return registered > 0 && (missed + suspect > 0 || layer_tags_mismatch()); }
};

// Hands finished layer packets from the generator stage of the export pipeline to the
// output stage that feeds the processor. Both stages run concurrently, so access is locked;
// it happens once per layer.
class OverhangSeamChannel
{
public:
    // A packet is keyed by the ordinal of the layer change tag that opens its layer.
    void                          publish(size_t layer_tag, std::vector<OverhangSeamLoop> loops);
    std::vector<OverhangSeamLoop> take(size_t layer_tag);
    std::vector<OverhangSeamLoop> take_all();
    void                          clear();

private:
    std::mutex                                       m_mutex;
    std::map<size_t, std::vector<OverhangSeamLoop>> m_packets;
};

// Processor side: the packet of the current layer and the loops of the current seam
// detector candidate. Used by the output stage only.
class OverhangSeamMatcher
{
public:
    void reset();

    // On a layer change tag: unmatched loops of the previous layer are missed.
    void on_layer_change(OverhangSeamChannel &channel);
    // Cheap gate for the per-move lookup.
    bool has_unmatched() const { return m_unmatched > 0; }
    // On an "Overhang wall" printing move starting at `start`. Returns true if it starts a
    // registered loop; `joins_candidate` tells the seam detector already has a first vertex.
    bool match_start(const OverhangSeamKey &start, bool joins_candidate);
    // True while the seam detector candidate started on a registered loop.
    bool in_candidate() const { return !m_candidate.empty(); }
    // On every printing move of such a candidate.
    void on_candidate_extrusion(const OverhangSeamKey &end) { m_candidate_end = end; }
    // When the seam detector closes its candidate.
    void on_candidate_end();
    // A path ended while still printed as "Overhang wall" only.
    void note_overhang_only_path() { m_stats.overhang_only_paths = true; }
    // At the end of the export; `layer_tags_expected` comes from the generator.
    void finish(OverhangSeamChannel &channel, size_t layer_tags_expected);

    const OverhangSeamStats &stats() const { return m_stats; }

private:
    struct Entry
    {
        OverhangSeamLoop loop;
        bool             used{false};
    };
    struct CandidateLoop
    {
        OverhangSeamLoop loop;
        bool             merged{false};
    };

    void close_packet();
    // `missed`: the start was never found; otherwise the match is suspect.
    void report(const OverhangSeamLoop &loop, bool missed, const char *problem);

    std::vector<Entry>         m_packet;
    // Packet entries sorted by start, then by registration order, for the per-move lookup.
    std::vector<std::pair<OverhangSeamKey, size_t>> m_index;
    size_t                     m_unmatched{0};
    std::vector<CandidateLoop> m_candidate;
    OverhangSeamKey            m_candidate_end;
    OverhangSeamStats          m_stats;
};

} // namespace Slic3r
