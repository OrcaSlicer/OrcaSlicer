#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/DropOldestQueue.hpp"
#include "libslic3r/FramePacer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>

using Slic3r::DropOldestQueue;
using Slic3r::FramePacer;
using Clock = FramePacer::Clock;
using namespace std::chrono_literals;
using std::chrono::microseconds;
using std::chrono::milliseconds;

namespace {

const Clock::time_point t0 = Clock::time_point() + 1h;

// The queue capacity the liveview uses: about 300 ms of frames, at least one.
size_t capacity_for(int frame_rate) { return static_cast<size_t>(std::max(300 * frame_rate / 1000, 1)); }

microseconds period_of(int frame_rate) { return microseconds(1000000 / frame_rate); }

struct Source
{
    int          frame_rate = 30;
    milliseconds stall{0}; // How long the connection stalls, starting at 2 s.
    milliseconds poll{0};  // 0: frames are received as they come; else the receiver wakes this often.
};

struct Playback
{
    microseconds worst_delay{0};  // From a frame being sent to it being shown.
    microseconds smallest_gap{1h}; // Between two frames being shown.
};

// Plays a live source through the queue and the pacer one millisecond at a time, the way the
// liveview's threads do, and measures the last `settle` of the run. The source sends a frame every
// period. The receiving thread decodes one frame every 5 ms into the queue, either as frames come
// or, with `poll`, everything that came since it last woke. After a stall the frames sent meanwhile
// arrive as a backlog. The display thread pops with a 100 ms timeout and shows frames when the pacer
// says.
Playback play(const Source &source, milliseconds settle)
{
    const auto                    period      = period_of(source.frame_rate);
    const size_t                  capacity    = capacity_for(source.frame_rate);
    const auto                    stall_at    = 2000ms;
    const auto                    decode_time = 5ms;
    const auto                    run_for     = stall_at + source.stall + 5000ms + settle;
    DropOldestQueue<microseconds> queue(capacity); // Each frame is the time it was sent at.
    FramePacer                    pacer(source.frame_rate, capacity);
    std::deque<microseconds>      in_transit; // Sent, not yet received.
    std::deque<microseconds>      received;   // Received, not yet decoded.
    Playback                      result;
    microseconds                  holding{-1};
    auto                          show_at     = t0;
    auto                          idle_for    = 0ms;
    microseconds                  next_send{0};
    auto                          next_decode = 0ms;
    microseconds                  last_shown{-1};
    for (auto now = 0ms; now < run_for; now += 1ms) {
        for (; next_send <= now; next_send += period)
            in_transit.push_back(next_send);
        const bool stalled = now >= stall_at && now < stall_at + source.stall;
        if (!stalled && (source.poll == 0ms || now % source.poll == 0ms)) {
            received.insert(received.end(), in_transit.begin(), in_transit.end());
            in_transit.clear();
        }
        if (!received.empty() && now >= next_decode) {
            queue.push(received.front());
            received.pop_front();
            next_decode = now + decode_time;
        }
        if (holding < 0us) {
            microseconds frame{0};
            if (queue.pop(frame, 0ms)) {
                idle_for            = 0ms;
                holding             = frame;
                const auto schedule = pacer.schedule(t0 + now, queue.size());
                show_at             = schedule.show_at;
                if (schedule.skip_to_newest)
                    while (queue.pop(frame, 0ms))
                        holding = frame;
            } else if ((idle_for += 1ms) >= 100ms) {
                pacer.reset();
                idle_for = 0ms;
            }
        }
        if (holding >= 0us && t0 + now >= show_at) {
            if (now >= run_for - settle) {
                result.worst_delay = std::max(result.worst_delay, microseconds(now) - holding);
                if (last_shown >= 0us)
                    result.smallest_gap = std::min(result.smallest_gap, microseconds(now) - last_shown);
            }
            last_shown = now;
            holding    = microseconds(-1);
        }
    }
    return result;
}

// How far behind a paced stream may settle: half the queue waiting behind the shown frame, plus
// that frame's own period and one more for the frame being held when the next arrives.
microseconds paced_bound(int frame_rate) { return period_of(frame_rate) * static_cast<long long>(capacity_for(frame_rate) / 2 + 2); }

} // namespace

