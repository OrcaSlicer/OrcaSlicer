#pragma once

#include "core/Measurement.hpp"
#include "core/Policy.hpp"
#include "core/Result.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r { namespace Bench {

// Thrown for a kind registered twice or without a factory, or an entry no kind can build.
class WorkloadError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Whether a workload times one operation or a whole slice.
enum class Tier { Micro, Macro };

// One benchmark as the catalog describes it.
struct CatalogEntry
{
    // Results are keyed by it, so renaming an entry orphans its history.
    std::string           name;
    // The registered kind that builds it.
    std::string           kind;
    // The fixture it runs on, by id.
    std::string           fixture;
    Tier                  tier = Tier::Macro;
    // Whether PGO training may run it, where unset leaves it to is_pgo_eligible().
    std::optional<bool>   pgo_eligible;
    std::set<std::string> tags;
    // Settings the kind applies over its defaults, such as print config keys.
    Properties            config;
};

// Whether PGO training may run the entry, which by default a macro entry may and a micro one may not.
bool is_pgo_eligible(const CatalogEntry& entry);

// Throws WorkloadError for a name that is empty or has a character outside printable ASCII, which
// would break a listing's one name per line.
void validate(const CatalogEntry& entry);

// The entries' names, one per line in the catalog's order.
std::string listing(const std::vector<CatalogEntry>& catalog);

// Whether the whole name matches the pattern, where * stands for any run of characters, / included,
// and every other character for itself.
bool matches(std::string_view pattern, std::string_view name);

// What a workload needs to know about the run it is part of.
struct RunContext
{
    // The stages to time, where whatever a timed stage needs runs untimed in prepare().
    StageSet    timed;
    // Where a kind keeps each pass's output for a person to read, or empty to keep none.
    std::string dump_dir;
};

// One benchmark's code, driven through setup() once, then prepare() and execute() for every
// iteration, where an exception from any of them fails the workload.
class Workload
{
public:
    virtual ~Workload() = default;

    // Untimed and once, returning the reason to skip when the fixture is unavailable.
    virtual std::optional<std::string> setup(const RunContext& context) = 0;

    // Untimed before each iteration, building fresh state so no iteration inherits another's.
    virtual void prepare(const RunContext& context) = 0;

    // Timed, one iteration reported through measurement.
    virtual void execute(const RunContext& context, Measurement& measurement) = 0;
};

using WorkloadFactory = std::function<std::unique_ptr<Workload>(const CatalogEntry&)>;

// Builds workloads by kind, where a kind is code and the entries it builds are data.
class WorkloadKinds
{
public:
    // The registry the kinds compiled into orca_bench add themselves to.
    static WorkloadKinds& instance();

    // Throws WorkloadError for a kind already added or a factory that is empty.
    void add(std::string kind, WorkloadFactory factory);

    // Throws WorkloadError for an invalid entry, one whose kind nobody added, or one whose factory
    // builds nothing.
    std::unique_ptr<Workload> create(const CatalogEntry& entry) const;

    // Throws WorkloadError for the registrations a WorkloadKindRegistrar could not make.
    void require_registered() const;

private:
    friend class WorkloadKindRegistrar;

    std::map<std::string, WorkloadFactory> m_factories;
    std::vector<std::string>               m_failed_registrations;
};

// Adds one kind from the kind's own file during static initialization, keeping any error for
// require_registered(), since an exception cannot leave a static initializer without ending the
// process.
class WorkloadKindRegistrar
{
public:
    WorkloadKindRegistrar(std::string kind, WorkloadFactory factory, WorkloadKinds& kinds = WorkloadKinds::instance());
};

}} // namespace Slic3r::Bench
