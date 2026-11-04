#pragma once

// A wall-clock trace of the ChromaStack preview path, off unless ORCA_CHROMA_PROFILE is set to
// something other than 0 in the environment. Every line goes to stderr as
//
//   chroma-prof <timestamp ms> <thread> <stage> <ms> <detail>
//
// The timestamp is what makes the chain readable: the gap between one stage's end and the next
// stage's start is time nobody is computing in - the settle delay, or a wait behind another job on
// the single UI worker thread. This is an investigation aid, not a feature: nothing in the program
// reads it and no UI exposes it.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>

namespace Slic3r {

inline bool chroma_profile_on()
{
    static const bool on = []() {
        const char *v = std::getenv("ORCA_CHROMA_PROFILE");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

inline double chroma_profile_now_ms()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void chroma_profile_line(const std::string &stage, double ms, const std::string &detail = {})
{
    if (!chroma_profile_on())
        return;
    std::ostringstream tid;
    tid << std::this_thread::get_id();
    std::fprintf(stderr, "chroma-prof %14.1f  t%-8s %-38s %9.2f ms  %s\n", chroma_profile_now_ms(),
                 tid.str().c_str(), stage.c_str(), ms, detail.c_str());
    std::fflush(stderr);
}

// Marks a moment rather than a duration: a queue, a settle, a frame the card asked for.
inline void chroma_profile_mark(const std::string &stage, const std::string &detail = {})
{
    chroma_profile_line(stage, 0.0, detail);
}

// Stage timing for one pass through a function: every step() reports the time since the previous
// step, and total() the time since construction.
class ChromaProfileSteps
{
public:
    explicit ChromaProfileSteps(std::string what)
        : m_what(std::move(what)), m_start(std::chrono::steady_clock::now()), m_last(m_start)
    {}

    void step(const char *name, const std::string &detail = {})
    {
        if (!chroma_profile_on())
            return;
        const auto now = std::chrono::steady_clock::now();
        chroma_profile_line(m_what + "/" + name,
                            std::chrono::duration<double, std::milli>(now - m_last).count(), detail);
        m_last = now;
    }

    void total(const std::string &detail = {})
    {
        if (!chroma_profile_on())
            return;
        chroma_profile_line(m_what + "/TOTAL",
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start)
                                .count(),
                            detail);
    }

private:
    std::string                           m_what;
    std::chrono::steady_clock::time_point m_start;
    std::chrono::steady_clock::time_point m_last;
};

// The same for a whole function, reported when it returns. `min_ms` is for something that runs every
// frame: at 0 every pass is reported, and above it only the passes that took that long, so a slow
// frame stands out without sixty lines a second of ordinary ones burying it.
class ChromaProfileScope
{
public:
    explicit ChromaProfileScope(std::string what, std::string detail = {}, double min_ms = 0.0)
        : m_what(std::move(what)), m_detail(std::move(detail)), m_min_ms(min_ms),
          m_start(std::chrono::steady_clock::now())
    {}
    ~ChromaProfileScope()
    {
        if (!chroma_profile_on())
            return;
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count();
        if (ms >= m_min_ms)
            chroma_profile_line(m_what, ms, m_detail);
    }

private:
    std::string                           m_what;
    std::string                           m_detail;
    double                                m_min_ms;
    std::chrono::steady_clock::time_point m_start;
};

} // namespace Slic3r
