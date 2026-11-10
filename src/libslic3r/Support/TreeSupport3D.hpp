// Tree supports by Thomas Rahm, losely based on Tree Supports by CuraEngine.
// Original source of Thomas Rahm's tree supports:
// https://github.com/ThomasRahm/CuraEngine
//
// Original CuraEngine copyright:
// Copyright (c) 2021 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef slic3r_TreeSupport_hpp
#define slic3r_TreeSupport_hpp

#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "SupportLayer.hpp"
#include "SupportParameters.hpp"
#include "TreeModelVolumes.hpp"
#include "TreeSupportCommon.hpp"

#include "../Point.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/container/small_vector.hpp>
#include <cstdint>
#include <limits>
#include <cstddef>
#include <optional>
#include <utility>
#include <deque>
#include <vector>
#include <functional>

namespace Slic3r { class TreeSupport; }


// #define TREE_SUPPORT_SHOW_ERRORS

#ifdef SLIC3R_TREESUPPORTS_PROGRESS
    // The various stages of the process can be weighted differently in the progress bar.
    // These weights are obtained experimentally using a small sample size. Sensible weights can differ drastically based on the assumed default settings and model.
    #define TREE_PROGRESS_TOTAL 10000
    #define TREE_PROGRESS_PRECALC_COLL TREE_PROGRESS_TOTAL * 0.1
    #define TREE_PROGRESS_PRECALC_AVO TREE_PROGRESS_TOTAL * 0.4
    #define TREE_PROGRESS_GENERATE_NODES TREE_PROGRESS_TOTAL * 0.1
    #define TREE_PROGRESS_AREA_CALC TREE_PROGRESS_TOTAL * 0.3
    #define TREE_PROGRESS_DRAW_AREAS TREE_PROGRESS_TOTAL * 0.1
    #define TREE_PROGRESS_GENERATE_BRANCH_AREAS TREE_PROGRESS_DRAW_AREAS / 3
    #define TREE_PROGRESS_SMOOTH_BRANCH_AREAS TREE_PROGRESS_DRAW_AREAS / 3
    #define TREE_PROGRESS_FINALIZE_BRANCH_AREAS TREE_PROGRESS_DRAW_AREAS / 3
#endif // SLIC3R_TREESUPPORTS_PROGRESS

namespace Slic3r
{

// Forward declarations
class Print;
class PrintObject;
struct SlicingParameters;

namespace TreeSupport3D
{


struct AreaIncreaseSettings
{
    AreaIncreaseSettings(
        TreeModelVolumes::AvoidanceType type = TreeModelVolumes::AvoidanceType::Fast, coord_t increase_speed = 0, 
        bool increase_radius = false, bool no_error = false, bool use_min_distance = false, bool move = false) :
        increase_speed{ increase_speed }, type{ type }, increase_radius{ increase_radius }, no_error{ no_error }, use_min_distance{ use_min_distance }, move{ move } {}

    coord_t         increase_speed;
    // Packing for smaller memory footprint of SupportElementState && SupportElementMerging
    TreeModelVolumes::AvoidanceType type;
    bool            increase_radius  : 1;
    bool            no_error         : 1;
    bool            use_min_distance : 1;
    bool            move             : 1;
    bool operator==(const AreaIncreaseSettings& other) const
    {
        return type             == other.type               &&
               increase_speed   == other.increase_speed     &&
               increase_radius  == other.increase_radius    &&
               no_error         == other.no_error           &&
               use_min_distance == other.use_min_distance   &&
               move             == other.move;
    }
};

#define TREE_SUPPORTS_TRACK_LOST

// C++17 does not support in place initializers of bit values, thus a constructor zeroing the bits is provided.
struct SupportElementStateBits {
    SupportElementStateBits() :
        to_buildplate(false),
        to_model_gracious(false),
        use_min_xy_dist(false),
        supports_roof(false),
        can_use_safe_radius(false),
        skip_ovalisation(false),
#ifdef TREE_SUPPORTS_TRACK_LOST
        lost(false),
        verylost(false),
#endif // TREE_SUPPORTS_TRACK_LOST
        deleted(false),
        marked(false)
        {}

    /*!
     * \brief The element trys to reach the buildplate
     */
    bool to_buildplate : 1;

    /*!
     * \brief Will the branch be able to rest completely on a flat surface, be it buildplate or model ?
     */
    bool to_model_gracious : 1;

    /*!
     * \brief Whether the min_xy_distance can be used to get avoidance or similar. Will only be true if support_xy_overrides_z=Z overrides X/Y.
     */
    bool use_min_xy_dist : 1;

    /*!
     * \brief True if this Element or any parent (element above) provides support to a support roof.
     */
    bool supports_roof : 1;

