#include "core/Workload.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace Slic3r { namespace Bench {

bool is_pgo_eligible(const CatalogEntry& entry) { return entry.pgo_eligible.value_or(entry.tier == Tier::Macro); }

void validate(const CatalogEntry& entry)
{
    const bool printable = std::all_of(entry.name.begin(), entry.name.end(), [](char c) { return c > ' ' && c <= '~'; });
    if (entry.name.empty() || !printable)
        throw WorkloadError("the catalog entry name '" + entry.name + "' is empty or has a character outside printable ASCII");
}

bool matches(std::string_view pattern, std::string_view name)
{
    // On a mismatch the last * takes one more character, so the scan never backtracks further.
    std::size_t p = 0, n = 0, star = std::string_view::npos, star_n = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star   = p++;
            star_n = n;
        } else if (p < pattern.size() && pattern[p] == name[n]) {
            ++p;
            ++n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++star_n;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

std::string listing(const std::vector<CatalogEntry>& catalog)
{
    std::string lines;
    for (const CatalogEntry& entry : catalog)
        lines += entry.name + "\n";
    return lines;
}

WorkloadKinds& WorkloadKinds::instance()
{
    static WorkloadKinds kinds;
    return kinds;
}

void WorkloadKinds::add(std::string kind, WorkloadFactory factory)
{
    if (!factory)
        throw WorkloadError("the workload kind '" + kind + "' has no factory");
    if (!m_factories.try_emplace(kind, std::move(factory)).second)
        throw WorkloadError("the workload kind '" + kind + "' is registered twice");
}

std::unique_ptr<Workload> WorkloadKinds::create(const CatalogEntry& entry) const
{
    validate(entry);
    const auto found = m_factories.find(entry.kind);
    if (found == m_factories.end())
        throw WorkloadError(entry.name + " is of the kind '" + entry.kind + "', which no workload registered");
    std::unique_ptr<Workload> workload = found->second(entry);
    if (!workload)
        throw WorkloadError("the " + entry.kind + " kind built nothing for " + entry.name);
    return workload;
}

void WorkloadKinds::require_registered() const
{
    std::string errors;
    for (const std::string& error : m_failed_registrations)
        errors += (errors.empty() ? "" : "; ") + error;
    if (!errors.empty())
        throw WorkloadError(errors);
}

WorkloadKindRegistrar::WorkloadKindRegistrar(std::string kind, WorkloadFactory factory, WorkloadKinds& kinds)
{
    try {
        kinds.add(std::move(kind), std::move(factory));
    } catch (const WorkloadError& error) {
        kinds.m_failed_registrations.emplace_back(error.what());
    }
}

}} // namespace Slic3r::Bench
