#ifndef JOB_HPP
#define JOB_HPP

#include <atomic>
#include <exception>
#include <future>

#include <wx/window.h>

#include "libslic3r/libslic3r.h"
#include "ProgressIndicator.hpp"

namespace Slic3r { namespace GUI {

// A class representing a job that is to be run in the background, not blocking
// the main thread. Running it is up to a Worker object (see Worker interface)
class Job {
public:

    enum JobPrepareState {
        PREPARE_STATE_DEFAULT = 0,
        PREPARE_STATE_MENU = 1,
    };

    // A controller interface that informs the job about cancellation and
    // makes it possible for the job to advertise its status.
    class Ctl {
    public:
        virtual ~Ctl() = default;

        // status update, to be used from the work thread (process() method)
        virtual void update_status(int st, const std::string &msg = "") = 0;

        // Returns true if the job was asked to cancel itself.
        virtual bool was_canceled() const = 0;

        // Orca:
        virtual void clear_percent()                                                                                             = 0;
        virtual void show_error_info(const std::string &msg, int code, const std::string &description, const std::string &extra) = 0;

        // Execute a functor on the main thread. Note that the exact time of
        // execution is hard to determine. This can be used to make modifications
        // on the UI, like displaying some intermediate results or modify the
        // cursor.
        // This function returns a std::future<void> object which enables the
        // caller to optionally wait for the main thread to finish the function call.
        virtual std::future<void> call_on_main_thread(std::function<void()> fn) = 0;
    };

    virtual ~Job() = default;

    // The method where the actual work of the job should be defined. This is
    // run on the worker thread.
    virtual void process(Ctl &ctl) = 0;

    // Launched when the job is finished on the UI thread.
    // If the job was cancelled, the first parameter will have a true value.
    // Exceptions occuring in process() are redirected from the worker thread
    // into the main (UI) thread. This method receives the exception and can
    // handle it properly. Assign nullptr to this second argument before
    // function return to prevent further action. Leaving it with a non-null
    // value will result in rethrowing by the worker.
    virtual void finalize(bool /*canceled*/, std::exception_ptr &) {}
};

}} // namespace Slic3r::GUI

#endif // JOB_HPP
