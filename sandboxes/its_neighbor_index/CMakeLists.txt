add_executable(its_neighbor_index main.cpp ItsNeighborIndex.cpp ItsNeighborIndex.hpp)

target_link_libraries(its_neighbor_index libslic3r admesh)

if (WIN32)
    prusaslicer_copy_dlls(its_neighbor_index)
endif()
