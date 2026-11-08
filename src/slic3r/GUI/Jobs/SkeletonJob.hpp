#ifndef SKELETONJOB_HPP
#define SKELETONJOB_HPP

// UNLAYERED_MOD — run the skeleton pipeline off the UI thread.
//
// build_skeleton() is expensive enough to stall the message pump: on a 42908-face intake,
// mean-curvature-flow alone took 126 seconds, and Windows logged AppHangTransient twice while the
// window sat greyed out. Nothing was wrong except that the work was on the UI thread.
//
// Lifetime is deliberately handled with a shared state object rather than a pointer back to the
// gizmo. The gizmo can be closed, or the selection changed, long before a two-minute run finishes;
// with shared state the worker simply fills in a block that nobody is looking at any more, instead
// of writing through a dangling this. The gizmo polls the state each frame and applies the result
// when it is ready, which also keeps every OpenGL call on the main thread where it belongs.

#include "Job.hpp"

#include "libslic3r/SkeletonBuild.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace Slic3r { namespace GUI {

// Shared between the worker and whoever asked for the skeleton.
struct SkeletonJobState
{
    std::atomic<bool> done{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> failed{false};
    std::atomic<int>  percent{0};

    std::mutex     mtx;    // guards stage/result/error below
    std::string    stage;
    std::string    error;
    SkeletonResult result;

    // Identity of the request, so a late result for a different object or a superseded set of
    // parameters can be recognised and dropped rather than applied to whatever is selected now.
    ObjectID source_id;
    int      request_id{0};

    std::string stage_copy()
    {
        std::lock_guard<std::mutex> lk(mtx);
        return stage;
    }
};

using SkeletonJobStatePtr = std::shared_ptr<SkeletonJobState>;

class SkeletonJob : public Job
{
public:
    SkeletonJob(indexed_triangle_set its, SkeletonParams params, Transform3d world,
                SkeletonJobStatePtr state)
        : m_its(std::move(its))
        , m_params(params)
        , m_world(world)
        , m_state(std::move(state))
    {}

    void process(Ctl& ctl) override;
    void finalize(bool canceled, std::exception_ptr& eptr) override;

private:
    // A copy of the mesh, not a reference: the model can be edited or deleted while this runs.
    indexed_triangle_set m_its;
    SkeletonParams       m_params;
    Transform3d          m_world;
    SkeletonJobStatePtr  m_state;
};

}} // namespace Slic3r::GUI

#endif // SKELETONJOB_HPP
