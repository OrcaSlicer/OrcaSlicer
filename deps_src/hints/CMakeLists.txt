cmake_minimum_required(VERSION 3.13)
project(hints)

# Create interface library for hints headers, always available
add_library(hints INTERFACE)

# Set include directories for the interface library
target_include_directories(hints SYSTEM
    INTERFACE 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

# Add any compile features or definitions if needed
target_compile_features(hints INTERFACE cxx_std_17)

# The HintsToPot executable is a developer-only tool for i18n.
# Build it only when ORCA_TOOLS is enabled, to avoid linking
# against mismatched Boost runtimes in normal builds.
if(ORCA_TOOLS)
    add_executable(hintsToPot
        HintsToPot.cpp)

    target_include_directories(hintsToPot SYSTEM
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # On MSVC, force hintsToPot to use the Release
    # CRT (/MD) even in Debug builds so that it
    # matches the prebuilt Boost libraries and
    # avoids LNK2038 RuntimeLibrary/_ITERATOR_DEBUG_LEVEL
    # mismatches when linking.
    if(MSVC)
        target_compile_options(hintsToPot PRIVATE /MD)
    endif()

    if(TARGET boost_libs)
        target_link_libraries(hintsToPot PRIVATE boost_libs)
    endif()

    if(COMMAND encoding_check)
        encoding_check(hintsToPot)
    endif()
endif()



