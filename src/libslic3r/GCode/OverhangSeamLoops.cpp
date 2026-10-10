#include "OverhangSeamLoops.hpp"

#include "libslic3r/GCodeWriter.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

namespace Slic3r {

// Problems logged one by one; the rest only counted in the summary.
static constexpr size_t MAX_LOGGED_PROBLEMS = 10;

OverhangSeamKey OverhangSeamKey::from_gcode(double x, double y, double z, int filament)
{
    // One grid step is the last digit the G-code writer emits for X, Y and Z.
    static const double scale = std::pow(10., GCodeFormatter::XYZF_EXPORT_DIGITS);
    return { std::llround(x * scale), std::llround(y * scale), std::llround(z * scale), filament };
}

void OverhangSeamChannel::publish(size_t layer_tag, std::vector<OverhangSeamLoop> loops)
{
    if (loops.empty())
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<OverhangSeamLoop> &packet = m_packets[layer_tag];
    packet.insert(packet.end(), std::make_move_iterator(loops.begin()), std::make_move_iterator(loops.end()));
}

std::vector<OverhangSeamLoop> OverhangSeamChannel::take(size_t layer_tag)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_packets.find(layer_tag);
    if (it == m_packets.end())
        return {};
    std::vector<OverhangSeamLoop> loops = std::move(it->second);
    m_packets.erase(it);
    return loops;
}

std::vector<OverhangSeamLoop> OverhangSeamChannel::take_all()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<OverhangSeamLoop> loops;
    for (auto &packet : m_packets)
        loops.insert(loops.end(), std::make_move_iterator(packet.second.begin()), std::make_move_iterator(packet.second.end()));
    m_packets.clear();
    return loops;
}

void OverhangSeamChannel::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_packets.clear();
}

void OverhangSeamMatcher::reset()
{
    m_packet.clear();
    m_index.clear();
    m_unmatched = 0;
    m_candidate.clear();
    m_candidate_end = {};
    m_stats = {};
}

void OverhangSeamMatcher::on_layer_change(OverhangSeamChannel &channel)
{
    close_packet();
    ++m_stats.layer_tags_seen;
    std::vector<OverhangSeamLoop> loops = channel.take(m_stats.layer_tags_seen);
    m_stats.registered += loops.size();
    m_unmatched = loops.size();
    m_packet.reserve(loops.size());
    m_index.reserve(loops.size());
    for (OverhangSeamLoop &loop : loops) {
        m_index.emplace_back(loop.start, m_packet.size());
        m_packet.push_back({ std::move(loop), false });
    }
    std::sort(m_index.begin(), m_index.end());
}

bool OverhangSeamMatcher::match_start(const OverhangSeamKey &start, bool joins_candidate)
{
    // Loops with the same start are taken in registration order.
    for (auto it = std::lower_bound(m_index.begin(), m_index.end(), std::make_pair(start, size_t(0)));
         it != m_index.end() && it->first == start; ++it) {
        Entry &entry = m_packet[it->second];
        if (entry.used)
            continue;
        entry.used = true;
        --m_unmatched;
        // The old detector merges loops printed without a move in between; the end check
        // cannot tell them apart, so all loops of such a candidate are suspect.
        if (joins_candidate)
            for (CandidateLoop &loop : m_candidate)
                loop.merged = true;
        m_candidate.push_back({ entry.loop, joins_candidate });
        return true;
    }
    return false;
}

void OverhangSeamMatcher::on_candidate_end()
{
    for (const CandidateLoop &loop : m_candidate) {
        if (loop.merged)
            report(loop.loop, false, "merged with another loop");
        else if (loop.loop.end != m_candidate_end)
            report(loop.loop, false, "ended elsewhere");
        else
            ++m_stats.end_consistent;
    }
    m_candidate.clear();
}

void OverhangSeamMatcher::finish(OverhangSeamChannel &channel, size_t layer_tags_expected)
{
    close_packet();
    // Packets of layers whose tag the processor never saw.
    for (const OverhangSeamLoop &loop : channel.take_all()) {
        ++m_stats.registered;
        report(loop, true, "start not found");
    }
    for (const CandidateLoop &loop : m_candidate)
        report(loop.loop, false, "never ended");
    m_candidate.clear();
    m_stats.layer_tags_expected = layer_tags_expected;

    if (m_stats.registered > 0)
        BOOST_LOG_TRIVIAL(info) << "[OverhangSeamPreview] loops " << m_stats.registered << ", end consistent " << m_stats.end_consistent
                                << ", suspect " << m_stats.suspect << ", missed " << m_stats.missed << ", layer tags written "
                                << m_stats.layer_tags_expected << ", read " << m_stats.layer_tags_seen;
}

void OverhangSeamMatcher::close_packet()
{
    for (const Entry &entry : m_packet)
        if (!entry.used)
            report(entry.loop, true, "start not found");
    m_packet.clear();
    m_index.clear();
    m_unmatched = 0;
}

void OverhangSeamMatcher::report(const OverhangSeamLoop &loop, bool missed, const char *problem)
{
    ++(missed ? m_stats.missed : m_stats.suspect);
    auto it = std::find_if(m_stats.problem_objects.begin(), m_stats.problem_objects.end(),
                           [&loop](const OverhangSeamStats::ProblemObject &problem) { return problem.object == loop.object; });
    if (it == m_stats.problem_objects.end())
        m_stats.problem_objects.push_back({ loop.object, loop.layer_num, 1 });
    else {
        ++it->count;
        // Problems are not reported in layer order: a candidate may close after the next layer.
        it->first_layer = std::min(it->first_layer, loop.layer_num);
    }
    if (m_stats.missed + m_stats.suspect <= MAX_LOGGED_PROBLEMS)
        BOOST_LOG_TRIVIAL(warning) << "[OverhangSeamPreview] " << problem << ": object \"" << loop.object << "\", layer " << loop.layer_num
                                   << ", start " << loop.start.x << ' ' << loop.start.y << ' ' << loop.start.z
                                   << ", filament " << loop.start.filament;
}

} // namespace Slic3r
