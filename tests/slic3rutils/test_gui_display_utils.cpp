#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/GUI_Utils.hpp"

#include <wx/defs.h>

TEST_CASE("Display lookup rejects detached window display indexes", "[GUI][Display][Regression]")
{
    using Slic3r::GUI::detail::safe_display_index;

    CHECK(safe_display_index(wxNOT_FOUND, 2) == wxNOT_FOUND);
    CHECK(safe_display_index(-2, 2) == wxNOT_FOUND);
    CHECK(safe_display_index(2, 2) == wxNOT_FOUND);
    CHECK(safe_display_index(100, 2) == wxNOT_FOUND);
}

TEST_CASE("Display lookup preserves valid window display indexes", "[GUI][Display][Regression]")
{
    using Slic3r::GUI::detail::safe_display_index;

    CHECK(safe_display_index(0, 1) == 0);
    CHECK(safe_display_index(1, 2) == 1);
}

TEST_CASE("Display lookup reports no display when the toolkit has none", "[GUI][Display][Regression]")
{
    using Slic3r::GUI::detail::safe_display_index;

    CHECK(safe_display_index(0, 0) == wxNOT_FOUND);
    CHECK(safe_display_index(wxNOT_FOUND, 0) == wxNOT_FOUND);
}

TEST_CASE("GTK backend selection stays native Wayland by default", "[GUI][Display][Regression]")
{
    using Slic3r::GUI::detail::should_force_x11_backend_for_wayland_kvm;

    CHECK_FALSE(should_force_x11_backend_for_wayland_kvm(nullptr, ":0", "wayland-0", false));
    CHECK_FALSE(should_force_x11_backend_for_wayland_kvm("wayland", ":0", "wayland-0", false));
    CHECK_FALSE(should_force_x11_backend_for_wayland_kvm("wayland,x11", ":0", "wayland-0", false));
}

TEST_CASE("GTK backend selection forces X11 when Wayland KVM workaround preference is enabled", "[GUI][Display][Regression]")
{
    using Slic3r::GUI::detail::should_force_x11_backend_for_wayland_kvm;

    CHECK(should_force_x11_backend_for_wayland_kvm(nullptr, ":0", "wayland-0", true));
    CHECK(should_force_x11_backend_for_wayland_kvm("", ":0", "wayland-0", true));
    CHECK(should_force_x11_backend_for_wayland_kvm("wayland", ":0", "wayland-0", true));
    CHECK(should_force_x11_backend_for_wayland_kvm("wayland,x11", ":0", "wayland-0", true));
}

TEST_CASE("GTK backend selection keeps existing X11 backend", "[GUI][Display][Regression]")
{
    using Slic3r::GUI::detail::should_force_x11_backend_for_wayland_kvm;

    CHECK_FALSE(should_force_x11_backend_for_wayland_kvm("x11", ":0", "wayland-0", false));
    CHECK_FALSE(should_force_x11_backend_for_wayland_kvm("x11", ":0", "wayland-0", true));
    CHECK_FALSE(should_force_x11_backend_for_wayland_kvm("x11,wayland", ":0", "wayland-0", true));
}

TEST_CASE("GTK backend selection requires XWayland for the workaround", "[GUI][Display][Regression]")
{
    using Slic3r::GUI::detail::should_force_x11_backend_for_wayland_kvm;

    CHECK_FALSE(should_force_x11_backend_for_wayland_kvm(nullptr, nullptr, "wayland-0", true));
    CHECK_FALSE(should_force_x11_backend_for_wayland_kvm(nullptr, "", "wayland-0", true));
    CHECK_FALSE(should_force_x11_backend_for_wayland_kvm(nullptr, ":0", nullptr, true));
    CHECK_FALSE(should_force_x11_backend_for_wayland_kvm(nullptr, ":0", "", true));
}
