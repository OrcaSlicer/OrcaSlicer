cmake_minimum_required(VERSION 3.13)
project(eigen)

add_library(eigen INTERFACE)

target_include_directories(eigen SYSTEM
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/unsupported
)

# Eigen is header-only, so we only need to specify the include directory
# The headers are in the Eigen/ subdirectory structure. There is also headers
# for unsupported extras in the unsupported/Eigen subdirectory structure.
