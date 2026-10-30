#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/Mcp/McpJournal.hpp"
#include "slic3r/GUI/Mcp/McpPaths.hpp"
#include "slic3r/GUI/Mcp/McpRecorder.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/GUI/Mcp/MkvWriter.hpp"
#include "slic3r/GUI/Mcp/RecordingSettings.hpp"
#include "slic3r/GUI/Mcp/RecordingSink.hpp"
#include "slic3r/GUI/Mcp/ScreenCapture.hpp"
#include "slic3r/GUI/Mcp/VideoEncode.hpp"
#include "slic3r/GUI/Mcp/VideoOverlay.hpp"
#include "test_utils.hpp"

#include "libslic3r/AppConfig.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>
#include <wx/app.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/init.h>
#include <wx/settings.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <future>
#include <ios>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
namespace fs = boost::filesystem;

namespace {

// ---- a small Matroska reader, enough to check what MkvWriter writes ------------------------------

constexpr uint64_t kUnknown = ~uint64_t(0);

struct Element
{
    uint32_t             id = 0;
    uint64_t             size = 0; // kUnknown when written as unknown
    std::size_t          data = 0; // offset of the payload
    std::vector<Element> children;
};

struct Reader
{
    std::string bytes;

    bool read_id(std::size_t& at, uint32_t& id) const
    {
        if (at >= bytes.size())
            return false;
        const unsigned char first = bytes[at];
        int                 len   = 1;
        while (len <= 4 && !(first & (0x80 >> (len - 1))))
            ++len;
        if (len > 4 || at + len > bytes.size())
            return false;
        id = 0;
        for (int i = 0; i < len; ++i)
            id = (id << 8) | (unsigned char) bytes[at + i];
        at += std::size_t(len);
        return true;
    }
    bool read_size(std::size_t& at, uint64_t& size) const
    {
        if (at >= bytes.size())
            return false;
        const unsigned char first = bytes[at];
        int                 len   = 1;
        while (len <= 8 && !(first & (0x80 >> (len - 1))))
            ++len;
        if (len > 8 || at + len > bytes.size())
            return false;
        uint64_t value   = first & (0xFF >> len);
        bool     all_one = value == (0xFFu >> len);
        for (int i = 1; i < len; ++i) {
            value   = (value << 8) | (unsigned char) bytes[at + i];
            all_one = all_one && (unsigned char) bytes[at + i] == 0xFF;
        }
        at += std::size_t(len);
        size = all_one ? kUnknown : value;
        return true;
    }
    static bool is_master(uint32_t id)
    {
        return id == 0x1A45DFA3 || id == 0x18538067 || id == 0x1549A966 || id == 0x1654AE6B || id == 0xAE || id == 0xE0 || id == 0x1F43B675;
    }
    // The elements from `at` to `end`; an unknown-size Cluster ends where the next Cluster starts.
    std::vector<Element> parse(std::size_t at, std::size_t end) const
    {
        std::vector<Element> out;
        while (at < end) {
            Element e;
            if (!read_id(at, e.id) || !read_size(at, e.size))
                break;
            e.data = at;
            std::size_t stop = e.size == kUnknown ? end : std::min<std::size_t>(end, at + std::size_t(e.size));
            if (e.size == kUnknown && e.id == 0x1F43B675)
                stop = std::min(end, next_cluster(at));
            if (is_master(e.id))
                e.children = parse(at, stop);
            at = stop;
            out.push_back(std::move(e));
        }
        return out;
    }
    std::size_t next_cluster(std::size_t from) const
    {
        const std::string marker("\x1F\x43\xB6\x75", 4);
        const std::size_t at = bytes.find(marker, from);
        return at == std::string::npos ? bytes.size() : at;
    }
    uint64_t uint(const Element& e) const
    {
        uint64_t v = 0;
        for (uint64_t i = 0; i < e.size; ++i)
            v = (v << 8) | (unsigned char) bytes[e.data + i];
        return v;
    }
    std::string str(const Element& e) const { return bytes.substr(e.data, std::size_t(e.size)); }
    double      float64(const Element& e) const
    {
        uint64_t bits = uint(e);
        double   d    = 0.;
        std::memcpy(&d, &bits, sizeof(d));
        return d;
    }
};

const Element* child(const Element& parent, uint32_t id)
{
    for (const Element& c : parent.children)
        if (c.id == id)
            return &c;
    return nullptr;
}

std::string read_file(const fs::path& p)
{
    boost::nowide::ifstream in(p.string().c_str(), std::ios::binary);
    std::ostringstream      ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Block
{
    int64_t     ms = 0;
    std::string data;
};

// Every SimpleBlock of the file, with its absolute time.
std::vector<Block> blocks(const Reader& r, const Element& segment)
{
    std::vector<Block> out;
    for (const Element& cluster : segment.children) {
        if (cluster.id != 0x1F43B675)
            continue;
        const Element* ts = child(cluster, 0xE7);
        REQUIRE(ts != nullptr);
        for (const Element& b : cluster.children) {
            if (b.id != 0xA3)
                continue;
            // track number (one byte vint), int16 relative time, flags, data
            const int16_t rel = int16_t((uint16_t((unsigned char) r.bytes[b.data + 1]) << 8) | (unsigned char) r.bytes[b.data + 2]);
            out.push_back({int64_t(r.uint(*ts)) + rel, r.bytes.substr(b.data + 4, std::size_t(b.size) - 4)});
        }
    }
    return out;
}

// ---- RecordingSink fakes ----------------------------------------------------------------------------

// A gate a fake holds a thread at until the test opens it.
struct Gate
{
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    open    = true;
    int                     waiting = 0;
    void close()
    {
        std::lock_guard<std::mutex> lock(mutex);
        open = false;
    }
    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
    void pass()
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++waiting;
        cv.notify_all();
        cv.wait(lock, [&] { return open; });
        --waiting;
    }
    bool wait_for_waiter(std::chrono::milliseconds timeout = 3000ms)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return waiting > 0; });
    }
};

struct FakeFile
{
    std::mutex                          mutex;
    std::vector<std::pair<int64_t, int64_t>> writes; // (ms, the frame's own ms from its bytes)
    bool                                closed = false;
    int64_t                             closed_at = -1;
    int                                 fail_at = -1; // the write number that fails
    Gate                                gate;
};

class FakeOutput final : public SinkOutput
{
public:
    explicit FakeOutput(std::shared_ptr<FakeFile> file) : m_file(std::move(file)) {}
    bool write(int64_t ms, const std::vector<uint8_t>& jpeg, std::string& err) override
    {
        m_file->gate.pass();
        std::lock_guard<std::mutex> lock(m_file->mutex);
        if (int(m_file->writes.size()) == m_file->fail_at) {
            err = "disk full";
            return false;
        }
        int64_t own = 0;
        std::memcpy(&own, jpeg.data(), sizeof(own));
        m_file->writes.emplace_back(ms, own);
        m_bytes += jpeg.size();
        return true;
    }
    bool close(int64_t end_ms, std::string&) override
    {
        std::lock_guard<std::mutex> lock(m_file->mutex);
        m_file->closed    = true;
        m_file->closed_at = end_ms;
        return true;
    }
    uint64_t bytes() const override { return m_bytes; }

private:
    std::shared_ptr<FakeFile> m_file;
    uint64_t                  m_bytes = 0;
};

// An encoder whose output is the frame's time, so writes can be matched to frames.
FrameEncoder ms_encoder(std::function<void(int64_t ms)> before = {}, std::set<int64_t> failing = {})
{
    return [before, failing](const SinkFrame& f) {
        if (before)
            before(f.ms);
        if (failing.count(f.ms) != 0)
            return std::vector<uint8_t>();
        std::vector<uint8_t> out(sizeof(int64_t));
        std::memcpy(out.data(), &f.ms, sizeof(f.ms));
        return out;
    };
}

SinkFrame frame_at(int64_t ms)
{
    SinkFrame f;
    f.ms      = ms;
    f.capture = RgbImage(10, 10);
    return f;
}

constexpr std::size_t kFrameCharge = 10 * 10 * 3 + 4096;

} // namespace

// ---- MKV ------------------------------------------------------------------------------------------------

