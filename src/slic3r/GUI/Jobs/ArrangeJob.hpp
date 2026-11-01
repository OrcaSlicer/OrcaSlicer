#ifndef ARRANGEJOB_HPP
#define ARRANGEJOB_HPP


#include <map>
#include "libslic3r/Polygon.hpp"
#include <atomic>
#include <exception>
#include <memory>
#include <utility>
#include <optional>
#include <string>
#include <vector>

#include "Job.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/IMEXArrange.hpp"

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
    int m_progress_value = 0;
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
    // The zones are stored already converted to plate-local coordinates, which is the space the
    // arranger works in.
    ImexArrangeInput m_imex;

    arrangement::ArrangeParams params;
    int current_plate_index = 0;
    Polygon bed_poly;
    Plater *m_plater;

    // BBS: add flag for whether on current part plate
    bool only_on_partplate{false};

    // Pack tighter. Stock arrange packs convex hulls. When they waste enough area that
    // sparrow, which packs true outlines, may do better, process() re-arranges the same
    // items with it and keeps that result only if it places more parts or uses fewer
    // plates. A cancel while it runs keeps the stock result.
    bool m_pack_eligible   = false; // the packer is on and applies to this arrange
    bool m_packing         = false; // the sparrow phase was reached
    int  m_stock_score     = -1;
    int  m_packed_score    = -1;    // >= 0 if the sparrow result was kept
    int  m_stock_unplaced  = 0;     // parts a full arrange left off every plate
    int  m_packed_unplaced = 0;

    // Lower is better: plates used, or parts left off the plate for a plate arrange.
    int         score(const ArrangePolygons &selected, const ArrangePolygons &unselected) const;
    int         unplaced(const ArrangePolygons &aps) const;
    bool        predict_better(int stock_score, double bed_area) const;
    std::string done_message(bool canceled) const;
    void        pack_tighter(Ctl &ctl, ArrangePolygons &selected, const Points &bedpts);
    void        apply_result(ArrangePolygons &selected, ArrangePolygons &unselected, ArrangePolygons &locked, ArrangePolygons &unprintable);

    ArrangePolygons m_stock_selected; // kept for the undo step

    // Live view of the sparrow phase. Applied from the transformations saved in
    // prepare(), which finalize() restores before applying the result.
    ArrangePolygons m_live_aps;
    std::vector<arrangement::ArrangeParams::Pose> m_live_stock; // where parts wait until sparrow takes them
    std::vector<std::pair<ModelInstance *, Geometry::Transformation>> m_live_restore;
    // Prime tower state as prepare() found it; restore_live() puts it back with the parts.
    std::vector<double> m_live_tower_x, m_live_tower_y;
    std::vector<Vec2d>  m_live_tower_pos;
    bool                m_live_open = false;
    bool                m_live_used = false;
    // Cleared by finalize(); queued preview updates check it before touching the job.
    std::shared_ptr<std::atomic<bool>> m_live_alive;
    void apply_live(const std::vector<arrangement::ArrangeParams::Pose> &poses);
    void restore_live();

    // clear m_selected and m_unselected, reserve space for next usage
    void clear_input();

    // Prepare the selected and unselected items separately. If nothing is
    // selected, behaves as if everything would be selected.
    void prepare_selected();

    void prepare_all();

    //BBS:prepare the items from current selected partplate
    void prepare_partplate();
    void prepare_wipe_tower();
    void prepare_belt_regions(int num_plates);
    void prepare_imex_zones();

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
