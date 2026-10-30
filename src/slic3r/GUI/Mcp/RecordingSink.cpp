#include "RecordingSink.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// The budget a frame is charged: its pixels, plus a little for what goes with it.
std::size_t charge_of(const SinkFrame& f) { return f.capture.pixels.size() + 4096; }

} // namespace

struct RecordingSink::State
{
    std::unique_ptr<SinkOutput>            output;
    FrameEncoder                           encoder;
    SinkOptions                            options;
    std::function<void(const SinkResult&)> on_finalized;

    std::atomic<std::size_t> charged{0};
    std::atomic<uint64_t>    accepted{0}, written{0}, dropped{0}, failed{0};
    std::atomic<bool>        ending{false}, aborted{false};

    // Queue: held only to push or pop, never across encoding or file I/O.
    struct Item
    {
        uint64_t    seq    = 0;
        std::size_t charge = 0;
        SinkFrame   frame;
    };
    std::mutex              queue_mutex;
    std::condition_variable queue_cv;
    std::deque<Item>        queue;
    uint64_t                next_seq = 0;
    int64_t                 end_ms   = 0;
    std::string             reason;
    int                     alive = 0;

    // Writer: the frames encoded out of order, and the file. Only sink threads take it.
    struct Encoded
    {
        std::vector<uint8_t> jpeg; // empty: the encode failed, a skip
        int64_t              ms     = 0;
        std::size_t          charge = 0;
    };
    std::mutex                  writer_mutex;
    std::map<uint64_t, Encoded> pending;
    uint64_t                    next_write = 0;
    std::vector<uint8_t>        last_jpeg;
    int64_t                     first_ms = -1, last_ms = 0;
    bool                        stopped_writing = false; // write error or size limit
    std::string                 error;

    mutable std::mutex              final_mutex;
    mutable std::condition_variable final_cv;
    bool                            finalized = false;
    SinkResult                      result;