TEST_CASE("MkvWriter writes an MJPEG track whose blocks keep their timestamps", "[Mcp][Recording]")
{
    ScopedTemporaryFile tmp(".mkv");
    MkvWriter           w;
    std::string         err;
    REQUIRE(w.open(tmp.path().string(), 640, 400, 10., err));
    // 6 s and 40 s apart start new clusters: relative times are 16-bit.
    const std::vector<int64_t> times{0, 100, 6000, 46000};
    for (int64_t t : times) {
        const std::string payload = "frame" + std::to_string(t);
        REQUIRE(w.add_frame(t, reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), err));
    }
    REQUIRE(w.close(47000, err));

    Reader r{read_file(tmp.path())};
    const std::vector<Element> top = r.parse(0, r.bytes.size());
    REQUIRE(top.size() == 2);
    const Element* doctype = child(top[0], 0x4282);
    REQUIRE(doctype != nullptr);
    CHECK(r.str(*doctype) == "matroska");
    const Element& segment = top[1];
    // Closed: the segment's size is exact and reaches the end of the file.
    REQUIRE(segment.size != kUnknown);
    CHECK(segment.data + segment.size == r.bytes.size());
    const Element* info = child(segment, 0x1549A966);
    REQUIRE(info != nullptr);
    REQUIRE(child(*info, 0x4489) != nullptr);
    CHECK_THAT(r.float64(*child(*info, 0x4489)), WithinAbs(47000., 0.1));
    const Element* tracks = child(segment, 0x1654AE6B);
    REQUIRE(tracks != nullptr);
    const Element* track = child(*tracks, 0xAE);
    REQUIRE(track != nullptr);
    CHECK(r.str(*child(*track, 0x86)) == "V_MJPEG");
    const Element* video = child(*track, 0xE0);
    REQUIRE(video != nullptr);
    CHECK(r.uint(*child(*video, 0xB0)) == 640);
    CHECK(r.uint(*child(*video, 0xBA)) == 400);

    const std::vector<Block> got = blocks(r, segment);
    REQUIRE(got.size() == times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        CHECK(got[i].ms == times[i]);
        CHECK(got[i].data == "frame" + std::to_string(times[i]));
    }
    std::size_t clusters = 0;
    for (const Element& e : segment.children)
        if (e.id == 0x1F43B675) {
            ++clusters;
            CHECK(e.size != kUnknown);
        }
    CHECK(clusters == 3);
}

TEST_CASE("an MKV whose writer never closed it still reads up to its last frame", "[Mcp][Recording]")
{
    ScopedTemporaryFile tmp(".mkv");
    {
        MkvWriter   w;
        std::string err;
        REQUIRE(w.open(tmp.path().string(), 64, 48, 0., err));
        for (int64_t t : {0, 250, 7000}) {
            const std::string payload = "x" + std::to_string(t);
            REQUIRE(w.add_frame(t, reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), err));
        }
        // No close(): as after a crash, the sizes stay unknown.
    }
    Reader                     r{read_file(tmp.path())};
    const std::vector<Element> top = r.parse(0, r.bytes.size());
    REQUIRE(top.size() == 2);
    CHECK(top[1].size == kUnknown);
    const std::vector<Block> got = blocks(r, top[1]);
    REQUIRE(got.size() == 3);
    CHECK(got[2].ms == 7000);
    CHECK(got[2].data == "x7000");
}

// ---- the sink ---------------------------------------------------------------------------------------------

TEST_CASE("the sink writes frames in capture order however its encoders finish", "[Mcp][Recording]")
{
    auto          file = std::make_shared<FakeFile>();
    RecordingSink sink(std::make_unique<FakeOutput>(file), ms_encoder([](int64_t ms) {
                           // Early frames take longest, so the second encoder finishes later frames first.
                           std::this_thread::sleep_for(std::chrono::milliseconds(ms < 30 ? 40 - ms : 1));
                       }));
    for (int64_t ms = 0; ms < 100; ms += 10)
        REQUIRE(sink.push(frame_at(ms)));
    sink.finish(500, "stopped");
    REQUIRE(sink.wait_finalized(5000ms));

    const SinkResult r = sink.result();
    CHECK(r.ended == "stopped");
    CHECK(r.written == 10);
    CHECK(r.end_ms == 500);
    std::lock_guard<std::mutex> lock(file->mutex);
    REQUIRE(file->writes.size() == 11); // the ten frames, and the last held until the end
    for (std::size_t i = 0; i < 10; ++i)
        CHECK(file->writes[i].first == int64_t(i) * 10);
    CHECK(file->writes.back() == std::make_pair(int64_t(500), int64_t(90)));
    CHECK(file->closed);
    CHECK(file->closed_at == 500);
    CHECK(sink.charged() == 0);
}

TEST_CASE("the memory budget counts frames until written, so frames waiting in order count too", "[Mcp][Recording]")
{
    auto file = std::make_shared<FakeFile>();
    Gate first;
    first.close();
    SinkOptions options;
    options.budget_bytes = kFrameCharge * 4;
    RecordingSink sink(std::make_unique<FakeOutput>(file), ms_encoder([&](int64_t ms) {
                           if (ms == 0)
                               first.pass(); // the first frame is held; the other encoder runs ahead
                       }),
                       options);
    REQUIRE(sink.push(frame_at(0)));
    REQUIRE(first.wait_for_waiter());
    int accepted = 1, refused = 0;
    for (int64_t ms = 10; ms < 200; ms += 10) {
        (sink.push(frame_at(ms)) ? accepted : refused)++;
        CHECK(sink.charged() <= options.budget_bytes);
        std::this_thread::sleep_for(2ms);
    }
    CHECK(accepted == 4);
    CHECK(refused > 0);
    CHECK(sink.result().dropped == uint64_t(refused));
    first.release();
    sink.finish(1000, "stopped");
    REQUIRE(sink.wait_finalized(5000ms));
    std::lock_guard<std::mutex> lock(file->mutex);
    // In order, with no hole where frames were dropped.
    REQUIRE(file->writes.size() == 5);
    for (std::size_t i = 1; i < 4; ++i)
        CHECK(file->writes[i].first > file->writes[i - 1].first);
    CHECK(sink.charged() == 0);
}

TEST_CASE("push returns at once while a write is blocked on the disk", "[Mcp][Recording]")
{
    auto file = std::make_shared<FakeFile>();
    file->gate.close();
    SinkOptions options;
    options.budget_bytes = kFrameCharge * 3;
    RecordingSink sink(std::make_unique<FakeOutput>(file), ms_encoder(), options);
    REQUIRE(sink.push(frame_at(0)));
    REQUIRE(file->gate.wait_for_waiter());
    const auto start = std::chrono::steady_clock::now();
    CHECK(sink.push(frame_at(10)));
    CHECK(sink.push(frame_at(20)));
    CHECK_FALSE(sink.push(frame_at(30))); // over the budget: dropped, not waited for
    CHECK(std::chrono::steady_clock::now() - start < 500ms);
    file->gate.release();
    sink.finish(100, "stopped");
    REQUIRE(sink.wait_finalized(5000ms));
    CHECK(sink.result().written == 3);
}

TEST_CASE("finish returns at once, and the sink drains, writes the end and closes", "[Mcp][Recording]")
{
    auto file = std::make_shared<FakeFile>();
    Gate slow;
    slow.close();
    RecordingSink sink(std::make_unique<FakeOutput>(file), ms_encoder([&](int64_t) { slow.pass(); }));
    for (int64_t ms = 0; ms < 50; ms += 10)
        REQUIRE(sink.push(frame_at(ms)));
    REQUIRE(slow.wait_for_waiter());
    std::atomic<bool> returned{false};
    std::thread       other([&] {
        sink.finish(300, "script_ended");
        returned = true;
    });
    other.join();
    CHECK(returned);
    CHECK_FALSE(sink.finalized());
    CHECK_FALSE(sink.push(frame_at(60)));
    slow.release();
    REQUIRE(sink.wait_finalized(5000ms));
    const SinkResult r = sink.result();
    CHECK(r.written == 5);
    CHECK(r.ended == "script_ended");
    std::lock_guard<std::mutex> lock(file->mutex);
    CHECK(file->writes.back().first == 300);
    CHECK(file->closed);
}

TEST_CASE("a write error ends the recording and refuses later frames", "[Mcp][Recording]")
{
    auto file     = std::make_shared<FakeFile>();
    file->fail_at = 2;
    RecordingSink sink(std::make_unique<FakeOutput>(file), ms_encoder());
    for (int64_t ms = 0; ms < 30; ms += 10)
        sink.push(frame_at(ms));
    REQUIRE(sink.wait_finalized(5000ms));
    const SinkResult r = sink.result();
    CHECK(r.ended == "write_error");
    CHECK(r.error == "disk full");
    CHECK(r.written == 2);
    CHECK(sink.ending());
    CHECK_FALSE(sink.push(frame_at(40)));
    CHECK(sink.charged() == 0);
}

