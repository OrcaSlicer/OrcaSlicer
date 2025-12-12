cmake_minimum_required(VERSION 3.13)

# Verify that your CMake version is exactly 3.5 series or higher on windows
if ( (MSVC OR WIN32) AND (${CMAKE_VERSION} VERSION_LESS "3.5") )
    message(FATAL_ERROR "CMake current version ${CMAKE_VERSION} is too old. Minimum required is 3.5.")
endif()

# The following line used to be in tests/CMakeLists.txt
# Having it there causes rebuilds of all targets on any CMakeLists.txt change under tests/
# It has no effect on how code is compiled or linked.
# It just lets you later do `set_property(TARGET foo PROPERTY FOLDER "bar")`
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if (WIN32)
    # Detect known CI environments
    set(IS_CI FALSE)
    if(DEFINED ENV{CI})
        set(IS_CI TRUE)
    elseif(DEFINED ENV{GITHUB_ACTIONS})
        set(IS_CI TRUE)
    elseif(DEFINED ENV{GITLAB_CI})
        set(IS_CI TRUE)
    elseif(DEFINED ENV{TF_BUILD})
        set(IS_CI TRUE)
    elseif(DEFINED ENV{BUILD_NUMBER}) # Jenkins
        set(IS_CI TRUE)
    endif()

    # Detect common misconfiguration (Strawberry Perl in PATH before CMake)
    # We use ENV to check the PATH order
    string(REPLACE "\\" "/" ENV_PATH "$ENV{PATH}")
    string(FIND "${ENV_PATH}" "Strawberry/c/bin" STRAWBERRY_POS)
    string(FIND "${ENV_PATH}" "Program Files/CMake/bin" CMAKE_POS)

    if (STRAWBERRY_POS GREATER -1 AND CMAKE_POS GREATER -1 AND STRAWBERRY_POS LESS CMAKE_POS)
        set(_warning_text "
#############################################################
Detected Strawberry Perl's 'c/bin' appearing before CMake in PATH.
This is known to cause CMake to misbehave (e.g., missing modules).
Please adjust your PATH so that:
C:\\Program Files\\CMake\\bin
appears before:
C:\\Strawberry\\c\\bin
You can do this in Environment Variables settings.
#############################################################
        ")

        if(NOT IS_CI)
            message(FATAL_ERROR "${_warning_text}")
        endif()
    endif()
endif ()

if (APPLE)
    # if CMAKE_OSX_DEPLOYMENT_TARGET is not set, set it to 11.3
    if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.3" CACHE STRING "Minimum OS X deployment version" FORCE)
    endif ()
    message(STATUS "CMAKE_OSX_DEPLOYMENT_TARGET: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif ()

project(OrcaSlicer)

# Backward compatibility for old CMake versions
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_VERSION VERSION_LESS "3.25")
    set(LINUX ON CACHE BOOL "" FORCE)
endif ()

include("version.inc")
include(GNUInstallDirs)
include(CMakeDependentOption)

set(SLIC3R_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
file(TO_NATIVE_PATH "${SLIC3R_RESOURCES_DIR}" SLIC3R_RESOURCES_DIR_WIN)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No build type selected, default to Release")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (default Release)" FORCE)
endif()

if (DEFINED BBL_RELEASE_TO_PUBLIC)
    add_compile_definitions("BBL_RELEASE_TO_PUBLIC=${BBL_RELEASE_TO_PUBLIC}")
else ()
    add_compile_definitions("BBL_RELEASE_TO_PUBLIC=$<CONFIG:Release>")
endif ()

find_package(Git)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    if(DEFINED ENV{git_commit_hash} AND NOT "$ENV{git_commit_hash}" STREQUAL "")
        message(STATUS "Specified git commit hash: $ENV{git_commit_hash}")

        # Convert the given hash to short hash
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short "$ENV{git_commit_hash}"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        add_definitions("-DGIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"")
    else()
        # Check current Git commit hash
        execute_process(
            COMMAND ${GIT_EXECUTABLE} log -1 --format=%h
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        add_definitions("-DGIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"")
    endif()
endif()

if(DEFINED ENV{SLIC3R_STATIC})
    set(SLIC3R_STATIC_INITIAL $ENV{SLIC3R_STATIC})
else()
    set(SLIC3R_STATIC_INITIAL 1)
endif()

option(SLIC3R_STATIC 			"Compile OrcaSlicer with static libraries (Boost, TBB, glew)" ${SLIC3R_STATIC_INITIAL})
option(SLIC3R_GUI    			"Compile OrcaSlicer with GUI components (OpenGL, wxWidgets)" 1)
option(SLIC3R_FHS               "Assume OrcaSlicer is to be installed in a FHS directory structure" 0)
option(SLIC3R_WX_STABLE         "Build against wxWidgets stable (3.0) as oppsed to dev (3.1) on Linux" 0)
option(SLIC3R_PROFILE 			"Compile OrcaSlicer with an invasive Shiny profiler" 0)
option(SLIC3R_PCH               "Use precompiled headers" 1)
option(SLIC3R_MSVC_COMPILE_PARALLEL "Compile on Visual Studio in parallel" 1)
option(SLIC3R_MSVC_PDB          "Generate PDB files on MSVC in Release mode" 1)
option(SLIC3R_ASAN              "Enable ASan on Clang and GCC" 0)
# If SLIC3R_FHS is 1 -> SLIC3R_DESKTOP_INTEGRATION is always 0, othrewise variable.
CMAKE_DEPENDENT_OPTION(SLIC3R_DESKTOP_INTEGRATION "Allow perfoming desktop integration during runtime" 1 "NOT SLIC3R_FHS" 0)

set(OPENVDB_FIND_MODULE_PATH "" CACHE PATH "Path to OpenVDB installation's find modules.")

set(SLIC3R_GTK "3" CACHE STRING "GTK version to use with wxWidgets on Linux")

set(IS_CROSS_COMPILE FALSE)

option (COLORED_OUTPUT "Always produce ANSI-colored output (GNU/Clang only)." OFF)
if (${COLORED_OUTPUT})
    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        add_compile_options (-fdiagnostics-color=always)
    elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
        add_compile_options (-fcolor-diagnostics)
    endif ()
endif ()

if (APPLE)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _arch_len)
    if (_arch_len GREATER 1)
        message(FATAL_ERROR "OrcaSlicer only supports building for one architecture at a time. Please make sure only one architecture is specified in CMAKE_OSX_ARCHITECTURES")
    endif ()
    set(CMAKE_FIND_FRAMEWORK LAST)
    set(CMAKE_FIND_APPBUNDLE LAST)
    list(FIND CMAKE_OSX_ARCHITECTURES ${CMAKE_SYSTEM_PROCESSOR} _arch_idx)
    if (CMAKE_OSX_ARCHITECTURES AND _arch_idx LESS 0)
        set(IS_CROSS_COMPILE TRUE)
    endif ()
    if (CMAKE_MACOSX_BUNDLE)
        set(CMAKE_INSTALL_RPATH @executable_path/../Frameworks)
    endif()
    SET(CMAKE_XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.softfever3d.orca-slicer")

    message(STATUS "Orca: IS_CROSS_COMPILE: ${IS_CROSS_COMPILE}")
endif ()

# Proposal for C++ unit tests and sandboxes
option(SLIC3R_BUILD_SANDBOXES   "Build development sandboxes" OFF)
option(BUILD_TESTS       "Build unit tests" OFF)
option(ORCA_TOOLS       "Build Orca tools" OFF)

if (FLATPAK)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++20")
    set(SLIC3R_PCH OFF CACHE BOOL "" FORCE)
    set(SLIC3R_FHS ON CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SLIC3R_DESKTOP_INTEGRATION OFF CACHE BOOL "" FORCE)
endif ()

if (IS_CROSS_COMPILE)
    message("Detected cross compilation setup. Tests and encoding checks will be forcedly disabled!")
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif ()

# Print out the SLIC3R_* cache options
get_cmake_property(_cache_vars CACHE_VARIABLES)
list (SORT _cache_vars)
foreach (_cache_var ${_cache_vars})
    if("${_cache_var}" MATCHES "^SLIC3R_")
        message(STATUS "${_cache_var}: ${${_cache_var}}")
    endif ()
endforeach()

if (SLIC3R_GUI)
    add_definitions(-DSLIC3R_GUI)
endif ()

if(SLIC3R_DESKTOP_INTEGRATION)
    add_definitions(-DSLIC3R_DESKTOP_INTEGRATION)
endif ()

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    message(STATUS "Automatically setting CMAKE_INSTALL_PREFIX")
    set_property(CACHE CMAKE_INSTALL_PREFIX PROPERTY VALUE "${CMAKE_BINARY_DIR}/OrcaSlicer")
endif()

message(STATUS "CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")

if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL Clang)
    set(IS_CLANG_CL TRUE)

    # clang-cl can interpret SYSTEM header paths if -imsvc is used
    set(CMAKE_INCLUDE_SYSTEM_FLAG_CXX "-imsvc")

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall \
        -Wno-old-style-cast -Wno-reserved-id-macro -Wno-c++98-compat-pedantic")
else ()
    set(IS_CLANG_CL FALSE)
endif ()

if (MSVC)
    if (SLIC3R_MSVC_COMPILE_PARALLEL AND NOT IS_CLANG_CL)
           add_compile_options(/MP)
    endif ()
    # /bigobj (Increase Number of Sections in .Obj file)
    # error C3859: virtual memory range for PCH exceeded; please recompile with a command line option of '-Zm90' or greater
    # Generate symbols at every build target, even for the release.
    add_compile_options(-bigobj -Zm520 /Zi)
    # Disable STL4007: Many result_type typedefs and all argument_type, first_argument_type, and second_argument_type typedefs are deprecated in C++17.
    #FIXME Remove this line after eigen library adapts to the new C++17 adaptor rules.
    add_compile_options(-D_SILENCE_CXX17_ADAPTOR_TYPEDEFS_DEPRECATION_WARNING)
    # Disable warnings on conversion from unsigned to signed (possible loss of data)
    # C4244: 'conversion' conversion from 'type1' to 'type2', possible loss of data. An integer type is converted to a smaller integer type.
    # C4267: The compiler detected a conversion from size_t to a smaller type.
    add_compile_options(/wd4244 /wd4267)
    # Disable warnings on comparison of unsigned and signed
    # C4018: signed/unsigned mismatch
    add_compile_options(/wd4018)
endif ()

if (${CMAKE_CXX_COMPILER_ID} STREQUAL "AppleClang" AND ${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 15)
    add_compile_definitions(BOOST_NO_CXX98_FUNCTION_BASE _HAS_AUTO_PTR_ETC=0)
endif()

if (MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif ()

if (NOT MSVC)
    # ARMs (Raspberry PI) use an unsigned char by default. Let's make it consistent for OrcaSlicer on all platforms.
    add_compile_options(-fsigned-char)
endif ()

if ("${DEP_BUILD_DIR}" STREQUAL "")
    get_filename_component(BIN_DIR_NAME ${CMAKE_BINARY_DIR} NAME)
    if (APPLE AND BIN_DIR_NAME STREQUAL "${CMAKE_OSX_ARCHITECTURES}")
        file(RELATIVE_PATH BIN_DIR_NAME ${CMAKE_BINARY_DIR}/../.. ${CMAKE_BINARY_DIR})
    endif ()
    set(DEP_BUILD_DIR "${CMAKE_SOURCE_DIR}/deps/${BIN_DIR_NAME}" CACHE PATH "Path to dependencies build directory" FORCE)
    message(STATUS "DEP_BUILD_DIR: ${DEP_BUILD_DIR} (generated automatically and saved to cache)")
    set(AUTOGENERATED_DEP_BUILD_DIR ${DEP_BUILD_DIR} CACHE PATH "Provides the last autogenerated DEP_BUILD_DIR" FORCE)
else ()
    message(STATUS "DEP_BUILD_DIR: ${DEP_BUILD_DIR} (from cache or command line)")
endif ()

if ("${CMAKE_PREFIX_PATH}" STREQUAL "" OR "${CMAKE_PREFIX_PATH}" STREQUAL "${AUTOGENERATED_PREFIX_PATH}")
    if (DEFINED AUTOGENERATED_DEP_BUILD_DIR AND NOT "${DEP_BUILD_DIR}" STREQUAL "${AUTOGENERATED_DEP_BUILD_DIR}")
        message(STATUS "CMAKE_PREFIX_PATH is being re-generated due to DEP_BUILD_DIR being manually updated")
        set(REGEN_DESTDIR TRUE)
        unset(AUTOGENERATED_DEP_BUILD_DIR CACHE)
    endif ()
else ()
    unset(AUTOGENERATED_PREFIX_PATH CACHE)
endif ()

# Display and check CMAKE_PREFIX_PATH
if ("${CMAKE_PREFIX_PATH}" STREQUAL "" OR REGEN_DESTDIR)
    set(CMAKE_PREFIX_PATH "${DEP_BUILD_DIR}/OrcaSlicer_dep/usr/local" CACHE PATH "Path to dependencies install directory" FORCE)
    message(STATUS "CMAKE_PREFIX_PATH: ${CMAKE_PREFIX_PATH} (generated automatically and saved to cache)")
    set(AUTOGENERATED_PREFIX_PATH ${CMAKE_PREFIX_PATH} CACHE STRING "Provides the last autogenerated CMAKE_PREFIX_PATH" FORCE)
    unset(REGEN_DESTDIR CACHE)
else ()
    message(STATUS "CMAKE_PREFIX_PATH: ${CMAKE_PREFIX_PATH} (from cache or command line)")
endif ()
set(PREFIX_PATH_CHECK ${CMAKE_PREFIX_PATH})

# the CMAKE_PREFIX_PATH environment variable is separate from the CMAKE_PREFIX_PATH cache variable and provides additional paths to search for libraries.
if (NOT "$ENV{CMAKE_PREFIX_PATH}" STREQUAL "")
    message(STATUS "CMAKE_PREFIX_PATH: $ENV{CMAKE_PREFIX_PATH} (from environment)")
    list(APPEND PREFIX_PATH_CHECK $ENV{CMAKE_PREFIX_PATH})
endif ()

# Check all directories in CMAKE_PREFIX_PATH variables
foreach (DIR ${PREFIX_PATH_CHECK})
    if (NOT EXISTS "${DIR}")
        message(WARNING "CMAKE_PREFIX_PATH element doesn't exist: ${DIR}")
    endif ()
endforeach ()

if (APPLE)
    set(CMAKE_MACOSX_RPATH ON CACHE BOOL "")
    set(CMAKE_MACOSX_BUNDLE ON CACHE BOOL "")
endif ()

if (APPLE AND CMAKE_MACOSX_RPATH AND "${CMAKE_INSTALL_RPATH}" STREQUAL "")
    set(CMAKE_INSTALL_RPATH ${CMAKE_PREFIX_PATH})
endif ()

# Add our own cmake module path.
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules/)
message(STATUS "PROJECT_SOURCE_DIR: ${PROJECT_SOURCE_DIR}")
message(STATUS "CMAKE_MODULE_PATH: ${CMAKE_MODULE_PATH}")

enable_testing ()

# Enable C++17 language standard.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    # Add DEBUG flags to debug builds.
    add_compile_definitions("$<IF:$<CONFIG:Debug>,DEBUG,NDEBUG>")
endif()

# To be able to link libslic3r with the Perl XS module.
# Once we get rid of Perl and libslic3r is linked statically, we can get rid of -fPIC
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# WIN10SDK_PATH is used to point CMake to the WIN10 SDK installation directory.
# We pick it from environment if it is not defined in another way
if(WIN32)
    if(NOT DEFINED WIN10SDK_PATH)
        if(DEFINED ENV{WIN10SDK_PATH})
                set(WIN10SDK_PATH "$ENV{WIN10SDK_PATH}")
        endif()
    endif()
    if(DEFINED WIN10SDK_PATH)
        #BBS: modify win10sdk_path
        if (EXISTS "${WIN10SDK_PATH}/winrt/windows.graphics.printing3d.h")
            set(WIN10SDK_INCLUDE_PATH "${WIN10SDK_PATH}")
        else()
            message("WIN10SDK_PATH is invalid: ${WIN10SDK_PATH}")
            message("${WIN10SDK_PATH}/winrt/windows.graphics.printing3d.h was not found")
            message("STL fixing by the Netfabb service will not be compiled")
            unset(WIN10SDK_PATH)
        endif()
    else()
        # Try to use the default Windows 10 SDK path.
        if (DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
            set(WIN10SDK_INCLUDE_PATH "$ENV{WindowsSdkDir}/Include/$ENV{WindowsSDKVersion}")
        else ()
            set(WIN10SDK_INCLUDE_PATH "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0")
        endif ()
        if (NOT EXISTS "${WIN10SDK_INCLUDE_PATH}/winrt/windows.graphics.printing3d.h")
            message("${WIN10SDK_INCLUDE_PATH}/winrt/windows.graphics.printing3d.h was not found")
            message("STL fixing by the Netfabb service will not be compiled")
            unset(WIN10SDK_INCLUDE_PATH)
        endif()
    endif()
    if(WIN10SDK_INCLUDE_PATH)
        message("Building with Win10 Netfabb STL fixing service support")
        add_definitions(-DHAS_WIN10SDK)
        include_directories(SYSTEM "${WIN10SDK_INCLUDE_PATH}")
    else()
        message("Building without Win10 Netfabb STL fixing service support")
    endif()
endif()

if (APPLE)
    message("OS X SDK Path: ${CMAKE_OSX_SYSROOT}")
    if (CMAKE_OSX_DEPLOYMENT_TARGET)
        message("OS X Deployment Target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else ()
        message("OS X Deployment Target: (default)")
    endif ()
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)

    if (CMAKE_VERSION VERSION_LESS "3.1")
        # Workaround for an old CMake, which does not understand CMAKE_CXX_STANDARD.
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
    endif()

    # Boost on Raspberry-Pi does not link to pthreads.
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)

    find_package(DBus REQUIRED)
    include_directories(SYSTEM ${DBUS_INCLUDE_DIRS})
endif()

if (CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUXX)
    # Adding -fext-numeric-literals to enable GCC extensions on definitions of quad float literals, which are required by Boost.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fext-numeric-literals" )
endif()

if (NOT MSVC AND ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
    if (NOT MINGW)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall" )
    endif ()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-reorder" )

    # On GCC and Clang, no return from a non-void function is a warning only. Here, we make it an error.
    add_compile_options(-Werror=return-type)

    # Since some portions of code are just commented out or put under conditional compilation, there are
    # a bunch of warning related to unused functions and variables. Suppress those warnings to not pollute
    # compilers diagnostics output with warnings we not going to look at
    add_compile_options(-Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label -Wno-unused-local-typedefs)

    # Ignore signed/unsigned comparison warnings
    add_compile_options(-Wno-sign-compare)

    # The mismatch of tabs and spaces throughout the project can sometimes
    # cause this warning to appear even though the indentation is fine.
    # Some includes also cause the warning
    add_compile_options(-Wno-misleading-indentation)

    # Disable warning if enum value does not have a corresponding case in switch statement
    add_compile_options(-Wno-switch)

    # removes LOTS of extraneous Eigen warnings (GCC only supports it since 6.1)
    # https://eigen.tuxfamily.org/bz/show_bug.cgi?id=1221
    if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 6.0)
        add_compile_options(-Wno-ignored-attributes) # Tamas: Eigen include dirs are marked as SYSTEM
    endif()

    # Clang reports legacy OpenGL calls as deprecated. Turn off the warning for now
    # to reduce the clutter, we know about this one. It should be reenabled after
    # we finally get rid of the deprecated code.
    if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        add_compile_options(-Wno-deprecated-declarations)
    endif()

    if((${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang" OR ${CMAKE_CXX_COMPILER_ID} STREQUAL "AppleClang") AND ${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 15)
        add_compile_options(-Wno-error=enum-constexpr-conversion)
    endif()

    #GCC generates loads of -Wunknown-pragmas when compiling igl. The fix is not easy due to a bug in gcc, see
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66943 or
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53431
    # We will turn the warning of for GCC for now:
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        # GCC generates loads of -Wunknown-pragmas when compiling igl. The fix is not easy due to a bug in gcc, see
        # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66943 or
        # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53431
        # We will turn the warning of for GCC for now:
        add_compile_options(-Wno-unknown-pragmas)
    endif()

    # Bit of a hack for flatpak building: compress the debug info with zstd to save space in CI
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 13.0)
        add_compile_options(-gz=zstd)
    endif()

    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 14)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=template-id-cdtor" )
    endif()

endif()

if (SLIC3R_ASAN)
    # ASAN should be available on MSVC starting with Visual Studio 2019 16.9
    # https://devblogs.microsoft.com/cppblog/address-sanitizer-for-msvc-now-generally-available/
    add_compile_options(-fsanitize=address)

    if (NOT MSVC)
        add_compile_options(-fno-omit-frame-pointer)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
        set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -fsanitize=address")
    else()
        add_compile_definitions(_DISABLE_STRING_ANNOTATION=1 _DISABLE_VECTOR_ANNOTATION=1)
    endif ()

    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lasan")
    endif ()
endif ()

if (APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=partial-availability -Werror=unguarded-availability -Werror=unguarded-availability-new")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=partial-availability -Werror=unguarded-availability -Werror=unguarded-availability-new")
endif ()

if(MSVC)
# 添加编译选项，忽略警告 C4305 （格式转换截断）
add_compile_options(/wd4305)
endif()

# Where all the bundled libraries reside?
set(LIBDIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(LIBDIR_BIN ${CMAKE_CURRENT_BINARY_DIR}/src)
message(STATUS "LIBDIR: ${LIBDIR}")
message(STATUS "LIBDIR_BIN: ${LIBDIR_BIN}")

# For the bundled boost libraries (boost::nowide)
include_directories(SYSTEM ${LIBDIR})
# For generated header files
include_directories(SYSTEM ${LIBDIR_BIN}/dev-utils/platform)
# For ligigl
include_directories(SYSTEM ${LIBDIR}/libigl)

if(WIN32)
    add_definitions(-D_USE_MATH_DEFINES -D_WIN32 -D_CRT_SECURE_NO_WARNINGS -D_SCL_SECURE_NO_WARNINGS)
    if(MSVC)
        # BOOST_ALL_NO_LIB: Avoid the automatic linking of Boost libraries on Windows. Rather rely on explicit linking.
        add_definitions(-DBOOST_ALL_NO_LIB -DBOOST_USE_WINAPI_VERSION=0x602 -DBOOST_SYSTEM_USE_UTF8 )
        # Force the source code encoding to UTF-8. See OrcaSlicer GH pull request #5583
        add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
        add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    endif(MSVC)
endif(WIN32)

add_definitions(-DwxUSE_UNICODE -D_UNICODE -DUNICODE -DWXINTL_NO_GETTEXT_MACRO)

# Disable unsafe implicit wxString to const char* / std::string and vice versa. This implicit conversion breaks the UTF-8 encoding quite often.
add_definitions(-DwxNO_UNSAFE_WXSTRING_CONV)

if (SLIC3R_PROFILE)
    message("OrcaSlicer will be built with a Shiny invasive profiler")
    add_definitions(-DSLIC3R_PROFILE)
endif ()

# Disable optimization for RelWithDebInfo
if(CMAKE_C_FLAGS_RELWITHDEBINFO MATCHES "/O2")
    string(REGEX REPLACE "/O2" "/Od" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endif()

if(CMAKE_CXX_FLAGS_RELWITHDEBINFO MATCHES "/O2")
    string(REGEX REPLACE "/O2" "/Od" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
endif()

if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    if(CMAKE_C_FLAGS_RELWITHDEBINFO MATCHES "-O2")
        string(REGEX REPLACE "-O2" "-O0" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    endif()

    if(CMAKE_CXX_FLAGS_RELWITHDEBINFO MATCHES "-O2")
        string(REGEX REPLACE "-O2" "-O0" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    endif()
endif()

if(MSVC)
    if(CMAKE_C_FLAGS_RELWITHDEBINFO MATCHES "/Ob1")
        string(REGEX REPLACE "/Ob1" "/Ob0" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    endif()
    if(CMAKE_CXX_FLAGS_RELWITHDEBINFO MATCHES "/Ob1")
        string(REGEX REPLACE "/Ob1" "/Ob0" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    endif()

    set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} /RTC1")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /RTC1")
endif()

list(REMOVE_DUPLICATES CMAKE_C_FLAGS_RELWITHDEBINFO)
list(REMOVE_DUPLICATES CMAKE_CXX_FLAGS_RELWITHDEBINFO)

# Find and configure boost
if(SLIC3R_STATIC)
    # Use static boost libraries.
    set(Boost_USE_STATIC_LIBS ON)
    # Use boost libraries linked statically to the C++ runtime.
    # set(Boost_USE_STATIC_RUNTIME ON)
else()
    add_definitions(-DBOOST_LOG_DYN_LINK)
endif()
#set(Boost_DEBUG ON)
# set(Boost_COMPILER "-mgw81")
# boost::process was introduced first in version 1.64.0,
# boost::beast::detail::base64 was introduced first in version 1.66.0
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.83.0 REQUIRED COMPONENTS system filesystem thread log log_setup locale regex chrono atomic date_time iostreams program_options nowide)

add_library(boost_libs INTERFACE)
add_library(boost_headeronly INTERFACE)

if (APPLE)
    # BOOST_ASIO_DISABLE_KQUEUE : prevents a Boost ASIO bug on OS X: https://svn.boost.org/trac/boost/ticket/5339
    target_compile_definitions(boost_headeronly INTERFACE BOOST_ASIO_DISABLE_KQUEUE)
endif()

if(NOT SLIC3R_STATIC)
    target_compile_definitions(boost_headeronly INTERFACE BOOST_LOG_DYN_LINK)
endif()

function(slic3r_remap_configs targets from_Cfg to_Cfg)
    if(MSVC)
        string(TOUPPER ${from_Cfg} from_CFG)

        foreach(tgt ${targets})
            if(TARGET ${tgt})
                set_target_properties(${tgt} PROPERTIES MAP_IMPORTED_CONFIG_${from_CFG} ${to_Cfg})
            endif()
        endforeach()
    endif()
endfunction()

target_include_directories(boost_headeronly SYSTEM INTERFACE ${Boost_INCLUDE_DIRS})
target_link_libraries(boost_libs INTERFACE boost_headeronly ${Boost_LIBRARIES})

# Find and configure intel-tbb
if(SLIC3R_STATIC)
    set(TBB_STATIC 1)
endif()
set(TBB_DEBUG 1)
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO RelWithDebInfo Release "")
find_package(TBB REQUIRED)
# include_directories(SYSTEM ${TBB_INCLUDE_DIRS})
# add_definitions(${TBB_DEFINITIONS})
# if(MSVC)
#     # Suppress implicit linking of the TBB libraries by the Visual Studio compiler.
#     add_definitions(-D__TBB_NO_IMPLICIT_LINKAGE)
# endif()
# The Intel TBB library will use the std::exception_ptr feature of C++11.
# add_definitions(-DTBB_USE_CAPTURED_EXCEPTION=0)

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(Freetype REQUIRED)


add_library(libcurl INTERFACE)
target_link_libraries(libcurl INTERFACE CURL::libcurl)

find_package(ZLIB REQUIRED)
target_link_libraries(libcurl INTERFACE ZLIB::ZLIB)

# Fixing curl's cmake config script bugs
if (NOT WIN32)
    # Required by libcurl
    #find_package(ZLIB REQUIRED)
    #target_link_libraries(libcurl INTERFACE ZLIB::ZLIB)
    #find_package(Libssh2 REQUIRED)
    #target_link_libraries(libcurl INTERFACE Libssh2::libssh2)
else()
    target_link_libraries(libcurl INTERFACE crypt32)
endif()

if (SLIC3R_STATIC AND NOT SLIC3R_STATIC_EXCLUDE_CURL)
    if (NOT APPLE)
        # libcurl is always linked dynamically to the system libcurl on OSX.
        # On other systems, libcurl is linked statically if SLIC3R_STATIC is set.
        target_compile_definitions(libcurl INTERFACE CURL_STATICLIB)
    endif()
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # As of now, our build system produces a statically linked libcurl,
        # which links the OpenSSL library dynamically.
        find_package(OpenSSL REQUIRED)
        message("OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
        message("OpenSSL libraries: ${OPENSSL_LIBRARIES}")
        target_include_directories(libcurl SYSTEM INTERFACE ${OPENSSL_INCLUDE_DIR})
        target_link_libraries(libcurl INTERFACE ${OPENSSL_LIBRARIES})
    endif()
endif()

## OPTIONAL packages

# Find eigen3 or use bundled version
if (NOT SLIC3R_STATIC)
    find_package(Eigen3 3.3)
endif ()
if (NOT EIGEN3_FOUND)
    set(EIGEN3_FOUND 1)
    set(EIGEN3_INCLUDE_DIR ${LIBDIR}/eigen/)
endif ()
include_directories(BEFORE SYSTEM ${EIGEN3_INCLUDE_DIR})

# Find expat or use bundled version
# Always use the system libexpat on Linux.

find_package(EXPAT)

if (NOT EXPAT_FOUND)
    add_library(expat STATIC
        ${LIBDIR}/expat/xmlparse.c
        ${LIBDIR}/expat/xmlrole.c
        ${LIBDIR}/expat/xmltok.c
    )
    set(EXPAT_FOUND 1)
    set(EXPAT_INCLUDE_DIRS ${LIBDIR}/expat/)
    set(EXPAT_LIBRARIES expat)
endif ()

find_package(PNG REQUIRED)

set(OpenGL_GL_PREFERENCE "LEGACY")
find_package(OpenGL REQUIRED)

set(GLEW_ROOT "${CMAKE_PREFIX_PATH}")
message("GLEW_ROOT: ${GLEW_ROOT}")
# Find glew or use bundled version
if (SLIC3R_STATIC AND NOT SLIC3R_STATIC_EXCLUDE_GLEW)
    set(GLEW_USE_STATIC_LIBS ON)
    set(GLEW_VERBOSE ON)
endif()

find_package(GLEW REQUIRED)

find_package(glfw3 REQUIRED)

# Find the Cereal serialization library
find_package(cereal REQUIRED)
if (NOT TARGET cereal::cereal)
    set_target_properties(cereal PROPERTIES IMPORTED_GLOBAL TRUE)
    add_library(cereal::cereal ALIAS cereal)
else ()
    set_target_properties(cereal::cereal PROPERTIES IMPORTED_GLOBAL TRUE)
endif ()

# l10n
set(L10N_DIR "${SLIC3R_RESOURCES_DIR}/i18n")
set(BBL_L18N_DIR "${CMAKE_CURRENT_SOURCE_DIR}/localization/i18n")
add_custom_target(gettext_make_pot
    COMMAND xgettext --keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2 --add-comments=TRN --from-code=UTF-8 --no-location --debug --boost
        -f "${BBL_L18N_DIR}/list.txt"
        -o "${BBL_L18N_DIR}/OrcaSlicer.pot"
	COMMAND hintsToPot ${SLIC3R_RESOURCES_DIR} ${BBL_L18N_DIR}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generate pot file from strings in the source tree"
)
add_custom_target(gettext_merge_po_with_pot
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Merge localization po with new generted pot file"
)
file(GLOB BBL_L10N_PO_FILES "${BBL_L18N_DIR}/*/OrcaSlicer*.po")
foreach(po_file ${BBL_L10N_PO_FILES})
    GET_FILENAME_COMPONENT(po_dir "${po_file}" DIRECTORY)
    SET(po_new_file "${po_dir}/OrcaSlicer_.po")
    add_custom_command(
        TARGET gettext_merge_po_with_pot PRE_BUILD
        COMMAND msgmerge -N -o ${po_file} ${po_file} "${BBL_L18N_DIR}/OrcaSlicer.pot"
        DEPENDS ${po_file}
    )
endforeach()
add_custom_target(gettext_po_to_mo
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generate localization po files (binary) from mo files (texts)"
)
file(GLOB L10N_PO_FILES "${BBL_L18N_DIR}/*/OrcaSlicer*.po")
foreach(po_file ${BBL_L10N_PO_FILES})
    GET_FILENAME_COMPONENT(SECOND_FOLDER_ABSOLUTE ${po_file} DIRECTORY)
    string(REGEX REPLACE ".*/(.*)" "\\1" po_dir "${SECOND_FOLDER_ABSOLUTE}" )
    SET(mo_file "${L10N_DIR}/${po_dir}/OrcaSlicer.mo")
    add_custom_command(
        TARGET gettext_po_to_mo PRE_BUILD
        COMMAND msgfmt ARGS --check-format -o ${mo_file} ${po_file}
        #COMMAND msgfmt ARGS --check-compatibility -o ${mo_file} ${po_file}
        DEPENDS ${po_file}
    )
endforeach()

find_package(NLopt 1.4 REQUIRED)


if(SLIC3R_STATIC)
    set(OPENVDB_USE_STATIC_LIBS ON)
    set(USE_BLOSC TRUE)
endif ()

find_package(OpenVDB 5.0 COMPONENTS openvdb)
if(OpenVDB_FOUND)
    slic3r_remap_configs(IlmBase::Half RelWithDebInfo Release)
    slic3r_remap_configs(Blosc::blosc RelWithDebInfo Release)
else ()
    message(FATAL_ERROR "OpenVDB could not be found with the bundled find module. "
                   "You can try to specify the find module location of your "
                   "OpenVDB installation with the OPENVDB_FIND_MODULE_PATH cache variable.")
endif ()

find_path(SPNAV_INCLUDE_DIR spnav.h)
if (SPNAV_INCLUDE_DIR)
    find_library(SPNAV_LIB NAMES libspnav.a) # Force linking libspnav statically
    if (SPNAV_LIB)
        add_definitions(-DHAVE_SPNAV)
        message(STATUS "SPNAV library found")
    else()
        message(STATUS "SPNAV library NOT found, Spacenavd not supported")
    endif()
else()
    message(STATUS "SPNAV library NOT found, Spacenavd not supported")
endif()

set(TOP_LEVEL_PROJECT_DIR ${PROJECT_SOURCE_DIR})
function(orcaslicer_copy_dlls target config postfix output_dlls)
    if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "AMD64")
        set(_arch "x64")
    elseif ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "X86")
        set(_arch "x86")
    else ()
        message(FATAL_ERROR "Unable to detect architecture")
    endif ()

    get_property(_is_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    get_target_property(_alt_out_dir ${target} RUNTIME_OUTPUT_DIRECTORY)

    if (_alt_out_dir)
        set (_out_dir "${_alt_out_dir}")
        message ("set out_dir to _alt_out_dir: ${_out_dir}")
    elseif (_is_multi)
        set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/${config}")
        message ("set out_dir to CMAKE_CURRENT_BINARY_DIR/config: ${_out_dir}")
    else ()
        set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}")
        message ("set out_dir to CMAKE_CURRENT_BINARY_DIR: ${_out_dir}")
    endif ()

    file(COPY ${TOP_LEVEL_PROJECT_DIR}/deps/GMP/gmp/lib/win-${_arch}/libgmp-10.dll
            ${TOP_LEVEL_PROJECT_DIR}/deps/MPFR/mpfr/lib/win-${_arch}/libmpfr-4.dll
            ${TOP_LEVEL_PROJECT_DIR}/deps/WebView2/lib/win-${_arch}/WebView2Loader.dll
        DESTINATION ${_out_dir})

    file(COPY ${CMAKE_PREFIX_PATH}/bin/occt/TKBO.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKBRep.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKCAF.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKCDF.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKernel.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKG2d.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKG3d.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKGeomAlgo.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKGeomBase.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKHLR.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKLCAF.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKMath.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKMesh.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKPrim.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKService.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKShHealing.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKSTEP.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKSTEP209.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKSTEPAttr.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKSTEPBase.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKTopAlgo.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKV3d.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKVCAF.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKXCAF.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKXDESTEP.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKXSBase.dll
            ${CMAKE_PREFIX_PATH}/bin/freetype.dll
         DESTINATION ${_out_dir})

    set(${output_dlls}
        ${_out_dir}/libgmp-10.dll
        ${_out_dir}/libmpfr-4.dll
        ${_out_dir}/WebView2Loader.dll

        ${_out_dir}/TKBO.dll
        ${_out_dir}/TKBRep.dll
        ${_out_dir}/TKCAF.dll
        ${_out_dir}/TKCDF.dll
        ${_out_dir}/TKernel.dll
        ${_out_dir}/TKG2d.dll
        ${_out_dir}/TKG3d.dll
        ${_out_dir}/TKGeomAlgo.dll
        ${_out_dir}/TKGeomBase.dll
        ${_out_dir}/TKHLR.dll
        ${_out_dir}/TKLCAF.dll
        ${_out_dir}/TKMath.dll
        ${_out_dir}/TKMesh.dll
        ${_out_dir}/TKPrim.dll
        ${_out_dir}/TKService.dll
        ${_out_dir}/TKShHealing.dll
        ${_out_dir}/TKSTEP.dll
        ${_out_dir}/TKSTEP209.dll
        ${_out_dir}/TKSTEPAttr.dll
        ${_out_dir}/TKSTEPBase.dll
        ${_out_dir}/TKTopAlgo.dll
        ${_out_dir}/TKV3d.dll
        ${_out_dir}/TKVCAF.dll
        ${_out_dir}/TKXCAF.dll
        ${_out_dir}/TKXDESTEP.dll
        ${_out_dir}/TKXSBase.dll

        ${_out_dir}/freetype.dll

        PARENT_SCOPE
    )

endfunction()


# libslic3r, OrcaSlicer GUI and the OrcaSlicer executable.
add_subdirectory(deps_src)
add_subdirectory(src)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT OrcaSlicer_app_gui)

add_dependencies(gettext_make_pot hintsToPot)

if(SLIC3R_BUILD_SANDBOXES)
    add_subdirectory(sandboxes)
endif()

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

if (NOT WIN32 AND NOT APPLE)
    set(SLIC3R_APP_CMD "orca-slicer")
    configure_file(${LIBDIR}/dev-utils/platform/unix/build_appimage.sh.in ${CMAKE_CURRENT_BINARY_DIR}/build_appimage.sh USE_SOURCE_PERMISSIONS @ONLY)
endif()


# Resources install target, configure fhs.hpp on UNIX
if (WIN32)
    install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "./resources")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    install (PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION ".")
elseif (SLIC3R_FHS)
    # CMAKE_INSTALL_FULL_DATAROOTDIR: read-only architecture-independent data root (share)
    set(SLIC3R_FHS_RESOURCES "${CMAKE_INSTALL_FULL_DATAROOTDIR}/OrcaSlicer")
    install(DIRECTORY ${SLIC3R_RESOURCES_DIR}/ DESTINATION ${SLIC3R_FHS_RESOURCES}
        PATTERN "*/udev" EXCLUDE
    )
    install(FILES src/dev-utils/platform/unix/OrcaSlicer.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    foreach(SIZE 32 128 192)
        install(FILES ${SLIC3R_RESOURCES_DIR}/images/OrcaSlicer_${SIZE}px.png
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${SIZE}x${SIZE}/apps RENAME OrcaSlicer.png
        )
    endforeach()
elseif (CMAKE_MACOSX_BUNDLE)
    # install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "${CMAKE_INSTALL_PREFIX}/OrcaSlicer.app/Contents/resources")
else ()
    install(FILES src/dev-utils/platform/unix/OrcaSlicer.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/resources/applications)
    install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "${CMAKE_INSTALL_PREFIX}/resources")
endif ()

install(FILES ${CMAKE_SOURCE_DIR}/LICENSE.txt DESTINATION ".")
configure_file(${LIBDIR}/dev-utils/platform/unix/fhs.hpp.in ${LIBDIR_BIN}/dev-utils/platform/unix/fhs.hpp)

set (CPACK_PACKAGE_NAME "OrcaSlicer")
set (CPACK_PACKAGE_VENDOR "SoftFever")
set (CPACK_PACKAGE_VERSION_MAJOR "${ORCA_VERSION_MAJOR}")
set (CPACK_PACKAGE_VERSION_MINOR "${ORCA_VERSION_MINOR}")
set (CPACK_PACKAGE_VERSION_PATCH "${ORCA_VERSION_PATCH}")
set (CPACK_PACKAGE_FILE_NAME "OrcaSlicer_Windows_Installer_V${SoftFever_VERSION}")
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Orca Slicer is an open source slicer for FDM printers")
set (CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/OrcaSlicer/OrcaSlicer")
set (CPACK_PACKAGE_INSTALL_DIRECTORY ${CPACK_PACKAGE_NAME})
set (CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/resources/images\\\\OrcaSlicer.ico")
set (CPACK_NSIS_MUI_ICON "${CPACK_PACKAGE_ICON}")
set (CPACK_NSIS_MUI_UNIICON "${CPACK_PACKAGE_ICON}")
set (CPACK_NSIS_INSTALLED_ICON_NAME  "$INSTDIR\\\\orca-slicer.exe")
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
    CreateShortCut \\\"$DESKTOP\\\\OrcaSlicer.lnk\\\" \\\"$INSTDIR\\\\orca-slicer.exe\\\"
")
set (CPACK_PACKAGE_CHECKSUM SHA256)
set (CPACK_PACKAGE_INSTALL_REGISTRY_KEY "OrcaSlicer")
set (CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set (CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
# set (CPACK_NSIS_MODIFY_PATH "ON")
set(CPACK_PACKAGE_EXECUTABLES "orca-slicer;OrcaSlicer")
set(CPACK_CREATE_DESKTOP_LINKS "orca-slicer")
set (CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE.txt) # must also include in install command

set(CPACK_WIX_UPGRADE_GUID "058245e8-20e0-4a95-9ab7-1acfe17ad511")
set(CPACK_GENERATOR NSIS)
include(CPack)
