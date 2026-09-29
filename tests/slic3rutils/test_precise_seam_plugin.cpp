#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "python_test_support.hpp"

#include <pybind11/embed.h>
#include <array>

namespace py = pybind11;
using namespace Slic3r;

TEST_CASE("Python exposes every seam mode and distinguishes strong weak and ordinary volumes", "[PreciseSeamPlugin][Python]")
{
    struct Mode {
        const char *name;
        ModelVolumeType type;
        bool strong;
        bool weak;
    };
    // Explicit expectations protect enum registration and both predicate groups independently.
    const std::array<Mode, 11> modes = {{
        {"PreciseSeamCenter", ModelVolumeType::PRECISE_SEAM_CENTER, true, false},
        {"PreciseSeamLeft", ModelVolumeType::PRECISE_SEAM_LEFT, true, false},
        {"PreciseSeamRight", ModelVolumeType::PRECISE_SEAM_RIGHT, true, false},
        {"PreciseSeamEnforced", ModelVolumeType::PRECISE_SEAM_ENFORCED, false, true},
        {"PreciseSeamBlocked", ModelVolumeType::PRECISE_SEAM_BLOCKED, false, true},
        {"PreciseSeamNeutral", ModelVolumeType::PRECISE_SEAM_NEUTRAL, false, true},
        {"ModelPart", ModelVolumeType::MODEL_PART, false, false},
        {"NegativeVolume", ModelVolumeType::NEGATIVE_VOLUME, false, false},
        {"ParameterModifier", ModelVolumeType::PARAMETER_MODIFIER, false, false},
        {"SupportEnforcer", ModelVolumeType::SUPPORT_ENFORCER, false, false},
        {"SupportBlocker", ModelVolumeType::SUPPORT_BLOCKER, false, false}
    }};

    // Bootstrap holds the GIL on first initialization; acquire explicitly for an already-running interpreter too.
    ensure_python_initialized();
    py::gil_scoped_acquire gil;
    py::object host = import_orca_module().attr("host");
    REQUIRE(py::hasattr(host, "ModelVolumeType"));
    py::object enumeration = host.attr("ModelVolumeType");
    for (const Mode &mode : modes) {
        DYNAMIC_SECTION(mode.name) {
            REQUIRE(py::hasattr(enumeration, mode.name));
            CHECK(enumeration.attr(mode.name).cast<ModelVolumeType>() == mode.type);
            Model model;
            auto *volume = model.add_object()->add_volume(make_cube(1, 1, 1));
            volume->set_type(mode.type);
            // Borrow the volume: the Python reference is destroyed before its owning Model.
            py::object py_volume = py::cast(volume, py::return_value_policy::reference);
            REQUIRE(py::hasattr(py_volume, "is_precise_seam"));
            REQUIRE(py::hasattr(py_volume, "is_precise_seam_strong"));
            REQUIRE(py::hasattr(py_volume, "is_precise_seam_weak"));
            CHECK(py_volume.attr("type")().cast<ModelVolumeType>() == mode.type);
            const bool precise = mode.strong || mode.weak;
            CHECK(py_volume.attr("is_precise_seam")().cast<bool>() == precise);
            CHECK(py_volume.attr("is_precise_seam_strong")().cast<bool>() == mode.strong);
            CHECK(py_volume.attr("is_precise_seam_weak")().cast<bool>() == mode.weak);
        }
    }
}