TEST_CASE("a failed encode is skipped in order and its budget returns", "[Mcp][Recording]")
{
    auto          file = std::make_shared<FakeFile>();
    RecordingSink sink(std::make_unique<FakeOutput>(file), ms_encoder({}, {10}));
    for (int64_t ms = 0; ms < 40; ms += 10)
        REQUIRE(sink.push(frame_at(ms)));
    sink.finish(40, "stopped");
    REQUIRE(sink.wait_finalized(5000ms));
    const SinkResult r = sink.result();
    CHECK(r.failed == 1);
    CHECK(r.written == 3);
    CHECK(sink.charged() == 0);
    std::lock_guard<std::mutex> lock(file->mutex);
    REQUIRE(file->writes.size() == 4);
    CHECK(file->writes[0].first == 0);
    CHECK(file->writes[1].first == 20);
    CHECK(file->writes[2].first == 30);
}

TEST_CASE("the finished file's bookkeeping is done before anyone waiting hears the sink finished", "[Mcp][Recording]")
{
    auto              file = std::make_shared<FakeFile>();
    Gate              bookkeeping;
    std::atomic<bool> booked{false};
    bookkeeping.close();
    RecordingSink sink(std::make_unique<FakeOutput>(file), ms_encoder(), SinkOptions{}, [&](const SinkResult&) {
        bookkeeping.pass();
        booked = true;
    });
    REQUIRE(sink.push(frame_at(0)));
    sink.finish(10, "stopped");
    REQUIRE(bookkeeping.wait_for_waiter());
    CHECK_FALSE(sink.wait_finalized(200ms)); // the file is closed, but its owner has not booked it yet
    CHECK_FALSE(sink.finalized());
    bookkeeping.release();
    REQUIRE(sink.wait_finalized(5000ms));
    CHECK(booked);
}

namespace {

// An output that throws from the calls named in `throwing` ("write", "second write", "close", "bytes"; "second
// write raw" throws something that is no std::exception), keeps count of what it wrote otherwise, and says
// whether close() ran and whether it was destroyed.
class ThrowingOutput final : public SinkOutput
{
public:
    explicit ThrowingOutput(std::set<std::string> throwing, std::shared_ptr<std::atomic<int>> closed = {},
                            std::shared_ptr<std::atomic<bool>> destroyed = {})
        : m_throwing(std::move(throwing)), m_closed(std::move(closed)), m_destroyed(std::move(destroyed))
    {}
    ~ThrowingOutput() override
    {
        if (m_destroyed)
            *m_destroyed = true;
    }
    bool write(int64_t, const std::vector<uint8_t>& jpeg, std::string&) override
    {
        if (m_throwing.count("write") != 0 || (m_throwing.count("second write") != 0 && ++m_writes == 2))
            throw std::runtime_error("device gone");
        if (m_throwing.count("second write raw") != 0 && ++m_writes == 2)
            throw 42;
        m_bytes += jpeg.size();
        return true;
    }
    bool close(int64_t, std::string&) override
    {
        if (m_closed)
            ++*m_closed;
        if (m_throwing.count("close") != 0)
            throw std::runtime_error("cannot close");
        return true;
    }
    uint64_t bytes() const override
    {
        if (m_throwing.count("bytes") != 0)
            throw std::runtime_error("no size");
        return m_bytes;
    }

private:
    std::set<std::string>              m_throwing;
    std::shared_ptr<std::atomic<int>>  m_closed;
    std::shared_ptr<std::atomic<bool>> m_destroyed;
    int                                m_writes = 0;
    uint64_t                           m_bytes  = 0;
};

} // namespace

TEST_CASE("an output that throws on write ends the recording without ending the process", "[Mcp][Recording]")
{
    RecordingSink sink(std::make_unique<ThrowingOutput>(std::set<std::string>{"write", "close"}), ms_encoder());
    REQUIRE(sink.push(frame_at(0)));
    REQUIRE(sink.wait_finalized(5000ms));
    const SinkResult r = sink.result();
    CHECK(r.ended == "write_error");
    CHECK(r.error.find("device gone") != std::string::npos); // the first error stays
    CHECK(r.accepted == 1);
    CHECK(r.written == 0);
    CHECK_FALSE(sink.push(frame_at(10)));
}

TEST_CASE("an output that throws after the frames are written keeps the result's counts", "[Mcp][Recording]")
{
    // bytes() is asked after every write, so the first one ends the recording; the frame is written by then.
    {
        RecordingSink sink(std::make_unique<ThrowingOutput>(std::set<std::string>{"bytes"}), ms_encoder());
        REQUIRE(sink.push(frame_at(0)));
        REQUIRE(sink.wait_finalized(5000ms));
        const SinkResult r = sink.result();
        CHECK(r.ended == "write_error");
        CHECK(r.error.find("no size") != std::string::npos);
        CHECK(r.written == 1);
        CHECK(r.accepted == 1);
    }
    // close() throws once both frames are in.
    {
        std::atomic<uint64_t> written_seen{0};
        RecordingSink         sink(std::make_unique<ThrowingOutput>(std::set<std::string>{"close"}), ms_encoder(), SinkOptions{},
                                   [&](const SinkResult& r) { written_seen = r.written; });
        REQUIRE(sink.push(frame_at(0)));
        REQUIRE(sink.push(frame_at(10)));
        sink.finish(20, "stopped");
        REQUIRE(sink.wait_finalized(5000ms));
        const SinkResult r = sink.result();
        CHECK(r.ended == "write_error");
        CHECK(r.error.find("cannot close") != std::string::npos);
        CHECK(r.written == 2);
        CHECK(r.end_ms == 20);
        CHECK(written_seen == 2); // the owner's bookkeeping saw the frames too
    }
}

TEST_CASE("an end frame that fails to write still lets the file close, before the owner hears it finished", "[Mcp][Recording]")
{
    auto              closed    = std::make_shared<std::atomic<int>>(0);
    auto              destroyed = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> released_after_close{false};
    RecordingSink     sink(std::make_unique<ThrowingOutput>(std::set<std::string>{"second write"}, closed, destroyed), ms_encoder(),
                           SinkOptions{}, [&](const SinkResult&) { released_after_close = *closed == 1 && *destroyed; });
    REQUIRE(sink.push(frame_at(0)));
    sink.finish(20, "stopped"); // the end frame is the second write, which throws
    REQUIRE(sink.wait_finalized(5000ms));
    const SinkResult r = sink.result();
    CHECK(r.ended == "write_error");
    CHECK(r.error.find("device gone") != std::string::npos);
    CHECK(r.written == 1);
    CHECK(*closed == 1);
    CHECK(released_after_close); // closed and let go of before the owner's bookkeeping ran
}

TEST_CASE("an end frame write that throws something unknown still lets the file close first", "[Mcp][Recording]")
{
    auto              closed    = std::make_shared<std::atomic<int>>(0);
    auto              destroyed = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> released_after_close{false};
    RecordingSink     sink(std::make_unique<ThrowingOutput>(std::set<std::string>{"second write raw"}, closed, destroyed), ms_encoder(),
                           SinkOptions{}, [&](const SinkResult&) { released_after_close = *closed == 1 && *destroyed; });
    REQUIRE(sink.push(frame_at(0)));
    sink.finish(20, "stopped"); // the end frame is the second write, which throws an int
    REQUIRE(sink.wait_finalized(5000ms));
    const SinkResult r = sink.result();
    CHECK(r.ended == "write_error");
    CHECK(r.error.find("unknown error") != std::string::npos);
    CHECK(r.written == 1);
    CHECK(*closed == 1);
    CHECK(released_after_close);
}

TEST_CASE("a bookkeeping callback that throws is reported, and the file is closed", "[Mcp][Recording]")
{
    auto          file = std::make_shared<FakeFile>();
    RecordingSink sink(std::make_unique<FakeOutput>(file), ms_encoder(), SinkOptions{}, [](const SinkResult&) { throw std::bad_alloc(); });
    REQUIRE(sink.push(frame_at(0)));
    sink.finish(10, "stopped");
    REQUIRE(sink.wait_finalized(5000ms));
    const SinkResult r = sink.result();
    CHECK(r.ended == "stopped");
    CHECK(r.written == 1);
    CHECK(r.error.find("Recording the finished file failed") != std::string::npos);
    std::lock_guard<std::mutex> lock(file->mutex);
    CHECK(file->closed);
}

