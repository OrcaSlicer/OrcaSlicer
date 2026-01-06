cmake_minimum_required(VERSION 3.13)
project(spline)

# Create interface library for spline (header-only library)
add_library(spline INTERFACE)

# Set include directories for the interface library
target_include_directories(spline SYSTEM
    INTERFACE 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

# Add compile features
target_compile_features(spline INTERFACE cxx_std_11)

# Create an alias for consistent naming
add_library(spline::spline ALIAS spline)

# Install headers if needed
install(FILES 
    spline.h
    DESTINATION include/spline
)

# Install the interface library
install(TARGETS spline
    EXPORT splineTargets
    INCLUDES DESTINATION include
)

# Export the targets
install(EXPORT splineTargets
    FILE splineTargets.cmake
    NAMESPACE spline::
    DESTINATION lib/cmake/spline
)
