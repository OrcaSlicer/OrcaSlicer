# Google OR-Tools — CP-SAT constraint solver for Magma tube assignment.
# Built from source with all LP solvers disabled (only CP-SAT needed).
# BUILD_DEPS=ON lets OR-Tools self-bootstrap abseil, protobuf, re2, etc.

if(BUILD_SHARED_LIBS)
    set(_ortools_shared ON)
else()
    set(_ortools_shared OFF)
endif()

# Magma uses ONLY CP-SAT from OR-Tools — strip everything else.
# CP-SAT's irreducible deps are: abseil, protobuf, re2, eigen3, zlib, bzip2.
#
# Flatpak-friendly handling:
# OR-Tools' BUILD_DEPS=ON uses FetchContent to pull abseil/protobuf/re2 from
# GitHub at build time. Flatpak's sandbox blocks all network during build,
# so we point FetchContent at directories pre-populated by the Flatpak
# manifest's `type: git` sources. zlib/bzip2/eigen3 are provided by the
# GNOME SDK runtime, so we tell OR-Tools to use those via find_package
# instead of bundling — saves three more flatpak sources.
#
# When FLATPAK is OFF, these args are unset and OR-Tools' BUILD_DEPS=ON
# downloads everything normally — zero impact on Linux/Win/Mac builds.
set(_ortools_flatpak_args "")
if (FLATPAK)
    # OR-Tools' BUILD_DEPS=ON force-overrides BUILD_ZLIB/BZip2/Eigen3 back to ON
    # via CMAKE_DEPENDENT_OPTION, so we can't tell it "use system" — we have to
    # pre-populate its FetchContent sources. The Flatpak manifest stages all six
    # subdeps as git checkouts in external-packages/; the FETCHCONTENT_SOURCE_DIR
    # vars below point OR-Tools at those local copies so no network is needed
    # during the sandboxed build.
    list(APPEND _ortools_flatpak_args
        -DFETCHCONTENT_SOURCE_DIR_ABSL=${DEP_DOWNLOAD_DIR}/abseil-cpp
        -DFETCHCONTENT_SOURCE_DIR_PROTOBUF=${DEP_DOWNLOAD_DIR}/protobuf
        -DFETCHCONTENT_SOURCE_DIR_RE2=${DEP_DOWNLOAD_DIR}/re2
        -DFETCHCONTENT_SOURCE_DIR_ZLIB=${DEP_DOWNLOAD_DIR}/zlib
        -DFETCHCONTENT_SOURCE_DIR_BZIP2=${DEP_DOWNLOAD_DIR}/bzip2
        -DFETCHCONTENT_SOURCE_DIR_EIGEN3=${DEP_DOWNLOAD_DIR}/eigen
    )
endif ()

orcaslicer_add_cmake_project(ORTools
    URL "https://github.com/google/or-tools/archive/refs/tags/v9.15.tar.gz"
    URL_HASH SHA256=6395a00a97ff30af878ee8d7fd5ad0ab1c7844f7219182c6d71acbee1b5f3026
    CMAKE_ARGS
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DBUILD_SHARED_LIBS=${_ortools_shared}
        -DBUILD_DEPS=ON
        -DBUILD_CXX=ON
        -DBUILD_PYTHON=OFF
        -DBUILD_JAVA=OFF
        -DBUILD_DOTNET=OFF
        -DBUILD_SAMPLES=OFF
        -DBUILD_EXAMPLES=OFF
        -DBUILD_TESTING=OFF
        -DBUILD_DOC=OFF
        # Magma uses CP-SAT only — drop FlatZinc to shrink the build.
        # (We tried -DBUILD_MATH_OPT=OFF too, but OR-Tools' Gurobi target has
        # no clean OFF switch and still links math_opt_proto, so disabling
        # MathOpt breaks the configure step. Leaving it ON is harmless: we
        # just don't use the resulting code.)
        -DBUILD_FLATZINC=OFF
        -DUSE_SCIP=OFF
        -DUSE_COINOR=OFF
        -DUSE_GLPK=OFF
        -DUSE_HIGHS=OFF
        -DUSE_PDLP=OFF
        ${_ortools_flatpak_args}
)

if (MSVC)
    add_debug_dep(dep_ORTools)
endif ()