TEST_CASE("an aborted sink dropped by its owner finishes on its own threads", "[Mcp][Recording]")
{
    auto file = std::make_shared<FakeFile>();
    Gate encoding;
    encoding.close();
    std::promise<SinkResult> done;
    auto                     finalized = done.get_future();
    auto                     sink      = std::make_unique<RecordingSink>(
        std::make_unique<FakeOutput>(file), ms_encoder([&](int64_t ms) {
            if (ms == 10)
                encoding.pass();
        }),
        SinkOptions{}, [&](const SinkResult& r) { done.set_value(r); });
    file->gate.close();
    REQUIRE(sink->push(frame_at(0)));
    REQUIRE(file->gate.wait_for_waiter()); // a write held on the disk
    REQUIRE(sink->push(frame_at(10)));
    REQUIRE(encoding.wait_for_waiter()); // and an encode held in flight
    for (int64_t ms = 20; ms < 60; ms += 10)
        sink->push(frame_at(ms));
    const auto start = std::chrono::steady_clock::now();
    sink->abort("app_closing");
    sink.reset(); // the owner lets go while both are held: no joinable thread is destroyed
    CHECK(std::chrono::steady_clock::now() - start < 500ms);
    file->gate.release();
    encoding.release();
    REQUIRE(finalized.wait_for(5s) == std::future_status::ready);
    const SinkResult r = finalized.get();
    CHECK(r.ended == "app_closing");
    std::lock_guard<std::mutex> lock(file->mutex);
    CHECK(file->closed);
    CHECK(file->writes.size() == 1);
}

// ---- frames and overlays -------------------------------------------------------------------------------------

TEST_CASE("the video size fits the long edge and is even", "[Mcp][Recording]")
{
    CHECK(video_size(1920, 1200, 1920) == std::make_pair(1920, 1200));
    CHECK(video_size(3840, 2400, 1920) == std::make_pair(1920, 1200));
    CHECK(video_size(1001, 701, 4000) == std::make_pair(1000, 700));
    CHECK(video_size(1, 1, 320) == std::make_pair(2, 2));
}

TEST_CASE("one transform maps a capture and its pointer into a letterboxed video", "[Mcp][Recording]")
{
    // A 400x100 canvas capture in a 400x200 video: centred, 50 rows of black above and below.
    const FrameTransform t = FrameTransform::fit(400, 100, 400, 200);
    CHECK_THAT(t.scale, WithinAbs(1., 1e-9));
    CHECK_THAT(t.y(0.), WithinAbs(50., 1e-9));
    RgbImage capture(400, 100);
    for (uint8_t& b : capture.pixels)
        b = 200;
    RgbImage video(400, 200);
    scale_into(capture, video, t);
    CHECK(video.at(10, 10)[0] == 0);
    CHECK(video.at(10, 60)[0] == 200);
    // Half size: a pointer at capture pixel (200, 50) lands in the middle of the video.
    const FrameTransform half = FrameTransform::fit(800, 400, 400, 200);
    CHECK_THAT(half.x(200.), WithinAbs(100., 1e-9));
    CHECK_THAT(half.y(50.), WithinAbs(25., 1e-9));
}

TEST_CASE("overlay shapes cover their area and clip at the frame's edges", "[Mcp][Recording]")
{
    RgbImage img(40, 40);
    fill_circle(img, 20, 20, 8, {255, 0, 0}, 1.);
    CHECK(img.at(20, 20)[0] == 255);
    CHECK(img.at(20, 35)[0] == 0);
    // Partly outside: drawn where it is inside, nothing written beyond.
    fill_circle(img, 0, 0, 10, {0, 255, 0}, 1.);
    CHECK(img.at(1, 1)[1] == 255);
    stroke_circle(img, 39, 39, 10, 2, {0, 0, 255}, 1.);
    fill_round_rect(img, -5, 30, 20, 20, 4, {9, 9, 9}, 1.);
    CHECK(img.at(5, 35)[0] == 9);

    RgbImage arrow(40, 40);
    draw_arrow(arrow, 10, 10, 19, 1.);
    CHECK(arrow.at(13, 19)[0] == 255); // well inside: white
    CHECK(arrow.at(10, 10)[0] < 128);  // the tip is outline
    CHECK(arrow.at(5, 5)[0] == 0);      // above and left of the tip: untouched
    CHECK(arrow.at(30, 10)[0] == 0);

    TextMask mask;
    mask.width = mask.height = 2;
    mask.coverage           = {255, 0, 128, 255};
    RgbImage text(4, 4);
    blend_mask(text, mask, 3, 3, {200, 200, 200}, 1.);
    CHECK(text.at(3, 3)[0] == 200);
    blend_mask(text, mask, 0, 0, {200, 200, 200}, 1.);
    CHECK(text.at(1, 0)[0] == 0);
    CHECK(text.at(0, 1)[0] == 100);
}

TEST_CASE("overlays keep their proportion to the app at any scale", "[Mcp][Recording]")
{
    CHECK_THAT(overlay_scale(10, 1., 1.), WithinAbs(1., 1e-9));   // 100 %, the video the capture's size
    CHECK_THAT(overlay_scale(15, 1., 1.), WithinAbs(1.5, 1e-9));  // 150 % on Windows: an em unit of 15
    CHECK_THAT(overlay_scale(10, 2., 0.5), WithinAbs(1., 1e-9));  // a Retina capture halved into the video
    // Letterboxed either way, the scale the pixels are drawn with, not the height ratio.
    CHECK_THAT(overlay_scale(10, 1., FrameTransform::fit(1920, 720, 1920, 1080).scale), WithinAbs(1., 1e-9));
    CHECK_THAT(overlay_scale(10, 1., FrameTransform::fit(960, 1080, 1920, 1080).scale), WithinAbs(1., 1e-9));
    CHECK_THAT(overlay_scale(10, 1., 0.25), WithinAbs(0.75, 1e-9)); // the floor, for a small video
}

TEST_CASE("a caption's spans fit its width, the title first and a failure's error before its detail", "[Mcp][Recording]")
{
    CHECK(fit_spans({100, 50, 30}, {0, 1, 2}, 400, 5, 45) == std::vector<int>{100, 50, 30});
    // A long detail is cut to what the title leaves.
    CHECK(fit_spans({100, 500}, {0, 1}, 300, 5, 45) == std::vector<int>{100, 195});
    // A title wider than the budget is cut, and the detail left out.
    CHECK(fit_spans({400, 50}, {0, 1}, 300, 5, 45) == std::vector<int>{300, 0});
    // A failure (drawn title, detail, error) gives the error its room before the detail.
    CHECK(fit_spans({100, 300, 80}, {0, 2, 1}, 300, 5, 45) == std::vector<int>{100, 110, 80});
    // Too little left: a span that doesn't fit whole is left out, one that does is kept.
    CHECK(fit_spans({100, 300}, {0, 1}, 140, 5, 45) == std::vector<int>{100, 0});
    CHECK(fit_spans({100, 30}, {0, 1}, 140, 5, 45) == std::vector<int>{100, 30});
    // An empty title takes no room and no gap: the detail is the first given room.
    CHECK(fit_spans({0, 500}, {0, 1}, 300, 5, 45) == std::vector<int>{0, 300});

    // The text's room: 40 % of a wide video, less the plate's padding and mark (28 units).
    CHECK_THAT(CaptionMetrics::text_budget(1586, 1.), WithinAbs(0.4 * 1586 - 28., 1e-9));
    // A 320-pixel video at the smallest unit: 240 units rather than 40 %, within the margins.
    CHECK_THAT(CaptionMetrics::text_budget(320, 0.75), WithinAbs(180. - 28. * 0.75, 1e-9));
}

