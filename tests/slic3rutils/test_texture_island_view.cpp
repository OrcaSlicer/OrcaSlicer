// How the texture displacement gizmo's view follows the UV editor's island highlight. The rules live in
// libslic3r_gui; this is the suite that links it. Same Windows include prologue as
// test_texture_color_palette.cpp (wx pulls in <windows.h>; keep WIN32_LEAN_AND_MEAN / NOMINMAX ahead of it).
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"

using Follow = Slic3r::GUI::GLGizmoTextureDisplacement::IslandViewFollow;

namespace {

// The View row's indices, and step()'s "apply nothing".
constexpr int NORMAL = 0, FAST = 1, CHECKER = 2, DISTORTION = 3, NONE = -1;

} // namespace

TEST_CASE("a Normal view gives way to Fast while islands are selected and comes back once none are", "[TextureIslandView][TextureDisplacement]")
{
    Follow f;
    CHECK(f.step(false, NORMAL, false) == NONE);
    CHECK(f.step(true, NORMAL, false) == FAST);
    CHECK(f.step(true, FAST, false) == NONE);
    CHECK(f.step(false, FAST, false) == NORMAL);
    CHECK(f.step(false, NORMAL, false) == NONE);
}

TEST_CASE("a Fast, Checker or Distortion view is left alone by the island highlight", "[TextureIslandView][TextureDisplacement]")
{
    const int view = GENERATE(FAST, CHECKER, DISTORTION);
    Follow    f;
    CHECK(f.step(true, view, false) == NONE);
    CHECK(f.step(false, view, false) == NONE);
}

TEST_CASE("arriving in Normal with islands already selected switches to Fast", "[TextureIslandView][TextureDisplacement]")
{
    // The pane's Height button over Checker lands in Normal with the islands still selected.
    Follow f;
    CHECK(f.step(true, CHECKER, false) == NONE);
    CHECK(f.step(true, NORMAL, false) == FAST);
    CHECK(f.step(false, FAST, false) == NORMAL);
}

TEST_CASE("Normal picked while islands are selected stays until they are deselected", "[TextureIslandView][TextureDisplacement]")
{
    Follow f;
    REQUIRE(f.step(true, NORMAL, false) == FAST);
    f.picked(NORMAL);
    CHECK(f.step(true, NORMAL, false) == NONE);
    // A debug stage opening and closing over it changes nothing.
    CHECK(f.step(true, NORMAL, true) == NONE);
    CHECK(f.step(true, NORMAL, false) == NONE);
    CHECK(f.step(false, NORMAL, false) == NONE);
    CHECK(f.step(true, NORMAL, false) == FAST);
}

TEST_CASE("Fast picked while it was switched on stays after deselecting", "[TextureIslandView][TextureDisplacement]")
{
    Follow f;
    REQUIRE(f.step(true, NORMAL, false) == FAST);
    f.picked(FAST);
    CHECK(f.step(false, FAST, false) == NONE);
}

TEST_CASE("Normal picked with nothing selected does not hold off the next switch", "[TextureIslandView][TextureDisplacement]")
{
    Follow f;
    CHECK(f.step(false, NORMAL, false) == NONE);
    f.picked(NORMAL);
    CHECK(f.step(true, NORMAL, false) == FAST);
}

TEST_CASE("a debug stage holds Normal while it is open, and the switch follows once it closes", "[TextureIslandView][TextureDisplacement]")
{
    Follow f;
    REQUIRE(f.step(true, NORMAL, false) == FAST);
    // The stage takes Normal itself, with the islands still selected.
    CHECK(f.step(true, NORMAL, true) == NONE);
    CHECK(f.step(true, NORMAL, false) == FAST);
}

TEST_CASE("islands deselected while a debug stage is open apply no view over it", "[TextureIslandView][TextureDisplacement]")
{
    Follow f;
    REQUIRE(f.step(true, NORMAL, false) == FAST);
    CHECK(f.step(false, NORMAL, true) == NONE);
    CHECK(f.step(false, NORMAL, false) == NONE);
}
