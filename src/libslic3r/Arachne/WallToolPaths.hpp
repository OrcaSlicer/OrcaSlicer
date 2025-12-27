// Copyright (c) 2020 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef CURAENGINE_WALLTOOLPATHS_H
#define CURAENGINE_WALLTOOLPATHS_H

#include <memory>
#include <ankerl/unordered_dense.h>

#include "BeadingStrategy/BeadingStrategyFactory.hpp"
#include "utils/ExtrusionLine.hpp"
#include "../Polygon.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r::Arachne
{

constexpr bool    fill_outline_gaps                        = true;
inline coord_t    meshfix_maximum_resolution() { return scaled<coord_t>(0.5); }
inline coord_t    meshfix_maximum_deviation() { return scaled<coord_t>(0.025); }
inline coord_t    meshfix_maximum_extrusion_area_deviation() { return scaled<coord_t>(2.); }

class WallToolPathsParams
{
public:
    float   min_bead_width;
    float   min_feature_size;
    float   min_length_factor;
    float   wall_transition_length;
    float   wall_transition_angle;
    float   wall_transition_filter_deviation;
    int     wall_distribution_count;
    bool    is_top_or_bottom_layer;
};

WallToolPathsParams make_paths_params(const int layer_id, const PrintObjectConfig &print_object_config, const PrintConfig &print_config);

class WallToolPaths
{
public:
    /*!
     * A class that creates the toolpaths given an outline, nominal bead width and maximum amount of walls
     * \param outline An outline of the area in which the ToolPaths are to be generated
     * \param bead_width_0 The bead width of the first wall used in the generation of the toolpaths
     * \param bead_width_x The bead width of the inner walls used in the generation of the toolpaths
     * \param inset_count The maximum number of parallel extrusion lines that make up the wall
     * \param wall_0_inset How far to inset the outer wall, to make it adhere better to other walls.
     */
    WallToolPaths(const Polygons& outline, coord_t bead_width_0, coord_t bead_width_x, size_t inset_count, coord_t wall_0_inset, coordf_t layer_height, const WallToolPathsParams &params);

    /*!
     * Generates the Toolpaths
     * \return A reference to the newly create  ToolPaths
     */
    const std::vector<VariableWidthLines> &generate();

    /*!
     * Gets the toolpaths, if this called before \p generate() it will first generate the Toolpaths
     * \return a reference to the toolpaths
     */
    const std::vector<VariableWidthLines> &getToolPaths();

    /*!
     * Compute the inner contour of the walls. This contour indicates where the walled area ends and its infill begins.
     * The inside can then be filled, e.g. with skin/infill for the walls of a part, or with a pattern in the case of
     * infill with extra infill walls.
     */
    void separateOutInnerContour();

    /*!
     * Gets the inner contour of the area which is inside of the generated tool
     * paths.
     *
     * If the walls haven't been generated yet, this will lazily call the
     * \p generate() function to generate the walls with variable width.
     * The resulting polygon will snugly match the inside of the variable-width
     * walls where the walls get limited by the LimitedBeadingStrategy to a
     * maximum wall count.
     * If there are no walls, the outline will be returned.
     * \return The inner contour of the generated walls.
     */
    const Polygons& getInnerContour();

    /*!
     * Removes empty paths from the toolpaths
     * \param toolpaths the VariableWidthPaths generated with \p generate()
     * \return true if there are still paths left. If all toolpaths were removed it returns false
     */
    static bool removeEmptyToolPaths(std::vector<VariableWidthLines> &toolpaths);

    using ExtrusionLineSet = ankerl::unordered_dense::set<std::pair<const ExtrusionLine *, const ExtrusionLine *>, boost::hash<std::pair<const ExtrusionLine *, const ExtrusionLine *>>>;

    /*!
     * Get the order constraints of the insets when printing walls per region / hole.
     * Each returned pair consists of adjacent wall lines where the left has an inset_idx one lower than the right.
     *
     * Odd walls should always go after their enclosing wall polygons.
     *
     * \param outer_to_inner Whether the wall polygons with a lower inset_idx should go before those with a higher one.
     */
    static ExtrusionLineSet getRegionOrder(const std::vector<ExtrusionLine *> &input, bool outer_to_inner);

protected:
    /*!
     * Stitch the polylines together and form closed polygons.
     *
     * Works on both toolpaths and inner contours simultaneously.
     */
    static void stitchToolPaths(std::vector<VariableWidthLines> &toolpaths, coord_t bead_width_x);

    /*!
     * Remove polylines shorter than half the smallest line width along that polyline.
     */
    void removeSmallLines(std::vector<VariableWidthLines> &toolpaths);

    /*!
     * Simplifies the variable-width toolpaths by calling the simplify on every line in the toolpath using the provided
     * settings.
     * \param settings The settings as provided by the user
     * \return
     */
    static void simplifyToolPaths(std::vector<VariableWidthLines>  &toolpaths);

private:
    const Polygons& outline; //<! A reference to the outline polygon that is the designated area
    coord_t bead_width_0; //<! The nominal or first extrusion line width with which libArachne generates its walls
    coord_t bead_width_x; //<! The subsequently extrusion line width with which libArachne generates its walls if WallToolPaths was called with the nominal_bead_width Constructor this is the same as bead_width_0
    size_t inset_count; //<! The maximum number of walls to generate
    coord_t wall_0_inset; //<! How far to inset the outer wall. Should only be applied when printing the actual walls, not extra infill/skin/support walls.
    coordf_t layer_height;
    bool print_thin_walls; //<! Whether to enable the widening beading meta-strategy for thin features
    coord_t min_feature_size; //<! The minimum size of the features that can be widened by the widening beading meta-strategy. Features thinner than that will not be printed
    coord_t min_bead_width;  //<! The minimum bead size to use when widening thin model features with the widening beading meta-strategy
    double small_area_length; //<! The length of the small features which are to be filtered out, this is squared into a surface
    coord_t wall_transition_filter_deviation; //!< The allowed line width deviation induced by filtering
    bool toolpaths_generated; //<! Are the toolpaths generated
    std::vector<VariableWidthLines> toolpaths; //<! The generated toolpaths
    Polygons inner_contour;  //<! The inner contour of the generated toolpaths
    const WallToolPathsParams m_params;
};

} // namespace Slic3r::Arachne

#endif // CURAENGINE_WALLTOOLPATHS_H
