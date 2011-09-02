
set(DEP_CMAKE_OPTS "-DCMAKE_POSITION_INDEPENDENT_CODE=ON")

include("deps-unix-common.cmake")

# Some Linuxes may have very old libpng, so it's best to bundle it instead of relying on the system version.
# find_package(PNG QUIET)
# if (NOT PNG_FOUND)
#     message(WARNING "No PNG dev package found in system, building static library. You should install the system package.")
# endif ()