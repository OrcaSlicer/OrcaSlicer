# orca-engine-cli — standalone canary that links the engine without wxWidgets.

add_executable(orca-engine-cli main.cpp)

# orca-engine (alias of libslic3r) provides the engine symbols + its PUBLIC
# include dirs (the generated libslic3r_version.h, boost, Eigen, ...). We add
# src/ so the canary can include headers by their "libslic3r/..." path, the same
# way GUI code does.
target_link_libraries(orca-engine-cli PRIVATE orca-engine)
target_include_directories(orca-engine-cli PRIVATE ${CMAKE_SOURCE_DIR}/src)

set_target_properties(orca-engine-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/engine")
