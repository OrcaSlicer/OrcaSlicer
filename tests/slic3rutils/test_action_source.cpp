#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/ActionRegistry.hpp"

#include <memory>
#include <string>
#include <type_traits>

using Slic3r::GUI::AppAction;
using Slic3r::GUI::AppActionRunResult;
using Slic3r::GUI::ActionRegistry;

namespace {

// AppAction is abstract; this minimal concrete action lets the tests exercise its
// constructor-composed identity without involving a plugin runner.
class TestAppAction final : public AppAction
{
public:
    TestAppAction() : AppAction("test", "Action title", "src-key", "Action source") {}

    AppActionRunResult run() const override { return {}; }
};

} // namespace

TEST_CASE("AppAction composes a stable id from prefix:title:source_key", "[speeddial][actions]")
{
    CHECK(AppAction::compose_id("test", "Action title", "src-key") == "test:Action title:src-key");
    // source_key (not the display name) carries identity, so it is the third field.
    CHECK(AppAction::compose_id("script", "Do Thing", "pack.py") == "script:Do Thing:pack.py");
}

TEST_CASE("AppAction definitions are immutable after construction", "[speeddial][actions]")
{
    using StringAccessor = const std::string& (AppAction::*)() const;

    STATIC_CHECK(std::is_same_v<decltype(&AppAction::id), StringAccessor>);
    STATIC_CHECK(std::is_same_v<decltype(&AppAction::title), StringAccessor>);
    STATIC_CHECK(std::is_same_v<decltype(&AppAction::source_key), StringAccessor>);
    STATIC_CHECK(std::is_same_v<decltype(&AppAction::source_name), StringAccessor>);

    const TestAppAction action;
    CHECK(action.id() == "test:Action title:src-key");
    CHECK(action.title() == "Action title");
    CHECK(action.source_key() == "src-key");
    CHECK(action.source_name() == "Action source");
}

TEST_CASE("ActionRegistry takes exclusive ownership of published actions", "[speeddial][actions]")
{
    using ExpectedUpsert = void (ActionRegistry::*)(std::unique_ptr<AppAction>);

    STATIC_CHECK(std::is_same_v<decltype(&ActionRegistry::upsert), ExpectedUpsert>);
}