TEST_CASE("the caption is centred at the bottom with its spans in their roles' colours", "[Mcp][Recording]")
{
    auto block = [] {
        auto m      = std::make_shared<TextMask>();
        m->width    = 10;
        m->height   = 4;
        m->coverage.assign(40, 255);
        return std::shared_ptr<const TextMask>(m);
    };
    OverlaySnapshot o;
    o.caption = OverlayCaption{{{block(), SpanRole::Title}, {block(), SpanRole::Detail}}, CaptionStatus::Ok, 1.};
    RgbImage video(200, 60);
    draw_overlay(video, o, FrameTransform{});
    // 53 wide (28 of padding and mark, two 10-pixel spans 5 apart), 12 tall, 12 above the bottom: x 73.5 to
    // 126.5, y 36 to 48; the spans start at 93.5 and 108.5, 4 below the plate's top.
    CHECK(video.at(72, 42)[1] == 0);
    CHECK(video.at(127, 42)[1] == 0);
    CHECK(video.at(80, 38)[1] > 15); // the plate
    CHECK(video.at(96, 41)[1] == 181);  // the title before a detail: quiet
    CHECK(video.at(112, 41)[1] == 247); // the detail
    CHECK(video.at(84, 41)[1] == 184);  // done: a teal dot

    OverlaySnapshot failed;
    failed.caption = OverlayCaption{{{block(), SpanRole::Title}, {block(), SpanRole::Error}}, CaptionStatus::Failed, 1.};
    RgbImage f(200, 60);
    draw_overlay(f, failed, FrameTransform{});
    CHECK(f.at(112, 41)[1] == 194); // the error's colour
    CHECK(f.at(84, 41)[0] == 255);  // a coral dot

    OverlaySnapshot alone;
    alone.caption = OverlayCaption{{{block(), SpanRole::Title}}, CaptionStatus::Running, 1.};
    RgbImage a(200, 60);
    draw_overlay(a, alone, FrameTransform{});
    // A title alone says it all; 38 wide, its text from x 101.
    CHECK(a.at(103, 41)[1] == 247);
}

TEST_CASE("caption text is drawn as tall as asked, and no wider than its room", "[Mcp][Recording]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("This test needs a display (for example, xvfb-run).");
#endif
    if (wxTheApp == nullptr)
        wxApp::SetInstance(new wxApp);
    wxInitializer init;
    REQUIRE(init.IsOk());
    const wxFont base = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    // The line heights a 100 % display, a 150 % one and a small video ask for, whatever DPI the memory DC
    // draws at.
    for (const int line_height : {19, 29, 14}) {
        const std::shared_ptr<const TextMask> mask = render_text("Hg", font_with_line_height(base, line_height), 0);
        REQUIRE(mask);
        CHECK(mask->height >= line_height - 1);
        CHECK(mask->height <= line_height + 1);
    }
    // A very long value drawn to its room: the bitmap is no wider.
    const std::shared_ptr<const TextMask> cut = render_text(std::string(5000, 'x'), font_with_line_height(base, 19), 200);
    REQUIRE(cut);
    CHECK(cut->width <= 200);
}

TEST_CASE("the pointer's ring leaves what it circles uncovered", "[Mcp][Recording]")
{
    OverlaySnapshot o;
    o.pointer   = true;
    o.pointer_x = 30.;
    o.pointer_y = 30.;
    RgbImage video(60, 60);
    draw_overlay(video, o, FrameTransform{});
    CHECK(video.at(41, 30)[0] > 200); // on the ring, 11 from the tip
    CHECK(video.at(22, 30)[0] == 0);  // inside it, left of the arrow
}

TEST_CASE("a still frame repeats the last one only while its overlay is still", "[Mcp][Recording]")
{
    OverlaySnapshot still;
    still.pointer       = true;
    still.pointer_alpha = 1.;
    const auto same     = [] { return true; };
    CHECK(repeats_last_frame(still, still, 200ms, 1s, same));
    CHECK_FALSE(repeats_last_frame(still, still, 1s, 1s, same)); // the heartbeat
    CHECK_FALSE(repeats_last_frame(still, still, 200ms, 1s, [] { return false; }));

    // Unchanged pixels under a ripple, or a fading caption, are still new frames: the overlay moves.
    OverlaySnapshot rippling = still;
    rippling.ripples.push_back({5., 5., 0.5, PointerButton::Left});
    CHECK_FALSE(repeats_last_frame(rippling, rippling, 200ms, 1s, same));
    OverlaySnapshot fading = still;
    fading.caption         = OverlayCaption{{}, CaptionStatus::Ok, 0.5};
    CHECK_FALSE(repeats_last_frame(fading, fading, 200ms, 1s, same));
    OverlaySnapshot moved = still;
    moved.pointer_x       = 3.;
    CHECK_FALSE(repeats_last_frame(moved, still, 200ms, 1s, same));
}

TEST_CASE("frames render the same on two threads at once as one after the other", "[Mcp][Recording]")
{
    RgbImage a(64, 48), b(64, 48);
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        a.pixels[i] = uint8_t(i * 7);
        b.pixels[i] = uint8_t(i * 13);
    }
    OverlaySnapshot o;
    o.pointer   = true;
    o.pointer_x = 20.;
    o.pointer_y = 20.;
    o.ripples.push_back({10., 10., 0.3, PointerButton::Right});
    const std::vector<uint8_t> serial_a = render_frame(a, o, 64, 48, 80);
    const std::vector<uint8_t> serial_b = render_frame(b, OverlaySnapshot(), 32, 24, 60);
    REQUIRE_FALSE(serial_a.empty());
    std::vector<uint8_t> parallel_a, parallel_b;
    std::thread          ta([&] { parallel_a = render_frame(a, o, 64, 48, 80); });
    std::thread          tb([&] { parallel_b = render_frame(b, OverlaySnapshot(), 32, 24, 60); });
    ta.join();
    tb.join();
    CHECK(parallel_a == serial_a);
    CHECK(parallel_b == serial_b);
    // A JPEG of the video's size: SOF0 holds height and width.
    const uint8_t sof[] = {0xFF, 0xC0};
    auto          at    = std::search(serial_a.begin(), serial_a.end(), std::begin(sof), std::end(sof));
    REQUIRE(at != serial_a.end());
    const std::size_t p = std::size_t(at - serial_a.begin());
    CHECK(((serial_a[p + 5] << 8) | serial_a[p + 6]) == 48);
    CHECK(((serial_a[p + 7] << 8) | serial_a[p + 8]) == 64);
}

TEST_CASE("a libjpeg error ends only the encode", "[Mcp][Recording]")
{
    const uint8_t  pixel[3] = {1, 2, 3};
    unsigned char* out      = reinterpret_cast<unsigned char*>(1);
    unsigned long  size     = 1;
    // Wider than libjpeg allows: it fails in jpeg_start_compress, before reading a row.
    CHECK_FALSE(encode_jpeg(pixel, 70000, 1, 3, 80, &out, &size));
    CHECK(out == nullptr);
    CHECK(size == 0);
}

TEST_CASE("a canvas is not pasted over the windows an X11 capture drew on top of it", "[Mcp][Recording]")
{
    wxImage dst(10, 10, true); // black
    wxImage src(10, 10);
    std::fill(src.GetData(), src.GetData() + 300, (unsigned char) 255);
    composite(dst, src, wxPoint(0, 0), {wxRect(2, 2, 3, 3)});
    CHECK(dst.GetRed(1, 1) == 255);
    CHECK(dst.GetRed(2, 2) == 0);
    CHECK(dst.GetRed(4, 4) == 0);
    CHECK(dst.GetRed(5, 4) == 255);
    CHECK(dst.GetRed(9, 9) == 255);
}

// ---- captions ------------------------------------------------------------------------------------------------------

TEST_CASE("the caption is the newest call that runs or lingers", "[Mcp][Recording]")
{
    CaptionBoard board;
    const auto   t0 = CaptionBoard::Clock::now();
    CHECK_FALSE(board.current(t0).has_value());
    board.call_started(1, "Run a script", "demo.json", t0);
    REQUIRE(board.current(t0).has_value());
    CHECK(board.current(t0)->id == 1);
    CHECK(board.current(t0)->status == CaptionStatus::Running);

    // A step replaces the script at once, and lingers after it ends.
    board.call_started(2, "Slice", "", t0 + 100ms);
    CHECK(board.current(t0 + 100ms)->id == 2);
    board.call_ended(2, true, "", t0 + 1s);
    std::optional<CaptionBoard::Line> line = board.current(t0 + 3s);
    REQUIRE(line.has_value());
    CHECK(line->id == 2);
    CHECK(line->status == CaptionStatus::Ok);
    CHECK(line->alpha == 1.);
    line = board.current(t0 + 5250ms); // half way through its fade
    REQUIRE(line.has_value());
    CHECK(line->id == 2);
    CHECK_THAT(line->alpha, WithinAbs(0.5, 0.01));

    // Faded: the script, still running, shows again, and lingers in turn when it ends.
    line = board.current(t0 + 5500ms);
    REQUIRE(line.has_value());
    CHECK(line->id == 1);
    CHECK(line->alpha == 1.);
    board.call_ended(1, false, "No such file", t0 + 6s);
    line = board.current(t0 + 7s);
    REQUIRE(line.has_value());
    CHECK(line->id == 1);
    CHECK(line->status == CaptionStatus::Failed);
    CHECK(line->detail == "demo.json");
    CHECK(line->error == "No such file");
    CHECK_FALSE(board.current(t0 + 10500ms).has_value());
}

