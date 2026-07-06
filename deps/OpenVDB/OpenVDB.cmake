if(BUILD_SHARED_LIBS)
    set(_build_shared ON)
    set(_build_static OFF)
else()
    set(_build_shared OFF)
    set(_build_static ON)
endif()

if (IN_GIT_REPO)
    set(OPENVDB_DIRECTORY_FLAG --directory ${BINARY_DIR_REL}/dep_OpenVDB-prefix/src/dep_OpenVDB)
endif ()

orcaslicer_add_cmake_project(OpenVDB
    #  support vs2022, update to 8.2
    URL https://github.com/AcademySoftwareFoundation/openvdb/archive/refs/tags/v13.0.0.zip
    URL_HASH SHA256=37b31e0e67cd071b4aafbf924b4ac7dce9b15c439c0e25b4b35aa47f3939a5fc 
    DEPENDS dep_TBB dep_Blosc dep_OpenEXR dep_Boost
    CMAKE_ARGS
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON 
        -DOPENVDB_BUILD_PYTHON_MODULE=OFF
        -DUSE_BLOSC=ON
        -DOPENVDB_CORE_SHARED=${_build_shared} 
        -DOPENVDB_CORE_STATIC=${_build_static}
        -DOPENVDB_ENABLE_RPATH:BOOL=OFF
        -DTBB_STATIC=${_build_static}
        -DOPENVDB_BUILD_VDB_PRINT=ON
        -DDISABLE_DEPENDENCY_VERSION_CHECKS=ON # Centos6 has old zlib
)

if (MSVC)
    if (${DEP_DEBUG})
        ExternalProject_Get_Property(dep_OpenVDB BINARY_DIR)
        ExternalProject_Add_Step(dep_OpenVDB build_debug
            DEPENDEES build
            DEPENDERS install
            COMMAND ${CMAKE_COMMAND} ../dep_OpenVDB -DOPENVDB_BUILD_VDB_PRINT=OFF
            COMMAND msbuild /m /P:Configuration=Debug INSTALL.vcxproj
            WORKING_DIRECTORY "${BINARY_DIR}"
        )
    endif ()
endif ()