TEST_CASE("Frames are paced one period apart", "[FramePacer]")
{
    FramePacer pacer(30, capacity_for(30));
    const auto first  = pacer.schedule(t0, 0);
    const auto second = pacer.schedule(t0, 0);
    const auto third  = pacer.schedule(t0, 0);
    CHECK(first.show_at == t0);
    CHECK(second.show_at - first.show_at == period_of(30));
    CHECK(third.show_at - second.show_at == period_of(30));
}

TEST_CASE("The first frame skips to the newest one and is shown at once", "[FramePacer]")
{
    FramePacer pacer(30, capacity_for(30));
    const auto schedule = pacer.schedule(t0, 3);
    CHECK(schedule.skip_to_newest);
    CHECK(schedule.show_at == t0);
}

TEST_CASE("A late frame restarts the schedule so the frames after it are paced", "[FramePacer]")
{
    // Frames received in a burst: the first is late, the ones behind it still get a period each.
    FramePacer pacer(30, capacity_for(30));
    pacer.schedule(t0, 0);
    const auto late   = pacer.schedule(t0 + 100ms, 2);
    const auto second = pacer.schedule(t0 + 100ms, 1);
    const auto third  = pacer.schedule(second.show_at, 0);
    CHECK(late.show_at == t0 + 100ms);
    CHECK(second.show_at - late.show_at == period_of(30));
    CHECK(third.show_at - second.show_at == period_of(30));
}

TEST_CASE("A frame with more than half the queue waiting behind it is shown at once", "[FramePacer]")
{
    const size_t capacity = capacity_for(30); // 9
    FramePacer   pacer(30, capacity);
    pacer.schedule(t0, 0);
    const auto paced = pacer.schedule(t0, capacity / 2);
    CHECK(paced.show_at > t0);
    const auto rushed = pacer.schedule(t0, capacity / 2 + 1);
    CHECK_FALSE(rushed.skip_to_newest);
    CHECK(rushed.show_at == t0);
}

TEST_CASE("At a low frame rate frames are shown as they arrive", "[FramePacer]")
{
    // A queue of one or two frames is too short to pace from without holding the picture back.
    const int  frame_rate = GENERATE(1, 5, 9);
    FramePacer pacer(frame_rate, capacity_for(frame_rate));
    pacer.schedule(t0, 0);
    const auto second = pacer.schedule(t0 + 10ms, 0);
    CHECK_FALSE(second.skip_to_newest);
    CHECK(second.show_at == t0 + 10ms);
}

TEST_CASE("A stall longer than the queue's worth skips to the newest frame", "[FramePacer]")
{
    FramePacer pacer(30, capacity_for(30));
    pacer.schedule(t0, 0);
    const auto after_stall = t0 + 2s;
    const auto schedule    = pacer.schedule(after_stall, 0);
    CHECK(schedule.skip_to_newest);
    CHECK(schedule.show_at == after_stall);
}

TEST_CASE("A reset starts a fresh schedule", "[FramePacer]")
{
    FramePacer pacer(30, capacity_for(30));
    pacer.schedule(t0, 0);
    pacer.reset();
    CHECK(pacer.schedule(t0 + 10ms, 0).skip_to_newest);
}

TEST_CASE("After a source stall the picture settles close to live", "[FramePacer]")
{
    const int  frame_rate = GENERATE(30, 15, 5, 1);
    const auto stall      = GENERATE(400ms, 3000ms);
    INFO("frame rate " << frame_rate << ", stall " << stall.count() << " ms");
    const auto playback = play({frame_rate, stall, 0ms}, 3000ms);
    // Below 10 fps frames are not paced, so only the decode stands between sending and showing.
    if (capacity_for(frame_rate) > 2)
        CHECK(playback.worst_delay <= paced_bound(frame_rate));
    else
        CHECK(playback.worst_delay <= microseconds(20ms));
}

TEST_CASE("Frames received in bursts are still shown about a period apart", "[FramePacer]")
{
    // The receiver wakes every `poll` and decodes what came since, so frames reach the queue in bursts.
    const int  frame_rate = GENERATE(30, 15);
    const auto poll       = GENERATE(30ms, 100ms);
    INFO("frame rate " << frame_rate << ", poll " << poll.count() << " ms");
    const auto playback = play({frame_rate, 0ms, poll}, 3000ms);
    CHECK(playback.smallest_gap >= period_of(frame_rate) * 8 / 10);
    CHECK(playback.worst_delay <= paced_bound(frame_rate));
}