    /*!
     * \brief An influence area is considered safe when it can use the holefree avoidance <=> It will not have to encounter holes on its way downward.
     */
    bool can_use_safe_radius : 1;

    /*!
     * \brief Skip the ovalisation to parent and children when generating the final circles.
     */
    bool skip_ovalisation : 1;

#ifdef TREE_SUPPORTS_TRACK_LOST
    // Likely a lost branch, debugging information.
    bool lost : 1;
    bool verylost : 1;
#endif // TREE_SUPPORTS_TRACK_LOST

    // Not valid anymore, to be deleted.
    bool deleted : 1;

    // General purpose flag marking a visited element.
    bool marked : 1;
};

struct SupportElementState : public SupportElementStateBits
{
    int type = 0;
    coordf_t radius = 0;
    float print_z = 0;

    /*!
     * \brief The layer this support elements wants reach
     */
    LayerIndex  target_height;

    /*!
     * \brief The position this support elements wants to support on layer=target_height
     */
    Point       target_position;

    /*!
     * \brief The next position this support elements wants to reach. NOTE: This is mainly a suggestion regarding direction inside the influence area.
     */
    Point       next_position;

    /*!
     * \brief The next height this support elements wants to reach
     */
    LayerIndex  layer_idx;

    /*!
     * \brief The Effective distance to top of this element regarding radius increases and collision calculations.
     */
    uint32_t    effective_radius_height;

    /*!
     * \brief The amount of layers this element is below the topmost layer of this branch.
     */
    uint32_t    distance_to_top;

    /*!
     * \brief The resulting center point around which a circle will be drawn later.
     * Will be set by setPointsOnAreas
     */
    Point result_on_layer { std::numeric_limits<coord_t>::max(), std::numeric_limits<coord_t>::max() };
    bool  result_on_layer_is_set() const { return this->result_on_layer != Point{ std::numeric_limits<coord_t>::max(), std::numeric_limits<coord_t>::max() }; }
    void  result_on_layer_reset() { this->result_on_layer = Point{ std::numeric_limits<coord_t>::max(), std::numeric_limits<coord_t>::max() }; }
    /*!
     * \brief The amount of extra radius we got from merging branches that could have reached the buildplate, but merged with ones that can not.
     */
    coord_t     increased_to_model_radius; // how much to model we increased only relevant for merging

    /*!
     * \brief Counter about the times the elephant foot was increased. Can be fractions for merge reasons.
     */
    double      elephant_foot_increases;

    /*!
     * \brief The element tries to not move until this dtt is reached, is set to 0 if the element had to move.
     */
    uint32_t    dont_move_until;

    /*!
     * \brief Number of pending roof/contact recovery slices from this node downward, including this node.
     */
    uint32_t    missing_roof_layers = 0;

    /*!
     * \brief Contact/interface depth that this node should recover when missing_roof_layers > 0.
     */
    uint32_t    roof_recovery_dtt = 0;

    void set_pending_roof_recovery(uint32_t pending_layers, uint32_t recovery_depth)
    {
        this->missing_roof_layers = pending_layers;
        this->roof_recovery_dtt = pending_layers > 0 ? recovery_depth : 0;
    }

    // called by increase_single_area() and increaseAreas()
    [[nodiscard]] static SupportElementState propagate_down(const SupportElementState &src)
    {
        SupportElementState dst{ src };
        ++ dst.distance_to_top;
        -- dst.layer_idx;
        if (dst.has_pending_roof_recovery()) {
            -- dst.missing_roof_layers;
            ++ dst.roof_recovery_dtt;
        }
        // set to invalid as we are a new node on a new layer
        dst.result_on_layer_reset();
        dst.skip_ovalisation = false;
        return dst;
    }