TEST_CASE("a burst of quick steps never pushes out the script still running", "[Mcp][Recording]")
{
    CaptionBoard board;
    const auto   t0 = CaptionBoard::Clock::now();
    board.call_started(1, "Run a script", "long.json", t0);
    for (uint64_t id = 2; id <= 101; ++id) {
        board.call_started(id, "Mouse gesture on the 3D view", "drag", t0 + 10ms * id);
        board.call_ended(id, true, "", t0 + 10ms * id + 5ms);
    }
    REQUIRE(board.current(t0 + 1100ms).has_value());
    CHECK(board.current(t0 + 1100ms)->id == 101);
    // The last step has faded: the script shows again.
    const std::optional<CaptionBoard::Line> line = board.current(t0 + 6s);
    REQUIRE(line.has_value());
    CHECK(line->id == 1);
    CHECK(line->status == CaptionStatus::Running);
}

TEST_CASE("a failure's caption hides what the call's caption hides", "[Mcp][Recording]")
{
    const std::string dots = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";
    // set_settings quotes a value it can't take; a sensitive one never shows.
    const json settings = {{"scope", "printer"}, {"values", {{"printhost_apikey", json::array({{{"value", "SECRET42"}}})}}}};
    CHECK(caption_error("set_settings", settings, R"(Unsupported value [{"value":"SECRET42"}])") ==
          "Unsupported value [{\"value\":\"" + dots + "\"}]");
    // A value typed into a field found by its label may be a password.
    CHECK(caption_error("ui_input", {{"target", {{"label", "Password"}}}, {"value", "hunter22"}}, "Cannot set hunter22") ==
          "Cannot set " + dots);
    // A setting field's value is shown in the caption, so its error stays as it is.
    CHECK(caption_error("ui_input", {{"target", {{"option", "layer_height"}}}, {"value", "0.33"}}, "0.33 is out of range") ==
          "0.33 is out of range");
    CHECK(caption_error("ui_key", {{"text", "pa55word"}}, "typed pa55word into nothing") == "typed " + dots + " into nothing");
    // A secret in a step of an inline script is found under its key.
    const json script = {{"script", {{"orca_script", 1}, {"steps", json::array({{{"tool", "set_settings"}, {"args", {{"values", {{"printhost_password", "letmein9"}}}}}}})}}}};
    CHECK(caption_error("run_script", script, "step 1 failed: letmein9 rejected") == "step 1 failed: " + dots + " rejected");
    // Too short to take out safely: the error is left out.
    CHECK(caption_error("ui_key", {{"text", "ab"}}, "typed ab").empty());
    // As JSON quotes it inside an error that dumps the arguments.
    const json quoted = {{"values", {{"printhost_apikey", "alpha\"omega"}}}};
    CHECK(caption_error("set_settings", quoted, R"(Unsupported value "alpha\"omega")") == "Unsupported value \"" + dots + "\"");
    // Masked in the whole error before it is shortened, so no start of a value is left past the cut.
    const std::string long_error = std::string(95, 'x') + " SECRETVALUE";
    const std::string masked     = shorten(caption_error("ui_key", {{"text", "SECRETVALUE"}}, long_error), 100);
    CHECK(masked.find("SECRE") == std::string::npos);
}

TEST_CASE("an error is shortened to its first line at a character boundary", "[Mcp][Recording]")
{
    CHECK(shorten("short\nsecond line", 100) == "short");
    CHECK(shorten("abcdef", 4) == "abcd...");
    // "\xE2\x80\xA2" is one character of three bytes: a cut inside it backs up to before it.
    CHECK(shorten("ab\xE2\x80\xA2" "cd", 3) == "ab...");
    CHECK(shorten("ab\xE2\x80\xA2" "cd", 5) == "ab\xE2\x80\xA2...");
}

TEST_CASE("captions name what a call acts on and never show secrets", "[Mcp][Recording]")
{
    CHECK(caption_detail("ui_click", {{"target", {{"label", "Slice plate"}}}}) == "Slice plate");
    CHECK(caption_detail("ui_click", {{"target", {{"menu", "File>Export>Export G-code"}}}}) == "File > Export > Export G-code");
    CHECK(caption_detail("ui_click", {{"target", {{"row", "Plate 1>Cube"}, {"column", "Printable"}}}}) == "Plate 1 > Cube (Printable)");
    CHECK(caption_detail("ui_click", {{"target", "w12"}}).empty()); // a ref: its locator comes later
    CHECK(caption_detail("ui_input", {{"target", {{"option", "layer_height"}}}, {"value", 0.2}}) == "layer_height = 0.2");
    CHECK(caption_detail("ui_input", {{"target", {{"option", "printhost_apikey"}}}, {"value", "s3cret"}}) == "printhost_apikey");
    CHECK(caption_detail("ui_input", {{"target", {{"label", "Password"}}}, {"value", "s3cret"}}) == "Password");
    CHECK(caption_detail("ui_input", {{"target", "w3"}, {"value", "s3cret"}}).empty());
    CHECK(caption_detail("ui_key", {{"keys", "ctrl+s"}, {"text", "s3cret"}}) == "ctrl+s");
    CHECK(caption_detail("ui_key", {{"text", "s3cret"}}).empty());
    const std::string settings =
        caption_detail("set_settings", {{"scope", "printer"}, {"values", {{"printhost_password", "s3cret"}, {"nozzle_diameter", "0.4"}}}});
    CHECK(settings.find("s3cret") == std::string::npos);
    CHECK(settings.find("nozzle_diameter = 0.4") != std::string::npos);
    CHECK(caption_detail("open_files", {{"paths", {"C:/models/part.stl", "/tmp/b.3mf"}}}) == "part.stl, b.3mf");
    CHECK(caption_detail("run_action", {{"id", "orca_command:obj_arrange:orca"}}) == "obj_arrange");
    CHECK(caption_detail("webview_eval", {{"script", "login('s3cret')"}}).empty());
    CHECK(is_sensitive_key("printhost_apikey"));
    CHECK(is_sensitive_key("cloud_Token"));
    CHECK_FALSE(is_sensitive_key("layer_height"));
}

// ---- encoding ------------------------------------------------------------------------------------------------------

TEST_CASE("the ffmpeg command keeps the capture timing and tags H.265 for Apple players", "[Mcp][Recording]")
{
    auto has = [&](const std::vector<std::string>& args, const std::string& flag, const std::string& value) {
        for (std::size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == flag && args[i + 1] == value)
                return true;
        return false;
    };
    EncodeSpec spec;
    spec.input                 = "in.mkv";
    spec.output                = "out.part";
    spec.timing                = {true, "demux"};
    std::vector<std::string> a = encode_args(spec);
    CHECK(has(a, "-c:v", "libx265"));
    CHECK(has(a, "-tag:v", "hvc1"));
    CHECK(has(a, "-fps_mode", "vfr"));
    CHECK(has(a, "-enc_time_base:v", "demux"));
    CHECK(has(a, "-pix_fmt", "yuv420p"));
    CHECK(has(a, "-color_range", "tv"));
    CHECK(has(a, "-f", "mp4"));
    CHECK(has(a, "-crf", "28"));
    CHECK(a.back() == "out.part");

    // -vsync and the time base are chosen apart; no time base option when there is none.
    spec.codec  = "h264";
    spec.crf    = 23;
    spec.timing = {false, "-1"};
    a           = encode_args(spec);
    CHECK(has(a, "-c:v", "libx264"));
    CHECK(has(a, "-vsync", "vfr"));
    CHECK(has(a, "-enc_time_base:v", "-1"));
    CHECK(std::find(a.begin(), a.end(), "hvc1") == a.end());
    spec.timing = {false, ""};
    a           = encode_args(spec);
    CHECK(std::find(a.begin(), a.end(), "-enc_time_base:v") == a.end());
    CHECK(encoder_for("vp9").empty());
}

