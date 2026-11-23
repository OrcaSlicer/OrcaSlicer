#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>

#include "LegendModel.hpp"
#include "LegendPanel.hpp"
#include "LegendText.hpp"
#include "PreviewOverlayLayout.hpp"
#include "PrintSummaryModel.hpp"
#include "PrintSummaryPanel.hpp"
#include "libvgcode/include/Types.hpp"

namespace Slic3r {
struct GCodeProcessorResult;

namespace GUI {

class GCodeViewer;

// The panels of the preview that read the G-code result: the legend, the print summary and the statistics of all plates, and
// what they point at in the 3D view. Kept out of GCodeViewer, which only calls them; they read its result and sliders as its
// friend, rather than through accessors that would widen the upstream class.
class PreviewPanels
{
public:
    explicit PreviewPanels(GCodeViewer& preview);

    void bind();
    void render(const PreviewOverlayLayout& layout);
    void render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& results, bool show) const;
    // A new result: the range filter and the isolated filament go.
    void drop_path_focus();
    // Another view: its scale is another, so the range filter goes.
    void drop_range_filter() { m_range_filter.reset(); }

private:
    GCodeViewer&      m_gcode;
    LegendPanel       m_legend;
    PrintSummaryPanel m_summary;
    // What the rows of the legend and the summary under the mouse stand for this frame, and the filament isolated: the paths
    // the preview keeps in colour, see apply_path_focus().
    struct PathHover
    {
        int kind{-1};
        int filament{-1};
        // The values of a category of the few-values table, and a span of the scale.
        std::optional<std::array<float, 2>> values;
        std::optional<std::array<float, 2>> range;
    };
    PathHover m_path_hover;
    int       m_isolated_filament{-1};
    // The values the current view keeps in colour, brushed on its scale in the legend.
    std::optional<std::array<float, 2>> m_range_filter;
    bool                                m_range_filter_shown{false};
    LegendPalette                       m_palette{LegendPalette::Classic};
    // The layers the scales should span and since when, so that they follow the layer slider once it rests.
    libvgcode::Interval                   m_ranges_layers{0, 0};
    std::chrono::steady_clock::time_point m_ranges_layers_since;
    unsigned int                          m_ranges_layers_result{0};
    // What a scale view's values come to in the loaded result, worked out once per view: a pass over every vertex.
    struct LegendValuesKey
    {
        unsigned int         result{0};
        size_t               vertices{0};
        libvgcode::EViewType type{libvgcode::EViewType::COUNT};
        size_t               time_mode{0};
        bool                 travels{false};
        bool                 wipes{false};
        bool                 custom{false};
        float                min{0.f};
        float                max{0.f};
        libvgcode::Interval  layers{0, 0};

        bool operator==(const LegendValuesKey& other) const
        {
            return result == other.result && vertices == other.vertices && type == other.type && time_mode == other.time_mode &&
                   travels == other.travels && wipes == other.wipes && custom == other.custom && min == other.min && max == other.max &&
                   layers == other.layers;
        }
    };
    LegendValuesKey m_legend_values_key;
    LegendValues    m_legend_values;
    // The profile's limits per filament the result was sliced with, read once per result, see profile_limits().
    struct ProfileLimits
    {
        unsigned int       result{0};
        bool               read{false};
        std::vector<float> flow;
        std::vector<float> layer_time;
    };
    ProfileLimits m_profile_limits;

    void                        bind_visibility();
    void                        bind_focus();
    void                        bind_summary();
    void                        refresh_preview();
    const ProfileLimits&        profile_limits();
    void                        apply_palette();
    void                        apply_ranges_layers();
    void                        apply_path_focus();
    void                        update_legend_values(LegendScale& scale);
    LegendValues                scan_legend_values(const LegendValuesKey& key, const LegendScale& scale);
    LegendModel                 legend_model();
    void                        add_legend_lines(LegendModel& model) const;
    void                        add_mode_summaries(LegendModel& model) const;
    void                        add_legend_scale(LegendModel& model);
    void                        add_legend_filaments(LegendModel& model) const;
    std::vector<LegendFilament> legend_filaments() const;
    PrintSummaryModel           print_summary_model() const;
    void                        add_nozzle_grouping(PrintSummaryModel& model) const;
    void                        add_summary_events(PrintSummaryModel& model) const;
    void                        add_file_profiles(PrintSummaryModel& model) const;
    void                        add_summary_totals(PrintSummaryModel& model) const;
    void                        render_legend_panel(const PreviewOverlayLayout& layout);
    void                        render_summary_panel(const PreviewOverlayLayout& layout);
};

} // namespace GUI
} // namespace Slic3r
