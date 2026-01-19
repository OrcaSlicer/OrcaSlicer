#ifndef PLATERWORKER_HPP
#define PLATERWORKER_HPP

#include <map>
#include <chrono>

#include "Worker.hpp"
#include "BusyCursorJob.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r { namespace GUI {

template<class WorkerSubclass>
class PlaterWorker: public Worker {
    WorkerSubclass m_w;
    wxWindow *m_plater;

    class PlaterJob : public Job {
        std::shared_ptr<Job> m_job;
        wxWindow *m_plater;
        long long m_process_duration; // [ms]

    public:
        void process(Ctl &c) override
        {
            // Ensure that wxWidgets processing wakes up to handle outgoing
            // messages in plater's wxIdle handler. Otherwise it might happen
            // that the message will only be processed when an event like mouse
            // move comes along which might be too late.
            struct WakeUpCtl: Ctl {
                Ctl &ctl;
                WakeUpCtl(Ctl &c) : ctl{c} {}

                void update_status(int st, const std::string &msg = "") override
                {
                    ctl.update_status(st, msg);
                    wxWakeUpIdle();
                }

                bool was_canceled() const override { return ctl.was_canceled(); }

                std::future<void> call_on_main_thread(std::function<void()> fn) override
                {
                    auto ftr = ctl.call_on_main_thread(std::move(fn));
                    wxWakeUpIdle();

                    return ftr;
                }

                void clear_percent() override {
                    ctl.clear_percent();
                    wxWakeUpIdle();
                }

                void show_error_info(const std::string &msg, int code, const std::string &description, const std::string &extra) override
                {
                    ctl.show_error_info(msg, code, description, extra);
                    wxWakeUpIdle();
                }

            } wctl{c};

            CursorSetterRAII busycursor{wctl};
            
            using namespace std::chrono;
            steady_clock::time_point process_start = steady_clock::now();
            m_job->process(wctl);
            steady_clock::time_point process_end = steady_clock::now();
            m_process_duration = duration_cast<milliseconds>(process_end - process_start).count();
        }

        void finalize(bool canceled, std::exception_ptr &eptr) override
        {
            using namespace std::chrono;
            steady_clock::time_point finalize_start = steady_clock::now();
            m_job->finalize(canceled, eptr);
            steady_clock::time_point finalize_end = steady_clock::now();
            long long finalize_duration = duration_cast<milliseconds>(finalize_end - finalize_start).count();

            BOOST_LOG_TRIVIAL(info)
                << std::fixed // do not use scientific notations
                << "Job '" << typeid(*m_job).name() << "' "
                << "spend " << m_process_duration + finalize_duration << "ms "
                << "(process " << m_process_duration << "ms + finalize " << finalize_duration << "ms)";

            if (eptr) try {
                std::rethrow_exception(eptr);
            }  catch (std::exception &e) {
                show_error(m_plater, _L("An unexpected error occurred") + ": " + e.what());
                eptr = nullptr;
            }
        }

        PlaterJob(wxWindow *p, std::shared_ptr<Job> j)
            : m_job{std::move(j)}, m_plater{p}
        {
            // TODO: decide if disabling slice button during UI job is what we
            // want.
            //        if (m_plater)
            //            m_plater->sidebar().enable_buttons(false);
        }

        ~PlaterJob() override
        {
            // TODO: decide if disabling slice button during UI job is what we want.

            // Reload scene ensures that the slice button gets properly
            // enabled or disabled after the job finishes, depending on the
            // state of slicing. This might be an overkill but works for now.
            //        if (m_plater)
            //            m_plater->canvas3D()->reload_scene(false);
        }
    };

    EventGuard on_idle_evt;
    EventGuard on_paint_evt;

public:

    template<class... WorkerArgs>
    PlaterWorker(wxWindow *plater, WorkerArgs &&...args)
        : m_w{std::forward<WorkerArgs>(args)...}
        , m_plater{plater}
        // Ensure that messages from the worker thread to the UI thread are
        // processed continuously.
        , on_idle_evt(plater, wxEVT_IDLE, [this](wxIdleEvent&) { process_events(); })
        , on_paint_evt(plater, wxEVT_PAINT, [this](wxPaintEvent&) { process_events(); })
    {
    }

    // Always package the job argument into a PlaterJob
    bool push(std::shared_ptr<Job> job) override
    {
        return m_w.push(std::make_shared<PlaterJob>(m_plater, std::move(job)));
    }

    bool is_idle() const override { return m_w.is_idle(); }
    void cancel() override { m_w.cancel(); }
    void cancel_all() override { m_w.cancel_all(); }
    void process_events() override { m_w.process_events(); }
    bool wait_for_current_job(unsigned timeout_ms = 0) override
    {
        return m_w.wait_for_current_job(timeout_ms);
    }
    bool wait_for_idle(unsigned timeout_ms = 0) override
    {
        return m_w.wait_for_idle(timeout_ms);
    }
};

}} // namespace Slic3r::GUI

#endif // PLATERJOB_HPP