TEST_CASE("the ffmpeg command takes the speed and profile, and main10 encodes 10 bits", "[Mcp][Recording]")
{
    auto has = [&](const std::vector<std::string>& args, const std::string& flag, const std::string& value) {
        for (std::size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == flag && args[i + 1] == value)
                return true;
        return false;
    };
    EncodeSpec spec;
    spec.input  = "in.mkv";
    spec.output = "out.part";
    std::vector<std::string> a = encode_args(spec);
    CHECK(has(a, "-preset", "medium"));
    CHECK(std::find(a.begin(), a.end(), "-profile:v") == a.end());

    spec.speed   = "veryslow";
    spec.profile = "main10";
    a            = encode_args(spec);
    CHECK(has(a, "-preset", "veryslow"));
    CHECK(has(a, "-profile:v", "main10"));
    CHECK(has(a, "-pix_fmt", "yuv420p10le"));

    spec.codec   = "h264";
    spec.profile = "baseline";
    a            = encode_args(spec);
    CHECK(has(a, "-profile:v", "baseline"));
    CHECK(has(a, "-pix_fmt", "yuv420p"));

    CHECK(codec_profiles("h264").front() == "high");
    CHECK(codec_profiles("h265").front() == "main");
    CHECK(codec_profiles("vp9").empty());
    CHECK(encoder_speeds().front() == "ultrafast");
    CHECK(encoder_speeds().back() == "veryslow");
}

TEST_CASE("each encode preset is matched by its own fields, and a changed field makes them custom", "[Mcp][Recording]")
{
    for (const EncodePreset& p : encode_presets()) {
        INFO(p.key);
        RecordingSettings s;
        apply_encode_preset(s, p);
        CHECK(matching_encode_preset(s) == p.key);
        const std::vector<std::string> profiles = codec_profiles(s.codec);
        CHECK(std::find(profiles.begin(), profiles.end(), s.profile) != profiles.end());
        s.crf += 1;
        CHECK(matching_encode_preset(s) == "custom");
    }
    // The defaults are the Balanced preset, with encoding off until the person turns it on.
    const RecordingSettings defaults;
    CHECK(matching_encode_preset(defaults) == "balanced");
    CHECK_FALSE(defaults.encode);
    CHECK(defaults.folder.empty());
}

TEST_CASE("remembered recording settings come back, and a bad value keeps its default", "[Mcp][Recording]")
{
    Slic3r::AppConfig config;
    RecordingSettings s;
    s.quality = 90;
    s.encode  = true;
    s.codec   = "h264";
    s.crf     = 20;
    s.speed   = "slow";
    s.profile = "baseline";
    s.folder  = "/somewhere/videos";
    save_recording_settings(config, s);
    CHECK(load_recording_settings(config) == s);

    config.set("recording", "quality", "200");
    config.set("recording", "crf", "x");
    config.set("recording", "codec", "vp9");
    config.set("recording", "speed", "warp");
    const RecordingSettings loaded = load_recording_settings(config);
    const RecordingSettings defaults;
    CHECK(loaded.quality == defaults.quality);
    CHECK(loaded.crf == defaults.crf);
    CHECK(loaded.codec == defaults.codec);
    CHECK(loaded.speed == defaults.speed);
    // H.265 has no baseline profile: its default instead.
    CHECK(loaded.profile == "main");
}

TEST_CASE("Start Recording's parameter overrides the settings, a preset before the fields", "[Mcp][Recording]")
{
    RecordingSettings s;
    bool              silent = false;
    CHECK(apply_recording_overrides(s, json{{"encode_preset", "compatible"}, {"crf", 18}, {"quality", 60}, {"silent", true}}, silent).empty());
    CHECK(s.codec == "h264");
    CHECK(s.profile == "high");
    CHECK(s.crf == 18);
    CHECK(s.quality == 60);
    CHECK(silent);

    // A codec without a profile takes the codec's default when the one it had doesn't fit.
    CHECK(apply_recording_overrides(s, json{{"codec", "h265"}}, silent).empty());
    CHECK(s.profile == "main");
    CHECK_FALSE(silent);
    CHECK(apply_recording_overrides(s, json{{"profile", "main10"}}, silent).empty());
    CHECK(s.profile == "main10");

    // Anything wrong leaves the settings as they were.
    const RecordingSettings before = s;
    CHECK_FALSE(apply_recording_overrides(s, json{{"codec", "h264"}, {"profile", "main10"}}, silent).empty());
    CHECK_FALSE(apply_recording_overrides(s, json{{"crf", 52}}, silent).empty());
    CHECK_FALSE(apply_recording_overrides(s, json{{"quality", "high"}}, silent).empty());
    CHECK_FALSE(apply_recording_overrides(s, json{{"encode_preset", "tiny"}}, silent).empty());
    CHECK_FALSE(apply_recording_overrides(s, json{{"fps", 30}}, silent).empty());
    CHECK_FALSE(apply_recording_overrides(s, json::array(), silent).empty());
    CHECK_FALSE(apply_recording_overrides(s, json{{"folder", "relative/videos"}}, silent).empty());
    CHECK_FALSE(apply_recording_overrides(s, json{{"folder", path_to_utf8(fs::temp_directory_path() / "no-such-folder-here")}}, silent).empty());
    CHECK(s == before);

    CHECK(apply_recording_overrides(s, json{{"folder", path_to_utf8(fs::temp_directory_path())}}, silent).empty());
    CHECK(RecordingFiles::same_file(path_from_utf8(s.folder), fs::temp_directory_path()));

    const json args = encode_arguments(s, "C:/videos/a.mkv");
    CHECK(args["path"] == "C:/videos/a.mkv");
    CHECK(args["codec"] == "h265");
    CHECK(args["profile"] == "main10");
}

TEST_CASE("a capture is named for its time, and a name in use gets a number", "[Mcp][Recording]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("orca-names-%%%%-%%%%");
    fs::create_directories(dir);
    struct Remove
    {
        fs::path dir;
        ~Remove()
        {
            boost::system::error_code ec;
            fs::remove_all(dir, ec);
        }
    } remove{dir};
    const auto touch = [&](const std::string& name) { boost::nowide::ofstream(path_to_utf8(dir / name).c_str()) << "x"; };

    // OrcaSlicer-<kind>-YYYYMMDD-HHMMSS<extension>, in local time.
    const std::time_t when  = std::time(nullptr);
    const fs::path    first = timestamped_path(dir, "recording", ".mkv", {".mp4"}, when);
    const std::string name  = path_to_utf8(first.filename());
    REQUIRE(name.size() == std::string("OrcaSlicer-recording-20261030-165405.mkv").size());
    CHECK(name.rfind("OrcaSlicer-recording-", 0) == 0);
    CHECK(first.parent_path() == dir);

    touch(name);
    const std::string stem = name.substr(0, name.size() - 4);
    CHECK(path_to_utf8(timestamped_path(dir, "recording", ".mkv", {".mp4"}, when).filename()) == stem + "-2.mkv");
    // A file made from a capture's name takes the name too: the encode of a recording whose .mkv was deleted.
    touch(stem + "-2.mp4");
    CHECK(path_to_utf8(timestamped_path(dir, "recording", ".mkv", {".mp4"}, when).filename()) == stem + "-3.mkv");
    // Without companions, only the file itself counts.
    CHECK(path_to_utf8(timestamped_path(dir, "recording", ".mp4", {}, when).filename()) == stem + ".mp4");
}

TEST_CASE("each ffmpeg version gets the timing options it accepts", "[Mcp][Recording]")
{
    using V    = std::vector<EncodeTiming>;
    auto for_v = [](const char* banner) { return timing_candidates(parse_ffmpeg_version(banner)); };
    CHECK(for_v("ffmpeg version 9.0.2-full_build") == V{{true, "demux"}});
    CHECK(for_v("ffmpeg version 6.1.1") == V{{true, "demux"}});
    CHECK(for_v("ffmpeg version 6.0") == V{{true, "-1"}});
    CHECK(for_v("ffmpeg version 5.1.2") == V{{true, "-1"}});
    CHECK(for_v("ffmpeg version 4.4.2-0ubuntu0.22.04.1") == V{{false, "-1"}});
    CHECK(for_v("ffmpeg version 3.4.8") == V{{false, "-1"}});
    CHECK(for_v("ffmpeg version 3.3") == V{{false, ""}});
    // A git build: the current spellings, then older ones if it refuses them.
    CHECK(for_v("ffmpeg version N-112233-gabcdef") == V{{true, "demux"}, {true, "-1"}, {false, "-1"}});
    CHECK(refused_timing_option("[vost#0:0/libx265 @ 0000] Invalid time base: -1"));
    CHECK(refused_timing_option("Unrecognized option 'fps_mode'."));
    CHECK_FALSE(refused_timing_option("in.mkv: No such file or directory"));
}

