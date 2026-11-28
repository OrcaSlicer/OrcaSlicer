#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Workload.hpp"
#include "orcabench_test_utils.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace Slic3r::Bench;
using namespace Slic3r::Bench::Test;

namespace {

CatalogEntry entry_of(std::string name, std::string kind)
{
    CatalogEntry entry;
    entry.name = std::move(name);
    entry.kind = std::move(kind);
    return entry;
}

std::unique_ptr<Workload> fake(const CatalogEntry& entry) { return std::make_unique<FakeWorkload>(entry); }

// A second kind, so a registry is seen holding more than one.
class OtherWorkload : public FakeWorkload
{
public:
    using FakeWorkload::FakeWorkload;
};

} // namespace

TEST_CASE("a registered kind builds workloads from their entries", "[OrcaBench][Workload]")
{
    WorkloadKinds kinds;
    kinds.add("fake", fake);
    const std::unique_ptr<Workload> workload = kinds.create(entry_of("fake/cube", "fake"));
    const auto* built = dynamic_cast<const FakeWorkload*>(workload.get());
    REQUIRE(built != nullptr);
    CHECK(built->entry.name == "fake/cube");
}

TEST_CASE("each kind builds its own workloads", "[OrcaBench][Workload]")
{
    WorkloadKinds kinds;
    kinds.add("fake", fake);
    kinds.add("other", [](const CatalogEntry& entry) { return std::make_unique<OtherWorkload>(entry); });
    const std::unique_ptr<Workload> other = kinds.create(entry_of("other/cube", "other"));
    const std::unique_ptr<Workload> plain = kinds.create(entry_of("fake/cube", "fake"));
    CHECK(dynamic_cast<const OtherWorkload*>(other.get()) != nullptr);
    CHECK(dynamic_cast<const OtherWorkload*>(plain.get()) == nullptr);
}

TEST_CASE("an entry of an unknown kind is refused", "[OrcaBench][Workload]")
{
    WorkloadKinds kinds;
    kinds.add("fake", fake);
    CHECK_THROWS_AS(kinds.create(entry_of("slice/cube", "slice")), WorkloadError);
}

TEST_CASE("an entry whose name is empty or unprintable is refused", "[OrcaBench][Workload]")
{
    const std::string name = GENERATE(as<std::string> {}, "", "two words", "line\nbreak", "tab\tname", "del\x7fname", "caf\xc3\xa9");
    CAPTURE(name);
    WorkloadKinds kinds;
    kinds.add("fake", fake);
    CHECK_THROWS_AS(kinds.create(entry_of(name, "fake")), WorkloadError);
}

TEST_CASE("a kind cannot be added twice", "[OrcaBench][Workload]")
{
    WorkloadKinds kinds;
    kinds.add("fake", fake);
    CHECK_THROWS_AS(kinds.add("fake", fake), WorkloadError);
}

TEST_CASE("a kind without a factory is refused", "[OrcaBench][Workload]")
{
    WorkloadKinds kinds;
    CHECK_THROWS_AS(kinds.add("fake", WorkloadFactory {}), WorkloadError);
}

TEST_CASE("a kind that builds nothing is refused", "[OrcaBench][Workload]")
{
    WorkloadKinds kinds;
    kinds.add("empty", [](const CatalogEntry&) { return std::unique_ptr<Workload> {}; });
    CHECK_THROWS_AS(kinds.create(entry_of("empty/cube", "empty")), WorkloadError);
}

TEST_CASE("a registrar adds its kind to the registry it is given", "[OrcaBench][Workload]")
{
    WorkloadKinds               kinds;
    const WorkloadKindRegistrar registrar("fake", fake, kinds);
    CHECK_NOTHROW(kinds.require_registered());
    const std::unique_ptr<Workload> workload = kinds.create(entry_of("fake/cube", "fake"));
    CHECK(dynamic_cast<const FakeWorkload*>(workload.get()) != nullptr);
}

TEST_CASE("a registrar keeps a failed registration for require_registered()", "[OrcaBench][Workload]")
{
    WorkloadKinds               kinds;
    const WorkloadKindRegistrar first("fake", fake, kinds);
    CHECK_NOTHROW(WorkloadKindRegistrar("fake", fake, kinds));
    CHECK_THROWS_WITH(kinds.require_registered(), Catch::Matchers::ContainsSubstring("'fake' is registered twice"));
}

TEST_CASE("PGO eligibility follows the tier unless the entry sets it", "[OrcaBench][Workload]")
{
    const auto [tier, flag, eligible] = GENERATE(table<Tier, std::optional<bool>, bool>({
        {Tier::Macro, std::nullopt, true},
        {Tier::Micro, std::nullopt, false},
        {Tier::Macro, false, false},
        {Tier::Micro, true, true},
    }));
    CatalogEntry entry = entry_of("fake/cube", "fake");
    entry.tier         = tier;
    entry.pgo_eligible = flag;
    CHECK(is_pgo_eligible(entry) == eligible);
}

TEST_CASE("the listing names each entry on its own line", "[OrcaBench][Workload]")
{
    CHECK(listing({entry_of("slice/cube", "slice"), entry_of("load/cube", "load")}) == "slice/cube\nload/cube\n");
    CHECK(listing({}).empty());
}

TEST_CASE("a pattern matches a whole workload name, where * stands for any text", "[OrcaBench][Workload]")
{
    const auto [pattern, name, matched] = GENERATE(table<std::string, std::string, bool>({
        {"slice/3dbenchy/classic", "slice/3dbenchy/classic", true},
        {"slice/3dbenchy", "slice/3dbenchy/classic", false},
        {"slice/3dbenchy/*", "slice/3dbenchy/classic", true},
        {"slice/*/classic", "slice/3dbenchy/classic", true},
        {"slice/*/classic", "slice/3dbenchy/arachne", false},
        {"*benchy*", "slice/3dbenchy/classic", true},
        {"*a*a*", "slice/3dbenchy/arachne", true},
        {"*a*a*a*", "slice/3dbenchy/arachne", false},
        {"*", "slice/3dbenchy/classic", true},
        {"slice/3dbenchy/classic*", "slice/3dbenchy/classic", true},
        {"", "slice/3dbenchy/classic", false},
    }));
    CAPTURE(pattern, name);
    CHECK(matches(pattern, name) == matched);
}
