
# The unix common part expects DEP_CMAKE_OPTS to be set

if (MINGW)
    set(TBB_MINGW_WORKAROUND "-flifetime-dse=1")
else ()
    set(TBB_MINGW_WORKAROUND "")
endif ()

find_package(ZLIB QUIET)
if (NOT ZLIB_FOUND)
    message(WARNING "No ZLIB dev package found in system, building static library. You should install the system package.")
endif ()

# TODO Evaluate expat modifications in the bundled version and test with system versions in various distros and OSX SDKs
# find_package(EXPAT QUIET)
# if (NOT EXPAT_FOUND)
#     message(WARNING "No EXPAT dev package found in system, building static library. Consider installing the system package.")
# endif ()