namespace {

std::string be(uint64_t value, int bytes)
{
    std::string out;
    for (int i = bytes - 1; i >= 0; --i)
        out.push_back(char((value >> (8 * i)) & 0xFF));
    return out;
}

std::string box(const std::string& type, const std::string& payload) { return be(8 + payload.size(), 4) + type + payload; }

} // namespace

TEST_CASE("an MP4's length is read from its movie header", "[Mcp][Recording]")
{
    ScopedTemporaryFile tmp(".mp4");
    auto                write = [&](const std::string& bytes) {
        boost::nowide::ofstream out(tmp.string().c_str(), std::ios::binary | std::ios::trunc);
        out << bytes;
    };
    // Version 0: 32-bit times. The movie header sits inside moov, after other boxes.
    const std::string mvhd0 = box("mvhd", be(0, 4) + be(0, 4) + be(0, 4) + be(1000, 4) + be(3034, 4) + std::string(80, '\0'));
    write(box("ftyp", "isom") + box("free", "") + box("moov", box("trak", "") + mvhd0) + box("mdat", "xxxx"));
    REQUIRE(mp4_duration(tmp.path()).has_value());
    CHECK_THAT(*mp4_duration(tmp.path()), WithinAbs(3.034, 1e-9));
    // Version 1: 64-bit times.
    const std::string mvhd1 = box("mvhd", be(0x01000000, 4) + be(0, 8) + be(0, 8) + be(90000, 4) + be(450000, 8) + std::string(80, '\0'));
    write(box("ftyp", "isom") + box("moov", mvhd1));
    CHECK_THAT(mp4_duration(tmp.path()).value_or(0.), WithinAbs(5., 1e-9));
    write("not an mp4 at all");
    CHECK_FALSE(mp4_duration(tmp.path()).has_value());
}

TEST_CASE("the frame rate of a recording shorter than a second is counted over a second", "[Mcp][Recording]")
{
    CHECK_THAT(frames_per_second(1, 0.03), WithinAbs(1., 1e-9));
    CHECK_THAT(frames_per_second(30, 3.), WithinAbs(10., 1e-9));
}

TEST_CASE("ffmpeg's version and encoder listings are read", "[Mcp][Recording]")
{
    FfmpegVersion v = parse_ffmpeg_version("ffmpeg version 7.1-full_build-www.gyan.dev Copyright (c) 2000-2024");
    CHECK(v.known);
    CHECK(v.major == 7);
    CHECK(v.minor == 1);
    v = parse_ffmpeg_version("ffmpeg version n7.0.3 Copyright");
    CHECK(v.major == 7);
    CHECK(v.minor == 0);
    v = parse_ffmpeg_version("ffmpeg version 4.4.2-0ubuntu0.22.04.1 Copyright");
    CHECK(v.major == 4);
    CHECK(v.minor == 4);
    CHECK_FALSE(parse_ffmpeg_version("ffmpeg version N-112233-gabcdef Copyright").known);

    const std::set<std::string> encoders = parse_encoders("Encoders:\n"
                                                          " V..... = Video\n"
                                                          " ------\n"
                                                          " V....D libx264              libx264 H.264\n"
                                                          " V....D libx265              libx265 H.265 / HEVC (codec hevc)\n"
                                                          " A....D aac                  AAC\n");
    CHECK(encoders.count("libx265") == 1);
    CHECK(encoders.count("libx264") == 1);
    CHECK(encoders.count("=") == 0);
}

TEST_CASE("recordings and encodes never share a file they write", "[Mcp][Recording]")
{
    using Use = RecordingFiles::Use;
    RecordingFiles files;
    const fs::path rec("C:/out/rec.mkv"), other("C:/out/other.mkv"), mp4("C:/out/rec.mp4");
    const uint64_t recorder = files.new_owner();
    REQUIRE(files.reserve(recorder, {{rec, Use::Recording}}).empty());
    const uint64_t a = files.new_owner();
    CHECK_FALSE(files.reserve(a, {{rec, Use::Source}, {mp4, Use::Destination}}).empty()); // still being recorded
    files.release(recorder);
    REQUIRE(files.reserve(a, {{rec, Use::Source}, {mp4, Use::Destination}}).empty());
    const uint64_t b = files.new_owner();
    CHECK_FALSE(files.reserve(b, {{other, Use::Source}, {mp4, Use::Destination}}).empty()); // one destination
    CHECK(files.reserve(b, {{rec, Use::Source}, {fs::path("C:/out/b.mp4"), Use::Destination}}).empty()); // shared reading
    const uint64_t c = files.new_owner();
    CHECK_FALSE(files.reserve(c, {{rec, Use::ExclusiveSource}, {fs::path("C:/out/c.mp4"), Use::Destination}}).empty()); // read by others
    CHECK_FALSE(files.reserve(c, {{mp4, Use::Recording}}).empty()); // an encode writes it
    files.release(a);
    files.release(b);
    REQUIRE(files.reserve(c, {{rec, Use::ExclusiveSource}, {fs::path("C:/out/c.mp4"), Use::Destination}}).empty());
    const uint64_t d = files.new_owner();
    CHECK_FALSE(files.reserve(d, {{rec, Use::Source}, {fs::path("C:/out/d.mp4"), Use::Destination}}).empty()); // until it ends
    files.release(c);
    CHECK(files.reserve(d, {{rec, Use::Source}, {fs::path("C:/out/d.mp4"), Use::Destination}}).empty());
#ifdef _WIN32
    CHECK(RecordingFiles::same_file(fs::path("C:/Out/REC.mkv"), fs::path("c:\\out\\rec.mkv")));
#endif
}

TEST_CASE("path keys ignore case beyond ASCII where the file system does", "[Mcp][Recording]")
{
    // "É.mp4" and "é.mp4", neither of which exists, so only the key can tell they are one file.
    const fs::path upper = path_from_utf8("C:/out/\xC3\x89.mp4");
    const fs::path lower = path_from_utf8("C:/out/\xC3\xA9.mp4");
#if defined(_WIN32) || defined(__APPLE__)
    CHECK(path_compare_key(upper) == path_compare_key(lower));
#ifdef _WIN32
    // NTFS cases one character to one: "ß" and "SS" are two names there, so two keys.
    CHECK(path_compare_key(path_from_utf8("C:/out/\xC3\x9F.mp4")) != path_compare_key(path_from_utf8("C:/out/SS.mp4")));
#endif
    RecordingFiles files;
    REQUIRE(files.reserve(files.new_owner(), {{upper, RecordingFiles::Use::Destination}}).empty());
    CHECK_FALSE(files.reserve(files.new_owner(), {{lower, RecordingFiles::Use::Destination}}).empty());
#else
    CHECK(path_compare_key(upper) != path_compare_key(lower)); // Linux file systems keep case
#endif
    CHECK(path_compare_key(path_from_utf8("/a/b/../c.mp4")) == path_compare_key(path_from_utf8("/a/c.mp4")));
}

TEST_CASE("a recording an encode deleted is no longer the last one", "[Mcp][Recording]")
{
    RecordingFiles files;
    files.add_finished(fs::path("C:/out/one.mkv"));
    files.add_finished(fs::path("C:/out/two.mkv"));
    files.forget(fs::path("C:/out/two.mkv"));
    REQUIRE(files.last_finished().has_value());
    CHECK(*files.last_finished() == fs::path("C:/out/one.mkv"));
    CHECK_FALSE(files.is_finished(fs::path("C:/out/two.mkv")));
}

TEST_CASE("an exported script encodes the recording it makes and leaves other sources alone", "[Mcp][Recording]")
{
    Journal j;
    auto    ok = [&](const std::string& tool, const json& args) { j.end(j.begin(tool, args), ToolResult::ok()); };
    ok("start_recording", {{"save_path", "C:/Users/me/out/rec.mkv"}});
    ok("stop_recording", json::object());
    ok("encode_recording", {{"path", "C:/Users/me/out/rec.mkv"}, {"save_path", "C:/Users/me/out/rec.mp4"}});
    ok("encode_recording", {{"path", "D:/videos/old.mkv"}});
    const json steps = j.to_script("s")["steps"];
    REQUIRE(steps.size() == 4);
    CHECK(steps[0]["args"]["save_path"] == "${out_dir}/rec.mkv");
    CHECK(steps[2]["args"]["path"] == "${out_dir}/rec.mkv");
    CHECK(steps[2]["args"]["save_path"] == "${out_dir}/rec.mp4");
    CHECK(steps[3]["args"]["path"] == "D:/videos/old.mkv");
}
