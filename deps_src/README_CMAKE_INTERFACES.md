# CMake Interfaces for deps_src Libraries

This document describes how to use the CMake interface libraries created for the subdirectories in `deps_src/`.

## Available Libraries

### 1. **semver** (Static Library)
- **Type**: Static library
- **Target**: `semver` or `semver::semver`
- **Headers**: `semver.h`
- **Usage**:
```cmake
target_link_libraries(your_target PRIVATE semver)
# or
target_link_libraries(your_target PRIVATE semver::semver)
```

### 2. **hints** (Interface Library)
- **Type**: Interface library (header-only)
- **Target**: `hints`
- **Utility**: `hintsToPot` executable
- **Usage**:
```cmake
target_link_libraries(your_target PRIVATE hints)
```

### 3. **stb_dxt** (Interface Library)
- **Type**: Interface library (header-only)
- **Target**: `stb_dxt` or `stb_dxt::stb_dxt`
- **Headers**: `stb_dxt.h`
- **Usage**:
```cmake
target_link_libraries(your_target PRIVATE stb_dxt)
# or
target_link_libraries(your_target PRIVATE stb_dxt::stb_dxt)
```

## How to Use in Your Project

### From within the OrcaSlicer src/ directory:

1. **In your CMakeLists.txt**, simply link the library:
```cmake
add_executable(my_app main.cpp)
target_link_libraries(my_app
    PRIVATE
        semver::semver      # For version parsing
        stb_dxt::stb_dxt    # For DXT texture compression
        hints               # For hints functionality
)
```

2. **In your C++ code**, include the headers:
```cpp
// For semver
#include <semver.h>

// For stb_dxt
#include <stb_dxt.h>

// Use the libraries as documented in their respective headers
```

## Benefits of This Approach

1. **Modern CMake**: Uses target-based CMake with proper INTERFACE/PUBLIC/PRIVATE visibility
2. **Proper Include Paths**: Automatically sets up include directories
3. **Namespace Aliases**: Provides namespaced aliases (e.g., `spline::spline`) for clarity
4. **Position Independent Code**: Static libraries are built with `-fPIC` for compatibility
5. **Install Support**: Libraries can be installed and used by external projects
6. **Build/Install Interface**: Separates build-time and install-time include paths

## Example Integration

Here's a complete example of using these libraries in a new component:

```cmake
# In src/mycomponent/CMakeLists.txt
add_library(mycomponent STATIC
    mycomponent.cpp
    mycomponent.h
)

target_link_libraries(mycomponent
    PUBLIC
        semver::semver  # Version handling is part of public API
    PRIVATE
        stb_dxt::stb_dxt # Used internally for texture compression
)

target_include_directories(mycomponent
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)
```

## Notes

- All header-only libraries use the INTERFACE library type, which means they don't produce any binaries
- The `semver` library produces a static library that will be linked into your target
- The `hints` project also produces a `hintsToPot` executable utility
- All libraries require at least C++11 (some require C++17)