    [[nodiscard]] bool locked() const { return this->distance_to_top < this->dont_move_until; }
    [[nodiscard]] bool has_pending_roof_recovery() const { return this->missing_roof_layers > 0; }
};

/*!
 * \brief Get the Distance to top regarding the real radius this part will have. This is different from distance_to_top, which is can be used to calculate the top most layer of the branch.
 * \param elem[in] The SupportElement one wants to know the effectiveDTT
 * \return The Effective DTT.
 */
[[nodiscard]] inline size_t getEffectiveDTT(const TreeSupportSettings &settings, const SupportElementState &elem)
{
    return elem.effective_radius_height < settings.increase_radius_until_layer ? 
        (elem.distance_to_top < settings.increase_radius_until_layer ? elem.distance_to_top : settings.increase_radius_until_layer) : 
        elem.effective_radius_height;
}

/*!
 * \brief Get the Radius, that this element will have.
 * \param elem[in] The Element.
 * \return The radius the element has.
 */
[[nodiscard]] inline coord_t support_element_radius(const TreeSupportSettings &settings, const SupportElementState &elem)
{ 
    return settings.getRadius(getEffectiveDTT(settings, elem), elem.elephant_foot_increases);
}

/*!
 * \brief Get the collision Radius of this Element. This can be smaller then the actual radius, as the drawAreas will cut off areas that may collide with the model.
 * \param elem[in] The Element.
 * \return The collision radius the element has.
 */
[[nodiscard]] inline coord_t support_element_collision_radius(const TreeSupportSettings &settings, const SupportElementState &elem)
{
    return settings.getRadius(elem.effective_radius_height, elem.elephant_foot_increases);
}

struct SupportElement
{
    using ParentIndices =
#ifdef NDEBUG
        // To reduce memory allocation in release mode.
        boost::container::small_vector<int32_t, 4>;
#else // NDEBUG
        // To ease debugging.
        std::vector<int32_t>;
#endif // NDEBUG

//    SupportElement(const SupportElementState &state) : SupportElementState(state) {}
    SupportElement(const SupportElementState &state, Polygons &&influence_area) : state(state), influence_area(std::move(influence_area)) {}
    SupportElement(const SupportElementState &state, ParentIndices &&parents, Polygons &&influence_area) :
        state(state), parents(std::move(parents)), influence_area(std::move(influence_area)) {}

    SupportElementState         state;

    /*!
     * \brief All elements in the layer above the current one that are supported by this element
     */
    ParentIndices               parents;

