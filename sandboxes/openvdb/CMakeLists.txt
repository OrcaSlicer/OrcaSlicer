if(TARGET OpenVDB::openvdb)
    add_executable(openvdb_example openvdb_example.cpp)

    target_link_libraries(openvdb_example libslic3r)
    target_link_libraries(openvdb_example OpenVDB::openvdb)
endif()

