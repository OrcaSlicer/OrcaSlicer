// Catch2 tests for the engine Session surface and its child services.
//
// Covers the "no attached state" path: a freshly-created Session has no
// PresetBundle and no Model bound, so the read-only accessors return safe
// defaults and the write paths fail with InvalidState / NotImplemented. The
// "with attached state" path is exercised by the GUI/CLI integration smoke
// tests — here we just lock in the boundary contract.

#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"
#include "orca/Presets.hpp"
#include "orca/Project.hpp"
#include "orca/Slicer.hpp"
#include "orca/Export.hpp"
#include "orca/Events.hpp"
#include "orca/Result.hpp"

#include <string>

TEST_CASE("Session::create returns a valid session", "[engine][session]") {
    auto s = orca::Session::create();
    REQUIRE(s != nullptr);

    REQUIRE_NOTHROW(s->presets());
    REQUIRE_NOTHROW(s->project());
    REQUIRE_NOTHROW(s->slicer());
    REQUIRE_NOTHROW(s->exporter());
    REQUIRE_NOTHROW(s->events());
    // Destructor runs at scope exit — must not throw.
}

TEST_CASE("Session lifecycle: create+destroy in a loop", "[engine][session]") {
    for (int i = 0; i < 5; ++i) {
        auto s = orca::Session::create();
        REQUIRE(s);
        s.reset();
    }
}

TEST_CASE("Presets accessors without an attached bundle", "[engine][presets]") {
    auto s = orca::Session::create();
    REQUIRE(s);
    const auto& p = s->presets();

    REQUIRE_FALSE(p.has_bundle());
    REQUIRE(p.extruder_count() == 0);
    REQUIRE_FALSE(p.is_bbl_vendor());
}

TEST_CASE("Project methods return NotImplemented stubs", "[engine][project]") {
    auto s = orca::Session::create();
    REQUIRE(s);
    auto& pr = s->project();

    SECTION("clear") {
        auto r = pr.clear();
        REQUIRE_FALSE(r.ok());
        REQUIRE(r.error().code == orca::ErrorCode::NotImplemented);
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("Project::clear"));
    }

    SECTION("save") {
        auto r = pr.save(std::filesystem::path{"/tmp/orca_unused.3mf"});
        REQUIRE_FALSE(r.ok());
        REQUIRE(r.error().code == orca::ErrorCode::NotImplemented);
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("Project::save"));
    }

    SECTION("remove_object") {
        auto r = pr.remove_object(0);
        REQUIRE_FALSE(r.ok());
        REQUIRE(r.error().code == orca::ErrorCode::NotImplemented);
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("Project::remove_object"));
    }
}

TEST_CASE("Slicer is initially not busy", "[engine][slicer]") {
    auto s = orca::Session::create();
    REQUIRE(s);
    REQUIRE_FALSE(s->slicer().is_busy());
}

TEST_CASE("Slicer::request_slice without attached state fails InvalidState", "[engine][slicer]") {
    auto s = orca::Session::create();
    REQUIRE(s);

    orca::SliceParams p;
    auto r = s->slicer().request_slice(p);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().code == orca::ErrorCode::InvalidState);
}

TEST_CASE("Exporter::export_gcode without completed slice fails", "[engine][exporter]") {
    auto s = orca::Session::create();
    REQUIRE(s);

    orca::ExportParams ep;
    ep.output_path = "/tmp/orca_test.gcode";
    auto r = s->exporter().export_gcode(ep);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().code == orca::ErrorCode::InvalidState);
}