    /*!
     * \brief The resulting influence area.
     * Will only be set in the results of createLayerPathing, and will be nullptr inside!
     */
    Polygons                    influence_area;
};

using SupportElements = std::deque<SupportElement>;

// Propagate and validate the influence area of one branch for the next layer down.
[[nodiscard]] std::optional<SupportElementState> increase_single_area(const TreeModelVolumes &volumes,
    const TreeSupportSettings &config, const AreaIncreaseSettings &settings, LayerIndex layer_idx,
    const SupportElement &parent, const Polygons &relevant_offset, Polygons &to_bp_data,
    Polygons &to_model_data, Polygons &increased, coord_t overspeed);

[[nodiscard]] inline coord_t support_element_radius(const TreeSupportSettings &settings, const SupportElement &elem)
{
    return support_element_radius(settings, elem.state);
}

[[nodiscard]] inline coord_t support_element_collision_radius(const TreeSupportSettings &settings, const SupportElement &elem)
{
    return support_element_collision_radius(settings, elem.state);
}

// Turn the top layers of a branch path, whose tip could not get its roof layers directly,
// into roofs. slices holds the branch slices from layer_begin up.
void recover_pending_branch_roofs(InterfacePlacer &interface_placer, const std::vector<const SupportElement*> &branch_path,
    LayerIndex layer_begin, std::vector<Polygons> &slices);

// Remove whole roof parts fully covered by a continuing branch, preserving bottom interfaces and walls they carry.
void organic_remove_roofs_inside_branches(const SupportGeneratorLayersPtr &bottom_contacts,
    SupportGeneratorLayersPtr &top_contacts, SupportGeneratorLayersPtr &interface_layers,
    SupportGeneratorLayersPtr &base_interface_layers, const SupportGeneratorLayersPtr &intermediate_layers, coord_t line_width);

// Fill the holes and the slits of the branch bodies where the body of the layer above covers them, with
// everything inside, away from the model. bodies are sorted by print_z, model holds for each the model grown by the
// XY distance, together with the model of the layers within the Z distances above and below it.
void organic_fill_covered_holes(SupportGeneratorLayersPtr &bodies, const std::vector<Polygons> &model, coord_t line_width);

// Keep a body whole where it splits going down into the branches that merge into it going up, narrowing by move a layer
// into them, away from the model on the layer and below it. bodies hold a layer each and may hold nullptr; model holds
// for each layer the model with the gaps around it.
void organic_keep_merged_bodies_whole(SupportGeneratorLayersPtr &bodies, const std::vector<Polygons> &model, coord_t move);

// Keep each body that stands on the layer below within half a line_width, or move where that is more, of what it stands
// on there going up: the body below but its parts narrower than line_width, the roofs, and the model where the support
// may rest on it, on_model. Where it may not, over the model of any layer below within move. A body standing on none of
// them is left as it is. bodies, roofs and model hold their layers as for organic_widen_bodies_under_roofs().
void organic_limit_body_growth(SupportGeneratorLayersPtr &bodies, const std::vector<Polygons> &roofs,
    const std::vector<Polygons> &model, bool on_model, coord_t line_width, coord_t move);

// Keep each hollow body that goes on over a body of the layer below whole there but for a band of step along its outline,
// away from the model and the roofs of its own layer. A body ending stays ended. bodies, roofs and model hold their layers
// as for organic_widen_bodies_under_roofs().
void organic_limit_body_shrink(SupportGeneratorLayersPtr &bodies, const std::vector<Polygons> &roofs,
    const std::vector<Polygons> &model, coord_t step);

// Widen the top layers of the bodies under the lowest layer of a roof, where the roof hangs over the material below it,
// by move a layer going up, half a line at most, until it does not, keeping the bodies a line and a half apart but the
// flares of tips and near where the roof still hangs. The roof hangs where a line of it in the directions roof_angles, see
// SupportParameters::support_interface_angle(), runs farther from that material than a line between two lines of the
// base, or along a gap in it narrower than that, or with none, where a point of it lies farther than reach from it.
// bodies hold a layer each, up to the layer above the top body, and may hold nullptr; roofs and model hold for each
// layer the roofs printed on it and the model with the gaps around it. The widened tips, and the bands the wider bodies
// widen by, are added to the solid polygons of their layers.
void organic_widen_bodies_under_roofs(SupportGeneratorLayersPtr &bodies, const std::vector<Polygons> &roofs,
    const std::vector<Polygons> &model, coord_t line_width, coord_t tip_radius, coord_t move, coord_t reach,
    const std::vector<double> &roof_angles = {});

// Grow trees inside the hollow bodies under the parts of the roofs and of the parts printed solid that lie over their
// inside, farther than reach from their walls: tips as many as it takes to bring every point of them within reach,
// and below them trees that lean by the branch angle toward the nearest wall until they stand on it, if they reach it
// before the bed, and otherwise go straight down, merging by the preferred angle. bodies, roofs and model hold their
// layers as for organic_widen_bodies_under_roofs(). The trees inside a body are added to the inner trees of their
// layers, those that leave it below to the polygons. On the first layer inner_trees also reserves their feet, printed
// separately from the sparse flange. first_layer_width is the width of the lines of the first layer.
// The directions of the lines of the lowest roof layers: an interlaced pattern turns them from layer to layer, and the
// contact layer of a grid crosses them; a concentric or spiral roof prints no lines.
std::vector<double> organic_roof_angles(const PrintObject &object, const SupportParameters &params);

// roof_angles are the directions of the lines of the lowest roof layers, see organic_roof_angles():
// the trees hold every one of those lines; with none, every point of a roof within reach.
void organic_grow_trees_inside_hollow_bodies(SupportGeneratorLayersPtr &bodies, const std::vector<Polygons> &roofs,
    const std::vector<Polygons> &model, const TreeSupportSettings &config, coord_t reach, coord_t first_layer_width,
    const std::vector<double> &roof_angles = {}, const std::function<void()> &throw_on_cancel = {});

// Points on each of the input lines, the tips of trees along an overhang, distance apart at most and at least that far
// from each other, both ends of a line among them; at least min_points of them, closer if need be. A short line gets
// one in its middle.
[[nodiscard]] Polylines ensure_maximum_distance_polyline(const Polylines &input, double distance, size_t min_points);

// Tips of tip_radius supporting an area interface, connect_length apart but no closer than their diameter and a line:
// on the outline of the area inset by half a line, one at every sharp convex corner and evenly between the corners;
// inside, on a hexagonal lattice fixed in the object's coordinates, at least half the spacing from the outline; and one
// wherever a point of the area is still farther than 0.7 spacing, and than the least distance, from them all. Two points
// of the outline closer than the least distance share one tip in their middle, so a strip too narrow for two rows of
// tips gets one along its middle. An area whose inset outline lies within two lines of the disc of one tip in its middle
// gets only that tip.
Polylines organic_roof_tips(const Polygons &area, coord_t line_width, coord_t tip_radius, coord_t connect_length);

// Organic specific: Smooth branches and produce one cummulative mesh to be sliced.
void organic_draw_branches(
    PrintObject                     &print_object,
    TreeModelVolumes                &volumes, 
    const TreeSupportSettings       &config,
    std::vector<SupportElements>    &move_bounds,

    // I/O:
    SupportGeneratorLayersPtr       &bottom_contacts,
    SupportGeneratorLayersPtr       &top_contacts,
    InterfacePlacer                 &interface_placer,

    // Output:
    SupportGeneratorLayersPtr       &intermediate_layers,
    SupportGeneratorLayerStorage    &layer_storage,

    std::function<void()> throw_on_cancel);

} // namespace TreeSupport3D

void generate_tree_support_3D(PrintObject &print_object, TreeSupport* tree_support, std::function<void()> throw_on_cancel = []{});

} // namespace Slic3r

#endif /* slic3r_TreeSupport_hpp */
