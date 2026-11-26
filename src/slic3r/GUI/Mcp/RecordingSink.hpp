#pragma once

#include "VideoOverlay.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// A captured frame on its way into a recording.
struct SinkFrame
{
    int64_t         ms = 0; // since the recording started
    RgbImage        capture;
    OverlaySnapshot overlay;
};

// Where encoded frames go: the MKV file in the app, a fake in tests. One sink thread at a time calls it.
class SinkOutput
{
public:
    virtual ~SinkOutput() = default;
    virtual bool     write(int64_t ms, const std::vector<uint8_t>& jpeg, std::string& err) = 0;
    virtual bool     close(int64_t end_ms, std::string& err)                             = 0;
    virtual uint64_t bytes() const                                                       = 0;
};

// Distinct frames per second over a recording `seconds` long, counted over at least a second, so a
// recording a fraction of a second long doesn't read as a high rate.
inline double frames_per_second(uint64_t frames, double seconds) { return double(frames) / (seconds > 1. ? seconds : 1.); }

// Turns a frame into its encoded bytes, empty when that failed. Several threads call it at once, so it
// must use nothing but the frame.
using FrameEncoder = std::function<std::vector<uint8_t>(const SinkFrame&)>;

struct SinkOptions
{
    // Raw frames from push() until written, encoded ones waiting their turn included.
    std::size_t budget_bytes = std::size_t(512) << 20;
    uint64_t    size_limit   = uint64_t(4) << 30;
    int         threads      = 2;
};

struct SinkResult
{
    bool        finalized = false;
    uint64_t    accepted = 0, written = 0, dropped = 0, failed = 0; // frames
    uint64_t    bytes = 0;
    int64_t     first_ms = 0, last_ms = 0, end_ms = 0;
    std::string ended; // why it ended: the reason finish() or abort() gave, write_error or size_limit
    std::string error;
};

// The writing half of a recording: a queue of captured frames bounded by memory, encoder threads, and
// the writes in capture order. push() never waits for encoding or the disk, so the UI thread can call
// it. finish() and abort() only signal; the sink's own threads drain, write and close, and the last one
// to exit finalizes. The threads are detached and share the state, so dropping the sink while they run
// is safe: the state lives until the last of them exits. wx-free.
class RecordingSink
{
public:
    // `on_finalized` runs on the thread that finalized, once the file is closed.
    RecordingSink(std::unique_ptr<SinkOutput> output, FrameEncoder encoder, SinkOptions options = {},
                  std::function<void(const SinkResult&)> on_finalized = {});
    // Aborts without waiting.
    ~RecordingSink();
    RecordingSink(const RecordingSink&)            = delete;
    RecordingSink& operator=(const RecordingSink&) = delete;

    // Queues a frame; false when the sink is ending, or when the frame doesn't fit the memory budget
    // (then it is counted as dropped). Never blocks on encoding or file I/O.
    bool push(SinkFrame frame);
    // No more frames: the queue drains, the last frame is written again at `end_ms` so the recording
    // lasts until then, and the file is closed. Idempotent; the first reason stays. Returns at once.
    void finish(int64_t end_ms, const std::string& reason);
    // Drops the queued frames; the threads exit after the frame each is on, and the last closes the file
    // with what was written. Returns at once.
    void abort(const std::string& reason);
    // True once the file is closed and the result final.
    bool wait_finalized(std::chrono::milliseconds timeout) const;
    bool finalized() const;
    // True once no more frames are taken: finished, aborted, or ended on its own (write error, size limit).
    bool ending() const;
    // The result so far (counters only until finalized), without waiting on the writer.
    SinkResult result() const;
    // Bytes charged against the budget now.
    std::size_t charged() const;

private:
    struct State;
    std::shared_ptr<State> m_state;
};

}}} // namespace Slic3r::GUI::Mcp
