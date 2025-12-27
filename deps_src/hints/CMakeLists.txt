cmake_minimum_required(VERSION 3.13)
project(hints)

# Create interface library for hints (header-only if no source files)
# Since HintsToPot.cpp is an executable, we create both:
# 1. An interface library for using hints in other projects
# 2. An executable for the HintsToPot utility

# Create the utility executable
add_executable(hintsToPot
    HintsToPot.cpp)

# Set include directories for the executable
target_include_directories(hintsToPot SYSTEM
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link required libraries
if(TARGET boost_libs)
    target_link_libraries(hintsToPot PRIVATE boost_libs)
endif()

# Create an interface library for other projects to use
add_library(hints INTERFACE)

# Set include directories for the interface library
target_include_directories(hints SYSTEM
    INTERFACE 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

# Add any compile features or definitions if needed
target_compile_features(hints INTERFACE cxx_std_17)

# Check if encoding_check function is available
if(COMMAND encoding_check)
    encoding_check(hintsToPot)
endif()



