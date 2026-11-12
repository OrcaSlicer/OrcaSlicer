cmake_minimum_required(VERSION 3.13)
project(stb_dxt)

# Create interface library for stb_dxt (header-only library)
add_library(stb_dxt INTERFACE)

# Set include directories for the interface library
target_include_directories(stb_dxt SYSTEM
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Add compile features
target_compile_features(stb_dxt INTERFACE cxx_std_11)

# Create an alias for consistent naming
add_library(stb_dxt::stb_dxt ALIAS stb_dxt)
