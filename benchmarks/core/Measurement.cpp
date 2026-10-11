#include "core/Measurement.hpp"

#include "core/Result.hpp"
#include "core/Sampler.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace Slic3r { namespace Bench {

namespace {

// Refused where it is reported, since the document writer would otherwise refuse the whole result.
void require_finite(const std::string& what, double value)
{
    if (!std::isfinite(value))
        throw std::invalid_argument(what + " is not a finite number");
}

void require_finite(const Metrics& metrics)
{
    for (const auto& [key, value] : metrics)
        require_finite("the metric '" + key + "'", value);
}

} // namespace

Scope Scope::print() { return Scope("print"); }

Scope Scope::object(std::size_t index) { return Scope("object:" + std::to_string(index)); }

void Measurement::span(std::string stage, const Scope& scope, Clock::time_point started_at, Clock::time_point done_at, Metrics metrics)
{
    if (stage.empty())
        throw std::invalid_argument("a span needs a stage");
    if (done_at < started_at)
        throw std::invalid_argument("the " + stage + " span ends before it starts");
    if (started_at < m_iteration_start)
        throw std::invalid_argument("the " + stage + " span starts before its iteration");
    if (done_at > Clock::now())
        throw std::invalid_argument("the " + stage + " span ends after it is reported");
    for (const char* key : SampledMetric::all)
        if (metrics.count(key) != 0)
            throw std::invalid_argument("the " + stage + " span reports " + key + ", which the sampler writes");
    require_finite(metrics);
    claim(stage, scope);
    m_timeline.push_back({std::move(stage), scope.text(), started_at, done_at, std::move(metrics)});
}

void Measurement::unfinished(std::string stage, const Scope& scope, Clock::time_point started_at)
{
    if (stage.empty())
        throw std::invalid_argument("an unfinished step needs a stage");
    if (started_at < m_iteration_start)
        throw std::invalid_argument("the " + stage + " span starts before its iteration");
    if (started_at > Clock::now())
        throw std::invalid_argument("the " + stage + " span starts after it is reported");
    claim(stage, scope);
    m_unfinished.push_back({std::move(stage), scope.text(), started_at});
}

void Measurement::not_run(std::string stage, const Scope& scope)
{
    if (stage.empty())
        throw std::invalid_argument("a step that did not run needs a stage");
    claim(stage, scope);
    m_not_run.push_back({std::move(stage), scope.text()});
}

// Refuses a stage already reported for the scope, and is called after every other check so a refused
// report can be made again.
void Measurement::claim(const std::string& stage, const Scope& scope)
{
    if (!m_reported.emplace(stage, scope.text()).second)
        throw std::invalid_argument("the " + stage + " stage of " + scope.text() + " was reported twice");
}

void Measurement::metric(std::string key, double value)
{
    require_finite("the metric '" + key + "'", value);
    if (!m_metrics.emplace(key, value).second)
        throw std::invalid_argument("the metric '" + key + "' was reported twice in one iteration");
}

void Measurement::work(WorkStats work)
{
    if (m_work)
        throw std::invalid_argument("work stats reported twice in one iteration");
    require_finite("the extrusion volume", work.extrusion_mm3);
    require_finite(work.metrics);
    m_work = std::move(work);
}

void Measurement::output_hash(std::uint64_t hash)
{
    if (m_hash)
        throw std::invalid_argument("an output hash reported twice in one iteration");
    m_hash = hash;
}

}} // namespace Slic3r::Bench
