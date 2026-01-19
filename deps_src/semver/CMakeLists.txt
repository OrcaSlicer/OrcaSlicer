cmake_minimum_required(VERSION 3.13)
project(semver)

# Create static library for semver
add_library(semver STATIC
    semver.c
    semver.h
)

# Set include directories for the library
target_include_directories(semver SYSTEM
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

# Set compile features
target_compile_features(semver PUBLIC c_std_99)

# Add any compile definitions if needed
target_compile_definitions(semver 
    PRIVATE
        # Add any private definitions here
    PUBLIC
        # Add any public definitions here
)

# Set position independent code for static library
set_target_properties(semver PROPERTIES 
    POSITION_INDEPENDENT_CODE ON
)

# Create an alias for consistent naming
add_library(semver::semver ALIAS semver)

# Export the library for use by other projects
if(NOT BUILD_SHARED_LIBS)
    set_target_properties(semver PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()

# Check if encoding_check function is available
if(COMMAND encoding_check)
    encoding_check(semver)
endif()
