// Tree supports by Thomas Rahm, losely based on Tree Supports by CuraEngine.
// Original source of Thomas Rahm's tree supports:
// https://github.com/ThomasRahm/CuraEngine
//
// Original CuraEngine copyright:
// Copyright (c) 2021 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef slic3r_TreeSupport_hpp
#define slic3r_TreeSupport_hpp

#include "SupportLayer.hpp"
#include "TreeModelVolumes.hpp"
#include "TreeSupportCommon.hpp"

#include "../BoundingBox.hpp"
#include "../Point.hpp"
#include "../Utils.hpp"

#include <boost/container/small_vector.hpp>


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
     * \brief Settings used to increase the influence area to its current state.
     */
    AreaIncreaseSettings last_area_increase;

    /*!
     * \brief Amount of roof layers that were not yet added, because the branch needed to move.
     */
    uint32_t    missing_roof_layers;

    // called by increase_single_area() and increaseAreas()
    [[nodiscard]] static SupportElementState propagate_down(const SupportElementState &src)
    {
        SupportElementState dst{ src };
        ++ dst.distance_to_top;
        -- dst.layer_idx;
        // set to invalid as we are a new node on a new layer
        dst.result_on_layer_reset();
        dst.skip_ovalisation = false;
        return dst;
    }

    [[nodiscard]] bool locked() const { return this->distance_to_top < this->dont_move_until; }
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

[[nodiscard]] inline coord_t support_element_radius(const TreeSupportSettings &settings, const SupportElement &elem)
{
    return support_element_radius(settings, elem.state);
}

[[nodiscard]] inline coord_t support_element_collision_radius(const TreeSupportSettings &settings, const SupportElement &elem)
{
    return support_element_collision_radius(settings, elem.state);
}

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