    // An output that threw, or memory running out, in a sink thread: a detached thread must not throw, so the
    // recording ends as on a write error, and the thread goes on to count itself out. Nothing here may throw:
    // the message is best effort, and the reason fits a short string's own buffer.
    void fail(const char* what) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(writer_mutex);
            stopped_writing = true;
            try {
                if (error.empty())
                    error = std::string("Writing the recording failed: ") + what;
            } catch (...) {
            }
        } catch (...) {
        }
        try {
            end(0, "write_error", true);
        } catch (...) {
        }
    }

    // Marks the end unless it is marked already; true when this call did. `drop` also drops what is queued.
    bool end(int64_t ms, const std::string& why, bool drop)
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        const bool first = !ending.exchange(true);
        if (first) {
            end_ms = ms;
            reason = why;
        }
        if (drop && !aborted.exchange(true)) {
            for (const Item& item : queue)
                charged -= item.charge;
            queue.clear();
        }
        queue_cv.notify_all();
        return first;
    }

    // Writes the encoded frames whose turn has come. Writer mutex held. Returns why writing stopped
    // just now, if it did.
    std::string write_ready()
    {
        std::string stopped;
        while (!pending.empty() && pending.begin()->first == next_write) {
            Encoded e = std::move(pending.begin()->second);
            pending.erase(pending.begin());
            ++next_write;
            if (!aborted && !stopped_writing) {
                if (e.jpeg.empty()) {
                    ++failed;
                } else if (std::string err; !output->write(e.ms, e.jpeg, err)) {
                    stopped_writing = true;
                    error           = err;
                    stopped         = "write_error";
                } else {
                    ++written;
                    if (first_ms < 0)
                        first_ms = e.ms;
                    last_ms   = e.ms;
                    last_jpeg = std::move(e.jpeg);
                    if (output->bytes() >= options.size_limit) {
                        stopped_writing = true;
                        stopped         = "size_limit";
                    }
                }
            }
            charged -= e.charge;
        }
        return stopped;
    }

    // Writes the end frame, closes the file and fills in what that tells: size, times, error and why it ended.
    // Only this sink thread is left by now.
    void close_file(SinkResult& r)
    {
        int64_t     end = 0;
        std::string why;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            end = end_ms;
            why = reason;
        }
        std::lock_guard<std::mutex> lock(writer_mutex);
        end = std::max(end, last_ms);
        // Each step on its own, so whatever one throws never skips closing the file. Describing the failure
        // is best effort: without memory for the message, the step failed all the same.
        auto attempt = [&](const char* what, auto&& step) {
            try {
                std::string err;
                step(err);
                if (!err.empty() && error.empty())
                    error = std::move(err);
            } catch (const std::exception& e) {
                try {
                    if (error.empty())
                        error = std::string(what) + e.what();
                } catch (...) {
                }
            } catch (...) {
                try {
                    if (error.empty())
                        error = std::string(what) + "unknown error";
                } catch (...) {
                }
            }
        };
        // Holding the last frame until the end, so the recording lasts as long as it ran.
        if (!aborted && !stopped_writing && !last_jpeg.empty() && end > last_ms)
            attempt("Writing the recording failed: ", [&](std::string& err) {
                if (!output->write(end, last_jpeg, err) && err.empty())
                    err = "Writing the recording failed";
            });
        attempt("Closing the recording failed: ", [&](std::string& err) {
            if (!output->close(end, err) && err.empty())
                err = "Closing the recording failed";
        });
        attempt("Reading the recording's size failed: ", [&](std::string&) { r.bytes = output->bytes(); });
        // A failed write ends the recording, whatever ended it first.
        if (!error.empty())
            why = "write_error";
        r.first_ms = std::max<int64_t>(first_ms, 0);
        r.last_ms  = last_ms;
        r.end_ms   = end;
        r.error    = std::move(error);
        r.ended    = std::move(why);
    }

    void finalize()
    {
        // A detached thread must not throw: whatever goes wrong, the sink still ends as finalized, with the
        // counters, which reading can't fail.
        SinkResult r;
        r.finalized = true;
        try {
            close_file(r);
        } catch (...) {
            r.ended = "write_error";
        }
        // Let go of the file whatever closing did (it can still throw, from its locks or copies), before the
        // owner hears it is finished and releases it.
        output.reset();
        r.accepted = accepted;
        r.written  = written;
        r.dropped  = dropped;
        r.failed   = failed;
        // The owner's bookkeeping of the finished file comes first, so whoever wait_finalized() wakes finds
        // it done (the file listed as finished, its reservation released). Its failure is reported, not
        // thrown: the file is closed whatever the bookkeeping did.
        if (on_finalized) {
            try {
                on_finalized(r);
            } catch (const std::exception& e) {
                try {
                    if (r.error.empty())
                        r.error = std::string("Recording the finished file failed: ") + e.what();
                } catch (...) {
                }
            } catch (...) {
            }
        }
        {
            std::lock_guard<std::mutex> lock(final_mutex);
            finalized = true;
            result    = std::move(r);
        }
        final_cv.notify_all();
    }

    static void run(std::shared_ptr<State> s)
    {
        try {
            for (;;) {
                Item item;
                {
                    std::unique_lock<std::mutex> lock(s->queue_mutex);
                    s->queue_cv.wait(lock, [&] { return !s->queue.empty() || s->ending || s->aborted; });
                    if (s->queue.empty())
                        break;
                    item = std::move(s->queue.front());
                    s->queue.pop_front();
                }
                Encoded e;
                e.ms     = item.frame.ms;
                e.charge = item.charge;
                if (!s->aborted) {
                    try {
                        e.jpeg = s->encoder(item.frame);
                    } catch (const std::exception&) {
                        e.jpeg.clear(); // a skip, like any failed encode
                    }
                }
                item.frame = SinkFrame(); // the pixels go now; the charge stays until the frame is written
                std::string stopped;
                {
                    std::lock_guard<std::mutex> lock(s->writer_mutex);
                    s->pending.emplace(item.seq, std::move(e));
                    stopped = s->write_ready();
                }
                // Ended on its own: no more frames, and a write error drops what is queued.
                if (!stopped.empty())
                    s->end(0, stopped, stopped == "write_error");
            }
        } catch (const std::exception& ex) {
            s->fail(ex.what());
        } catch (...) {
            s->fail("unknown error");
        }
        bool last = false;
        {
            std::lock_guard<std::mutex> lock(s->queue_mutex);
            last = --s->alive == 0;
        }
        if (last)
            s->finalize();
    }
};

