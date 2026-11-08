#include "SkeletonJob.hpp"

#include "slic3r/GUI/I18N.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

void SkeletonJob::process(Ctl& ctl)
{
    if (!m_state)
        return;

    ctl.update_status(0, _u8L("Finding pipe centreline"));

    SkeletonProgress prog;
    prog.canceled = [&ctl]() { return ctl.was_canceled(); };
    prog.report   = [this, &ctl](int pct, const std::string& stage) {
        m_state->percent.store(pct);
        {
            std::lock_guard<std::mutex> lk(m_state->mtx);
            m_state->stage = stage;
        }
        // The worker owns the status text; the gizmo panel reads `stage`/`percent` directly, so
        // this is only for the notification area.
        ctl.update_status(pct, _u8L("Finding pipe centreline"));
    };

    SkeletonResult res;
    try {
        res = build_skeleton(m_its, m_params, m_world, &prog);
    } catch (const std::exception& e) {
        // A malformed mesh can throw from deep inside CGAL. That must not take the app down, and
        // it must not look like a cancel either — record it and let the UI say so.
        BOOST_LOG_TRIVIAL(error) << "SkeletonJob: build_skeleton threw: " << e.what();
        std::lock_guard<std::mutex> lk(m_state->mtx);
        m_state->error = e.what();
        m_state->failed.store(true);
        return;
    }

    if (ctl.was_canceled() || res.note == "canceled") {
        m_state->canceled.store(true);
        return;
    }

    std::lock_guard<std::mutex> lk(m_state->mtx);
    m_state->result = std::move(res);
}

void SkeletonJob::finalize(bool canceled, std::exception_ptr& eptr)
{
    if (!m_state)
        return;

    if (canceled)
        m_state->canceled.store(true);

    if (eptr) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "SkeletonJob failed: " << e.what();
            std::lock_guard<std::mutex> lk(m_state->mtx);
            m_state->error = e.what();
            m_state->failed.store(true);
        } catch (...) {
            std::lock_guard<std::mutex> lk(m_state->mtx);
            m_state->error = "unknown error";
            m_state->failed.store(true);
        }
        eptr = nullptr; // handled here; do not let the worker rethrow and kill the app
    }

    // Set last: the gizmo polls `done` and reads everything else once it flips, so the payload has
    // to be fully written before that happens.
    m_state->percent.store(100);
    m_state->done.store(true);
}

}} // namespace Slic3r::GUI
