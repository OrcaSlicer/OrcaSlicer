#ifndef ARRANGEJOB_HPP
#define ARRANGEJOB_HPP


#include <optional>

#include "Job.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"

namespace Slic3r {

class ModelInstance;

namespace GUI {

class Plater;

class ArrangeJob : public Job
{
    using ArrangePolygon = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    //BBS: add locked logic
    ArrangePolygons m_selected, m_unselected, m_unprintable, m_locked;
    std::vector<ModelInstance*> m_unarranged;
    std::map<int, ArrangePolygons> m_selected_groups;   // groups of selected items for sequential printing
    std::vector<int> m_uncompatible_plates;  // plate indices with different printing sequence than global

    // IMEX zone snapshot, taken on the main thread in prepare().
    //
    // process() runs on the worker thread (see Job::process). PartPlate::imex_primary_zone() is
    // non-const and calls ensure_imex_zones(), which reads wxGetApp().preset_bundle -- main-thread
    // GUI state -- on every call, and on a cache miss calls calc_imex_zones(), which destroys and
    // rebuilds std::vector<GLModel> members. ~GLModel issues glDeleteBuffers/glDeleteVertexArrays,
    // and no GL context is current on the worker, while the GUI thread may be painting those same
    // vectors from GLCanvas3D::on_paint. (imex_collision_zones() is itself const and merely reads
    // the member -- but it is only valid once imex_primary_zone() has warmed the cache, so it
    // cannot be moved off the main thread on its own.) Snapshotting plain geometry here keeps
    // every one of those touches on the main thread.
    //
    // Both are stored already converted to plate-local coordinates, which is the space the
    // arranger works in.
    std::optional<BoundingBoxf> m_imex_primary_zone_local;
    std::vector<BoundingBoxf>   m_imex_collision_zones_local;

    arrangement::ArrangeParams params;
    int current_plate_index = 0;
    Polygon bed_poly;
    Plater *m_plater;

    // BBS: add flag for whether on current part plate
    bool only_on_partplate{false};

    // clear m_selected and m_unselected, reserve space for next usage
    void clear_input();

    // Prepare the selected and unselected items separately. If nothing is
    // selected, behaves as if everything would be selected.
    void prepare_selected();

    void prepare_all();

    //BBS:prepare the items from current selected partplate
    void prepare_partplate();
    void prepare_wipe_tower();

    ArrangePolygon prepare_arrange_polygon(void* instance);

protected:

    void check_unprintable();

public:

    void prepare();

    void process(Ctl &ctl) override;

    ArrangeJob();

    int status_range() const
    {
        // ensure finalize() is called after all operations in process() is finished.
        return int(m_selected.size() + m_unprintable.size() + 1);
    }

    void finalize(bool canceled, std::exception_ptr &e) override;
};

std::optional<arrangement::ArrangePolygon> get_wipe_tower_arrangepoly(const Plater &);

// The gap between logical beds in the x axis expressed in ratio of
// the current bed width.
static const constexpr double LOGICAL_BED_GAP = 1. / 5.;

//BBS: add sudoku-style strides for x and y
// Stride between logical beds
double bed_stride_x(const Plater* plater);
double bed_stride_y(const Plater* plater);

arrangement::ArrangeParams init_arrange_params(Plater *p);

}} // namespace Slic3r::GUI

#endif // ARRANGEJOB_HPP