RecordingSink::RecordingSink(std::unique_ptr<SinkOutput> output, FrameEncoder encoder, SinkOptions options,
                             std::function<void(const SinkResult&)> on_finalized)
    : m_state(std::make_shared<State>())
{
    m_state->output       = std::move(output);
    m_state->encoder      = std::move(encoder);
    m_state->options      = options;
    m_state->on_finalized = std::move(on_finalized);
    // Each thread is counted before it starts, so none can count out first. One that can't start isn't
    // counted: the others do the work, and with none at all, nothing would close the file, so it is closed
    // here and the construction fails.
    for (int i = 0; i < std::max(1, options.threads); ++i) {
        {
            std::lock_guard<std::mutex> lock(m_state->queue_mutex);
            ++m_state->alive;
        }
        try {
            std::thread(&State::run, m_state).detach();
        } catch (...) {
            bool none = false;
            {
                std::lock_guard<std::mutex> lock(m_state->queue_mutex);
                none = --m_state->alive == 0;
            }
            if (!none)
                break;
            m_state->end(0, "start_failed", true);
            m_state->finalize();
            throw;
        }
    }
}

RecordingSink::~RecordingSink() { abort("abandoned"); }

bool RecordingSink::push(SinkFrame frame)
{
    State& s = *m_state;
    if (s.ending)
        return false;
    const std::size_t cost = charge_of(frame);
    std::size_t       now  = s.charged.load();
    do {
        // One frame always fits, however large, so a recording never stalls on its budget alone.
        if (now != 0 && now + cost > s.options.budget_bytes) {
            ++s.dropped;
            return false;
        }
    } while (!s.charged.compare_exchange_weak(now, now + cost));
    {
        std::lock_guard<std::mutex> lock(s.queue_mutex);
        if (s.ending) {
            s.charged -= cost;
            return false;
        }
        State::Item item;
        item.seq    = s.next_seq++;
        item.charge = cost;
        item.frame  = std::move(frame);
        s.queue.push_back(std::move(item));
        ++s.accepted;
    }
    s.queue_cv.notify_one();
    return true;
}

void RecordingSink::finish(int64_t end_ms, const std::string& reason) { m_state->end(end_ms, reason, false); }

void RecordingSink::abort(const std::string& reason) { m_state->end(0, reason, true); }

bool RecordingSink::wait_finalized(std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(m_state->final_mutex);
    return m_state->final_cv.wait_for(lock, timeout, [&] { return m_state->finalized; });
}

bool RecordingSink::finalized() const
{
    std::lock_guard<std::mutex> lock(m_state->final_mutex);
    return m_state->finalized;
}

bool RecordingSink::ending() const { return m_state->ending; }

SinkResult RecordingSink::result() const
{
    {
        std::lock_guard<std::mutex> lock(m_state->final_mutex);
        if (m_state->finalized)
            return m_state->result;
    }
    SinkResult r;
    r.accepted = m_state->accepted;
    r.written  = m_state->written;
    r.dropped  = m_state->dropped;
    r.failed   = m_state->failed;
    return r;
}

std::size_t RecordingSink::charged() const { return m_state->charged; }

}}} // namespace Slic3r::GUI::Mcp
