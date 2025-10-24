#ifndef BOOSTTHREADWORKER_HPP
#define BOOSTTHREADWORKER_HPP

#include <boost/variant.hpp>

#include "Worker.hpp"

#include <libslic3r/Thread.hpp>
#include <boost/log/trivial.hpp>

#include "ThreadSafeQueue.hpp"
#include "slic3r/GUI/GUI.hpp"

namespace Slic3r { namespace GUI {

// An implementation of the Worker interface which uses the boost::thread
// API and two thread safe message queues to communicate with the main thread
// back and forth. The queue from the main thread to the worker thread holds the
// job entries that will be performed on the worker. The other queue holds messages
// from the worker to the main thread. These messages include status updates,
// finishing operation and arbitrary functiors that need to be performed
// on the main thread during the jobs execution, like displaying intermediate
// results.
class BoostThreadWorker : public Worker, private Job::Ctl
{
    struct JobEntry // Goes into worker and also out of worker as a finalize msg
    {
        std::shared_ptr<Job> job;
        bool                 canceled = false;
        std::exception_ptr   eptr     = nullptr;
    };

    // A message data for status updates. Only goes from worker to main thread.
    struct StatusInfo { int status; std::string msg; };

    // An arbitrary callback to be called on the main thread. Only from worker
    // to main thread.
    struct MainThreadCallData
    {
        std::function<void()> fn;
        std::promise<void>    promise;
    };

    struct EmptyMessage {};

    class WorkerMessage
    {
    public:
        enum MsgType { Empty, Status, Finalize, MainThreadCall };

    private:
        boost::variant<EmptyMessage, StatusInfo, JobEntry, MainThreadCallData> m_data;

    public:
        WorkerMessage() = default;
        WorkerMessage(int s, std::string txt)
            : m_data{StatusInfo{s, std::move(txt)}}
        {}
        WorkerMessage(JobEntry &&entry) : m_data{std::move(entry)} {}
        WorkerMessage(MainThreadCallData fn) : m_data{std::move(fn)} {}

        int get_type () const { return m_data.which(); }

        void deliver(BoostThreadWorker &runner);
    };

    using JobQueue     = ThreadSafeQueueSPSC<JobEntry>;
    using MessageQueue = ThreadSafeQueueSPSC<WorkerMessage>;

    boost::thread                      m_thread;
    std::atomic<bool>                  m_running{false}, m_canceled{false};
    std::shared_ptr<ProgressIndicator> m_progress;
    JobQueue     m_input_queue;  // from main thread to worker
    MessageQueue m_output_queue; // form worker to main thread
    std::string  m_name;

    void run();

    bool join(int timeout_ms = 0);

protected:
    // Implement Job::Ctl interface:

    void update_status(int st, const std::string &msg = "") override;

    bool was_canceled() const override { return m_canceled.load(); }

    std::future<void> call_on_main_thread(std::function<void()> fn) override;

public:
    explicit BoostThreadWorker(std::shared_ptr<ProgressIndicator> pri,
                               boost::thread::attributes &        attr,
                               const char *                       name = "");

    explicit BoostThreadWorker(std::shared_ptr<ProgressIndicator> pri,
                               boost::thread::attributes &&       attr,
                               const char *                       name = "")
        : BoostThreadWorker{std::move(pri), attr, name}
    {}

    explicit BoostThreadWorker(std::shared_ptr<ProgressIndicator> pri,
                               const char *                       name = "")
        : BoostThreadWorker{std::move(pri), {}, name}
    {}

    ~BoostThreadWorker();

    BoostThreadWorker(const BoostThreadWorker &) = delete;
    BoostThreadWorker(BoostThreadWorker &&)      = delete;
    BoostThreadWorker &operator=(const BoostThreadWorker &) = delete;
    BoostThreadWorker &operator=(BoostThreadWorker &&) = delete;

    bool push(std::shared_ptr<Job> job) override;

    bool is_idle() const override
    {
        // The assumption is that jobs can only be queued from a single main
        // thread from which this method is also called. And the output
        // messages are also processed only in this calling thread. In that
        // case, if the input queue is empty, it will remain so during this
        // function call. If the worker thread is also not running and the
        // output queue is already processed, we can safely say that the
        // worker is dormant.
        return m_input_queue.empty() && !m_running.load() && m_output_queue.empty();
    }

    void cancel() override { m_canceled.store(true); }
    void cancel_all() override { m_input_queue.clear(); cancel(); }

    ProgressIndicator * get_pri() { return m_progress.get(); }
    const ProgressIndicator * get_pri() const  { return m_progress.get(); }

    void clear_percent() override
    {
        if (m_progress) {
            m_progress->clear_percent();
        }
    }

    void show_error_info(const std::string &msg, int code, const std::string &description, const std::string &extra) override
    {
        if (m_progress) {
            m_progress->show_error_info(from_u8(msg), code, from_u8(description), from_u8(extra));
        }
    }

    void process_events() override;
    bool wait_for_current_job(unsigned timeout_ms = 0) override;
    bool wait_for_idle(unsigned timeout_ms = 0) override;

};

}} // namespace Slic3r::GUI

#endif // BOOSTTHREADWORKER_HPP
