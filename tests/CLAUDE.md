# CLAUDE.md - Testing Guide for OrcaSlicer

This guide provides comprehensive instructions for Claude Code when writing, maintaining, and understanding tests in the OrcaSlicer codebase.

## ⚠️ CRITICAL RULES - MUST FOLLOW

### 1. **SECTIONS IN LOOPS - NEVER REUSE NAMES**
❌ **WRONG**: Will cause unpredictable behavior
```cpp
TEST_CASE("Bad loop sections") {
    for (int i = 0; i < 3; ++i) {
        SECTION("Same name") {  // WRONG! Same name used multiple times
            REQUIRE(i >= 0);
        }
    }
}
```

✅ **CORRECT**: Use DYNAMIC_SECTION or incorporate counter
```cpp
TEST_CASE("Good loop sections") {
    for (int i = 0; i < 3; ++i) {
        DYNAMIC_SECTION("Section " << i) {  // Unique name per iteration
            REQUIRE(i >= 0);
        }
    }
}
```

### 2. **THREAD SAFETY - ASSERTIONS ARE NOT THREAD-SAFE**
❌ **WRONG**: Will cause undefined behavior or crashes
```cpp
TEST_CASE("Multi-threaded test") {
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([]() {
            REQUIRE(some_calculation() == expected);  // NOT THREAD-SAFE!
        });
    }
}
```

✅ **CORRECT**: Synchronize results, test on main thread
```cpp
TEST_CASE("Multi-threaded test") {
    std::vector<std::thread> threads;
    std::atomic<int> passed{0};
    
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&passed]() {
            if (some_calculation() == expected) {
                passed++;
            }
        });
    }
    
    for (auto& t : threads) t.join();
    REQUIRE(passed == 4);  // Test results on main thread
}
```

### 3. **EXPRESSION DECOMPOSITION - AVOID BINARY OPERATORS**
❌ **WRONG**: Cannot decompose properly
```cpp
REQUIRE(a > 0 && b < 10);  // Shows "false" on failure, not individual values
```

✅ **CORRECT**: Split into separate assertions
```cpp
REQUIRE(a > 0);
REQUIRE(b < 10);  // Each shows individual values on failure
```

### 4. **FLOATING POINT - NEVER USE APPROX**
❌ **WRONG**: Approx is deprecated and asymmetric
```cpp
REQUIRE(calculated_value == Catch::Approx(expected));  // Deprecated!
```

✅ **CORRECT**: Use floating point matchers
```cpp
REQUIRE_THAT(calculated_value, WithinAbs(expected, 0.001));
REQUIRE_THAT(calculated_value, WithinRel(expected, 0.01));  // 1% tolerance
REQUIRE_THAT(calculated_value, WithinULP(expected, 4));     // 4 ULPs apart
```

### 5. **TEST ORDERING - ALWAYS USE RANDOM ORDER**
✅ **REQUIRED**: For CI/CD and development
```bash
# Essential flags for running tests
./tests --order rand --warn NoAssertions

# For test sharding (parallel execution), share random seed
./tests --order rand --shard-index 0 --shard-count 3 --rng-seed 0xBEEF
./tests --order rand --shard-index 1 --shard-count 3 --rng-seed 0xBEEF
./tests --order rand --shard-index 2 --shard-count 3 --rng-seed 0xBEEF
```

## Overview of OrcaSlicer's Testing Framework

OrcaSlicer uses **Catch2 v2** as its primary testing framework. The test suite is organized into several modules that mirror the project's architectural components:

> **Note**: OrcaSlicer currently uses Catch2 v2 (based on `#include <catch2/catch.hpp>` includes). Some features mentioned in this guide are only available in v3 and marked accordingly.

### Test Structure
```
tests/
├── CMakeLists.txt              # Main test configuration
├── catch_main.hpp              # Custom test reporter
├── libslic3r/                  # Core library tests (21 test files)
├── fff_print/                  # FFF printing tests (12 test files)
├── sla_print/                  # SLA printing tests (4 test files)
├── libnest2d/                  # 2D nesting tests
├── slic3rutils/               # Utility tests
├── data/                      # Test data files and meshes
└── catch2/                    # Catch2 framework files
```

### Build Integration
- Tests are built using CMake with `catch_discover_tests()` integration
- Each test module creates a separate executable (e.g., `libslic3r_tests`, `fff_print_tests`)
- Test data directory is available via `TEST_DATA_DIR` preprocessor definition
- Custom verbose console reporter provides detailed test output

## Test Suite Organization

### libslic3r Tests
Core slicing engine tests covering:
- **Geometry operations**: Points, polygons, lines, Voronoi diagrams
- **File formats**: STL, 3MF, AMF parsing and validation
- **Algorithms**: Clipper operations, mesh boolean operations, optimization
- **Configuration**: Print settings validation and parsing
- **Utilities**: String processing, time utilities, data structures

### fff_print Tests
Fused Filament Fabrication specific tests:
- **G-code generation**: Writer functionality, cooling, lift/unlift
- **Slicing algorithms**: Layer generation, infill patterns
- **Print mechanics**: Flow calculations, extrusion, support material
- **Model processing**: Print objects, skirt/brim generation

### sla_print Tests
Stereolithography specific tests:
- **SLA print processing**: Layer curing, support generation
- **Raycast operations**: Light path calculations
- **Test utilities**: SLA-specific helper functions

## Writing New Tests - Best Practices

### File Organization
1. **Naming Convention**: `test_<feature>.cpp` (e.g., `test_geometry.cpp`)
2. **Header Structure**: Include `<catch2/catch.hpp>` first, then relevant headers
3. **Namespace Usage**: Use `using namespace Slic3r;` for convenience
4. **File Placement**: Add to appropriate test directory and update CMakeLists.txt

### Test Naming and Structure
```cpp
#include <catch2/catch.hpp>
#include "libslic3r/Point.hpp"

using namespace Slic3r;

TEST_CASE("Feature description", "[category_tag]") {
    // Test implementation
}
```

### Tagging System
Use descriptive tags for test categorization:
- `[Geometry]` - Geometric operations and calculations
- `[GCodeWriter]` - G-code generation functionality  
- `[Config]` - Configuration and settings tests
- `[FileFormat]` - File I/O operations (STL, 3MF, etc.)
- `[Algorithm]` - Core algorithms and processing
- `[Performance]` - Performance benchmarks (if applicable)

## Catch2 Features Guide

### Basic Assertions
```cpp
// Primary assertions - stop test on failure
REQUIRE(expression);
REQUIRE_FALSE(expression);

// Continuing assertions - continue test after failure  
CHECK(expression);
CHECK_FALSE(expression);

// Non-failing checks - record result but don't fail test
CHECK_NOFAIL(expression);  // Useful for assumptions that might be violated
```

### Exception Testing
```cpp
// Verify no exception is thrown
REQUIRE_NOTHROW(function_call());

// Verify any exception is thrown
REQUIRE_THROWS(risky_function());

// Verify specific exception type
REQUIRE_THROWS_AS(function_call(), SpecificException);

// Verify exception message
REQUIRE_THROWS_WITH(function_call(), "Expected error message");

// Verify exception with matchers (for partial matching)
REQUIRE_THROWS_MATCHES(function_call(), SpecificException, 
                       Catch::Matchers::Message("contains this"));
```

### Complex Assertions with Matchers
```cpp
#include <catch2/matchers/catch_matchers.hpp>

// String matchers
using Catch::Matchers::StartsWith;
using Catch::Matchers::EndsWith;
using Catch::Matchers::ContainsSubstring;  // Note: v2 uses "Contains"
using Catch::Matchers::Equals;
using Catch::Matchers::Matches;  // Regex matching

REQUIRE_THAT(result_string, StartsWith("Expected prefix"));
REQUIRE_THAT(result_string, ContainsSubstring("middle part"));
REQUIRE_THAT(result_string, Matches(".*pattern.*"));

// Floating point matchers - ALWAYS use these instead of Approx!
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

REQUIRE_THAT(float_value, WithinAbs(expected, 0.001));     // Absolute tolerance
REQUIRE_THAT(float_value, WithinRel(expected, 0.01));      // Relative tolerance (1%)
REQUIRE_THAT(float_value, WithinULP(expected, 4));         // ULP difference (requires IEEE-754)

// Combining matchers
REQUIRE_THAT(value, WithinRel(expected, 0.001) || WithinAbs(0.0, 0.000001));
```

### Sections for Test Organization
```cpp
TEST_CASE("Complex feature testing", "[Feature]") {
    // Common setup code
    SomeObject obj;
    
    SECTION("First scenario") {
        // Specific test case
        REQUIRE(obj.method1() == expected_value);
    }
    
    SECTION("Second scenario") {
        // Another test case with same setup
        REQUIRE(obj.method2() == other_expected);
    }
}
```

### BDD-Style Tests
Use for complex scenarios and user story testing:

> **Note**: BDD macros are aliases for TEST_CASE and SECTION with prefixed names
```cpp
SCENARIO("User performs complex operation", "[UserStory]") {
    GIVEN("A specific setup condition") {
        GCodeWriter writer;
        // Setup code
        
        WHEN("User performs action") {
            auto result = writer.some_operation();
            
            THEN("Expected outcome occurs") {
                REQUIRE(result.size() > 0);
                
                AND_WHEN("Follow-up action occurs") {
                    auto next_result = writer.next_operation();
                    
                    THEN("Final outcome is correct") {
                        REQUIRE(next_result == expected);
                    }
                }
            }
        }
    }
}
```

### Data Generators for Parameterized Tests
```cpp
TEST_CASE("Function works with various inputs", "[Algorithm]") {
    auto test_value = GENERATE(1, 3, 5, 7, 11, 13);
    
    REQUIRE(is_odd(test_value));
    REQUIRE(test_value > 0);
}

// Range-based generators
TEST_CASE("Range testing", "[Algorithm]") {
    auto i = GENERATE(range(1, 10));  // 1 to 9
    REQUIRE(process_value(i) > i);
}

// Using variables in generators (requires GENERATE_COPY or GENERATE_REF)
TEST_CASE("Generator with variables", "[Algorithm]") {
    std::vector<int> values = {1, 2, 3, 4, 5};
    auto test_value = GENERATE_REF(from_range(values));  // Use GENERATE_REF for references
    
    REQUIRE(test_value > 0);
}

// Custom generators
TEST_CASE("Random values", "[Algorithm]") {
    auto random_int = GENERATE(take(100, random(-1000, 1000)));  // 100 random values
    REQUIRE(process_random_value(random_int));
}
```

### Test Fixtures
```cpp
class GeometryFixture {
public:
    Point origin{0, 0};
    Point unit_x{1, 0};
    Point unit_y{0, 1};
    
    mutable double tolerance = EPSILON;  // Use mutable for data that might change
};

// Standard fixture - new instance per test run
TEST_CASE_METHOD(GeometryFixture, "Point operations", "[Geometry]") {
    REQUIRE(origin.distance_to(unit_x) == 1.0);
}

// Persistent fixture - single instance for entire test case (v2.12.0+)
TEST_CASE_PERSISTENT_FIXTURE(GeometryFixture, "Persistent operations", "[Geometry]") {
    static int call_count = 0;
    ++call_count;
    INFO("This fixture persists across sections, call: " << call_count);
    
    SECTION("First section") {
        REQUIRE(origin.distance_to(unit_x) == 1.0);
    }
    
    SECTION("Second section") {
        REQUIRE(origin.distance_to(unit_y) == 1.0);
        // call_count will be 2 here with persistent fixture
    }
}

// Template fixtures for type-parameterized tests
template<typename T>
class NumericFixture {
public:
    T zero = T{0};
    T one = T{1};
};

TEMPLATE_TEST_CASE_METHOD(NumericFixture, "Numeric operations", "[Template]", int, float, double) {
    REQUIRE(TestType{} == this->zero);
    REQUIRE(TestType{1} == this->one);
}
```

### Advanced Testing Features

#### Logging and Information Macros
```cpp
TEST_CASE("Advanced logging", "[Logging]") {
    INFO("This info persists until end of scope");
    
    SECTION("Section A") {
        INFO("Section A specific info");
        CAPTURE(some_variable, another_var);  // Captures variable names and values
        CHECK(some_condition);
    }
    
    SECTION("Section B") {
        UNSCOPED_INFO("This survives beyond its scope");  // v2.7.0+
        CHECK(other_condition);
    }
}

// Warning and explicit control
TEST_CASE("Explicit test control", "[Control]") {
    WARN("This warns but doesn't fail the test");
    
    if (precondition_not_met) {
        // SKIP("Reason");  // v3.3.0+ only, not available in v2
        SUCCEED("Test cannot run due to precondition");  // v2 alternative
        return;
    }
    
    if (critical_failure) {
        FAIL("Critical condition failed");  // Fails and stops test
    }
    
    SUCCEED("Reached successful completion");  // Explicit success marker
}
```

#### Static Assertions (Compile-time Testing)
```cpp
TEST_CASE("Compile-time checks", "[Static]") {
    STATIC_REQUIRE(sizeof(int) >= 4);  // Checked at compile time
    STATIC_REQUIRE_FALSE(std::is_void_v<int>);
    
    // For traits and template metaprogramming
    STATIC_CHECK(std::is_trivially_copyable_v<Point>);  // v3.0.1+
}
```

#### Conditional Testing
```cpp
TEST_CASE("Conditional blocks", "[Conditional]") {
    int value = get_test_value();
    
    // These record the expression but don't count as test failures (v3.0.1+)
    CHECKED_IF(value > 0) {
        // This block runs if value > 0
        REQUIRE(value <= 100);
    } CHECKED_ELSE(value > 0) {
        // This block runs if value <= 0  
        REQUIRE(value >= -100);
    }
}
```

#### Benchmarking (v2.9.0+)
```cpp
TEST_CASE("Performance testing", "[Benchmark]") {
    // Simple benchmarking
    BENCHMARK("Algorithm performance") {
        return expensive_algorithm();
    };
    
    // Advanced benchmarking with setup
    BENCHMARK_ADVANCED("Advanced benchmark")(Catch::Benchmark::Chronometer meter) {
        std::vector<int> data = setup_test_data();  // Setup not measured
        
        meter.measure([&] { 
            return process_data(data);  // Only this is measured
        });
    };
}
```

## OrcaSlicer-Specific Testing Patterns

### Geometry Testing
```cpp
TEST_CASE("Line operations", "[Geometry]") {
    Line line{{100000, 0}, {0, 0}};
    Line parallel{{200000, 0}, {0, 0}};
    
    REQUIRE(line.parallel_to(line));
    REQUIRE(line.parallel_to(parallel));
    
    // Test with epsilon tolerance
    Line rotated(parallel);
    rotated.rotate(0.9 * EPSILON, {0, 0});
    REQUIRE(line.parallel_to(rotated));
}
```

### Configuration Testing
```cpp
TEST_CASE("Config loading", "[Config]") {
    DynamicPrintConfig config;
    std::string config_path = std::string(TEST_DATA_DIR) + "/test_config/sample.ini";
    
    REQUIRE_NOTHROW(config.load_from_ini(config_path));
    REQUIRE(config.has("layer_height"));
}
```

### File I/O Testing
```cpp
TEST_CASE("STL file parsing", "[FileFormat]") {
    std::string stl_path = std::string(TEST_DATA_DIR) + "/test_stl/20mmbox.stl";
    
    TriangleMesh mesh;
    REQUIRE_NOTHROW(mesh.ReadSTLFile(stl_path.c_str()));
    REQUIRE(!mesh.empty());
    REQUIRE(mesh.volume() > 0);
}
```

### G-code Generation Testing
```cpp
TEST_CASE("G-code writer functionality", "[GCodeWriter]") {
    GCodeWriter writer;
    
    // Load test configuration
    std::string config_path = std::string(TEST_DATA_DIR) + "/fff_print_tests/test_config.ini";
    writer.config.load(config_path, ForwardCompatibilitySubstitutionRule::Disable);
    
    // Test specific G-code generation
    std::string result = writer.lift();
    REQUIRE(!result.empty());
    REQUIRE_THAT(result, Catch::Matchers::ContainsSubstring("G1"));
}
```

### Performance Testing Patterns
```cpp
TEST_CASE("Algorithm performance", "[Performance][Algorithm]") {
    // Large test data
    std::vector<Point> points = generate_large_point_set(10000);
    
    // Time the operation (manual timing for Catch2 v2)
    auto start = std::chrono::high_resolution_clock::now();
    auto result = convex_hull(points);
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    REQUIRE(result.size() > 0);
    REQUIRE(duration.count() < 1000);  // Should complete in < 1 second
}
```

### Custom String Conversions

#### For Custom Types
```cpp
// Method 1: operator<< overload (preferred)
std::ostream& operator<<(std::ostream& os, const Point& point) {
    os << "Point(" << point.x << ", " << point.y << ")";
    return os;
}

// Method 2: StringMaker specialization
namespace Catch {
    template<>
    struct StringMaker<MyCustomType> {
        static std::string convert(const MyCustomType& value) {
            return "MyCustomType{" + std::to_string(value.data) + "}";
        }
    };
}

// Method 3: Enum registration (v2.8.0+)
enum class Status { Ready, Processing, Complete, Error };

// Must be at global scope!
CATCH_REGISTER_ENUM(Status, Status::Ready, Status::Processing, Status::Complete, Status::Error);

// Method 4: Exception translation
CATCH_TRANSLATE_EXCEPTION(MyCustomException const& ex) {
    return "MyCustomException: " + std::string(ex.what());
}

// Method 5: Disable range iteration for problematic types
namespace Catch {
    template<>
    struct is_range<ProblematicType> {
        static const bool value = false;
    };
}
```

## Running and Debugging Tests

### Building Tests
```bash
# Build all tests
cd build && make

# Build specific test suite
cd build && make libslic3r_tests

# Build and run tests
cd build && make && ctest
```

### Running Tests

#### Essential Test Execution Patterns
```bash
# REQUIRED: Random order with assertion warnings (best practice)
cd build && ./tests/libslic3r/libslic3r_tests --order rand --warn NoAssertions

# Run all tests with verbose output via CTest
cd build && ctest --output-on-failure

# Run specific test suite with best practices
cd build && ./tests/libslic3r/libslic3r_tests --order rand --warn NoAssertions

# Filter tests with specific tags
cd build && ./tests/libslic3r/libslic3r_tests "[Geometry]" --order rand

# Filter by test name patterns
cd build && ./tests/libslic3r/libslic3r_tests "*geometry*" --order rand

# Exclude tests (negation)
cd build && ./tests/libslic3r/libslic3r_tests "~[Performance]" --order rand

# Combine filters: (Geometry AND Config) OR Algorithm
cd build && ./tests/libslic3r/libslic3r_tests "[Geometry][Config],[Algorithm]" --order rand

# List available tests, tags, and reporters
cd build && ./tests/libslic3r/libslic3r_tests --list-tests
cd build && ./tests/libslic3r/libslic3r_tests --list-tags  
cd build && ./tests/libslic3r/libslic3r_tests --list-reporters

# Debug failing tests
cd build && ./tests/libslic3r/libslic3r_tests --break    # Break into debugger on failure
cd build && ./tests/libslic3r/libslic3r_tests --success  # Show passing tests too
cd build && ./tests/libslic3r/libslic3r_tests --durations yes  # Show timing info

# Abort on first failure
cd build && ./tests/libslic3r/libslic3r_tests --abort

# Test sharding for parallel execution (MUST share random seed)
cd build && ./tests/libslic3r/libslic3r_tests --order rand --shard-index 0 --shard-count 4 --rng-seed 0xBEEF &
cd build && ./tests/libslic3r/libslic3r_tests --order rand --shard-index 1 --shard-count 4 --rng-seed 0xBEEF &
cd build && ./tests/libslic3r/libslic3r_tests --order rand --shard-index 2 --shard-count 4 --rng-seed 0xBEEF &
cd build && ./tests/libslic3r/libslic3r_tests --order rand --shard-index 3 --shard-count 4 --rng-seed 0xBEEF &
wait  # Wait for all to complete
```

#### Reporter Options for CI Integration
```bash
# Different output formats for CI systems
cd build && ./tests/libslic3r/libslic3r_tests --reporter console     # Default human-readable
cd build && ./tests/libslic3r/libslic3r_tests --reporter compact     # Minimal output
cd build && ./tests/libslic3r/libslic3r_tests --reporter xml         # Catch2 XML format
cd build && ./tests/libslic3r/libslic3r_tests --reporter junit       # JUnit XML (widely supported)
cd build && ./tests/libslic3r/libslic3r_tests --reporter tap         # Test Anything Protocol
cd build && ./tests/libslic3r/libslic3r_tests --reporter teamcity    # TeamCity integration
cd build && ./tests/libslic3r/libslic3r_tests --reporter sonarqube   # SonarQube integration
cd build && ./tests/libslic3r/libslic3r_tests --reporter automake    # Automake integration

# Multiple reporters simultaneously (if supported)
cd build && ./tests/libslic3r/libslic3r_tests --reporter console --reporter junit::out=results.xml
```

### Test Output Control
The custom `VerboseConsoleReporter` provides enhanced output:
- Test case start/end notifications with timing
- Section execution tracking
- Color-coded success/failure indicators
- Duration reporting for performance analysis

## Test Data Management

### Using TEST_DATA_DIR
All test data is accessible via the `TEST_DATA_DIR` preprocessor definition:

```cpp
std::string mesh_path = std::string(TEST_DATA_DIR) + "/20mm_cube.obj";
std::string config_path = std::string(TEST_DATA_DIR) + "/test_config/printer.ini";
```

### Available Test Assets

#### 3D Models
- **Basic shapes**: `20mm_cube.obj`, `pyramid.obj`, `sphere.obj`
- **Complex geometry**: `extruder_idler.obj`, `ipadstand.obj`, `bridge.obj`
- **Edge cases**: `cube_with_hole.obj`, `sloping_hole.obj`, `small_dorito.obj`

#### File Format Tests
- **STL variants**: ASCII/binary, different line endings, Unicode names
- **3MF files**: Multi-material, complex assemblies
- **Configuration files**: Various printer/material profiles

#### Test Utilities
The `Test` namespace provides helper functions:
```cpp
using namespace Slic3r::Test;

// Load standard test meshes
TriangleMesh mesh = mesh(TestMesh::cube_20x20x20);

// Standard test configurations
DynamicPrintConfig config = config(TestConfig::PLA_default);
```

## Common Pitfalls and Solutions

### Floating-Point Comparisons

> **CRITICAL**: Never use Approx - it's deprecated due to asymmetry and other issues

❌ **Incorrect**:
```cpp
REQUIRE(calculated_volume == expected_volume);           // Exact equality
REQUIRE(calculated_volume == Catch::Approx(expected));   // Deprecated! Asymmetric!
```

✅ **Correct**: Always use floating point matchers
```cpp
// Absolute tolerance - good when values are near zero
REQUIRE_THAT(calculated_volume, WithinAbs(expected_volume, 0.001));

// Relative tolerance - good for values with different magnitudes
REQUIRE_THAT(calculated_volume, WithinRel(expected_volume, 0.01));  // 1% tolerance

// ULP (Units in Last Place) - most precise, requires IEEE-754
REQUIRE_THAT(calculated_volume, WithinULP(expected_volume, 4));

// Combined approach - relative OR absolute
REQUIRE_THAT(calculated_volume, 
    WithinRel(expected_volume, 0.001) || WithinAbs(0.0, 0.000001));

// Precision control for output
Catch::StringMaker<double>::precision = 15;  // Show more decimal places
```

### Why Approx is Problematic:
- **Asymmetric**: `Approx(10).epsilon(0.1) != 11.1` but `Approx(11.1).epsilon(0.1) == 10`
- **Double-only**: All computation done in `double`, causes issues with `float` inputs  
- **Default behavior**: Only uses relative comparison, so `Approx(0) == X` only works for `X == 0`

### Path Handling
❌ **Incorrect**:
```cpp
std::string path = TEST_DATA_DIR + "/model.obj";  // May have path separator issues
```

✅ **Correct**:
```cpp
std::string path = std::string(TEST_DATA_DIR) + "/model.obj";
// or use boost::filesystem for complex path operations
```

### Exception Testing
❌ **Incorrect**:
```cpp
bool threw_exception = false;
try {
    risky_function();
} catch (...) {
    threw_exception = true;
}
REQUIRE(threw_exception);
```

✅ **Correct**:
```cpp
REQUIRE_THROWS(risky_function());
// or for specific exceptions
REQUIRE_THROWS_AS(risky_function(), SpecificException);
```

### Thread Safety

⚠️ **CRITICAL**: Catch2 assertions are **NOT thread-safe** by default!

> **Note**: Catch2 v3.9.0+ has opt-in thread-safe assertions via `CATCH_CONFIG_EXPERIMENTAL_THREAD_SAFE_ASSERTIONS`, but OrcaSlicer uses v2

❌ **Incorrect**: Will cause undefined behavior or crashes
```cpp
std::thread t([&]() {
    REQUIRE(threaded_operation() == expected);  // NOT THREAD-SAFE!
    CHECK(other_operation());                   // NOT THREAD-SAFE!
});
```

✅ **Correct**: Collect results, assert on main thread
```cpp
std::atomic<bool> success{false};
std::atomic<int> error_count{0};

std::thread t([&]() {
    // Do work in thread, collect results
    bool result1 = (threaded_operation() == expected);
    bool result2 = other_operation();
    
    if (result1 && result2) {
        success = true;
    } else {
        error_count++;
    }
});

t.join();

// Assert results on main thread
REQUIRE(success);
REQUIRE(error_count == 0);
```

#### Thread Safety Rules:
- **REQUIRE family**: Would terminate process in spawned threads (throws exception with no try-catch)
- **CHECK family**: Not thread-safe, can corrupt internal state
- **SKIP, FAIL, SUCCEED**: Not thread-safe even with v3 thread-safe assertions
- **Message macros**: INFO, CAPTURE, WARN - not thread-safe
- **STATIC_REQUIRE/CHECK**: Not thread-safe (relies on runtime registration)

### Memory Management
Use RAII and smart pointers in tests:
```cpp
TEST_CASE("Resource management", "[Memory]") {
    auto model = std::make_unique<Model>();
    // Automatic cleanup on test completion/failure
    
    REQUIRE(model->objects.empty());
}
```

## Performance Considerations

### Compilation Optimizations
```cpp
// In CMakeLists.txt or as preprocessor definition
#define CATCH_CONFIG_FAST_COMPILE  // 20% faster compilation, disables some features

// For faster test iteration during development
#define CATCH_CONFIG_DISABLE_STRINGIFICATION  // Workaround for VS2017 raw string bug
```

### Runtime Performance
```cpp
TEST_CASE("Performance-sensitive test", "[Performance]") {
    // Manual timing for Catch2 v2 (v3 has built-in benchmarking)
    auto start = std::chrono::high_resolution_clock::now();
    
    auto result = expensive_operation();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    REQUIRE(result.is_valid());
    REQUIRE(duration.count() < 1000);  // Should complete in < 1 second
    
    INFO("Operation took " << duration.count() << "ms");
}
```

### Memory Leak Detection
```cpp
// For Windows builds - detects memory leaks
#define CATCH_CONFIG_WINDOWS_CRTDBG  // Must be defined for whole build
```

## Integration with CMake

### Adding New Test Files
1. Create test file: `test_new_feature.cpp`
2. Add to appropriate `CMakeLists.txt`:
```cmake
add_executable(${_TEST_NAME}_tests 
    ${_TEST_NAME}_tests.cpp
    test_existing_feature.cpp
    test_new_feature.cpp  # Add here
)
```

### Advanced Test Discovery
```cmake
# Basic test discovery
catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: ")

# Advanced test discovery with customization
catch_discover_tests(${_TEST_NAME}_tests
    TEST_PREFIX "${_TEST_NAME}: "
    TEST_SUFFIX " (auto)"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    EXTRA_ARGS --order rand --warn NoAssertions
    PROPERTIES 
        TIMEOUT 300
        LABELS "unit;core"
    DISCOVERY_MODE PRE_TEST  # or POST_BUILD
    REPORTER junit
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    OUTPUT_PREFIX "results_"
    OUTPUT_SUFFIX ".xml"
)

# Test sharding for parallel execution
include(CatchShardTests)  # If available
catch_shard_tests(${_TEST_NAME}_tests
    SHARD_COUNT 4
    TEST_PREFIX "${_TEST_NAME}_shard: "
)
```

### Conditional Test Compilation
```cmake
# Feature-dependent tests
if (TARGET OpenVDB::openvdb)
    target_sources(${_TEST_NAME}_tests PRIVATE test_hollowing.cpp)
endif()

# Platform-specific tests
if(WIN32)
    target_sources(${_TEST_NAME}_tests PRIVATE test_windows_specific.cpp)
elseif(UNIX)
    target_sources(${_TEST_NAME}_tests PRIVATE test_unix_specific.cpp)
endif()

# Compiler-specific workarounds
if(MSVC)
    target_compile_definitions(${_TEST_NAME}_tests PRIVATE CATCH_CONFIG_DISABLE_STRINGIFICATION)
endif()

# Fast compile mode for development
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(${_TEST_NAME}_tests PRIVATE CATCH_CONFIG_FAST_COMPILE)
endif()
```

## Known Issues and Workarounds

### Platform-Specific Issues
```cpp
// MinGW/CygWin slow linking workaround
// Use: -fuse-ld=lld flag to speed up linking significantly

// Visual Studio 2017 raw string literal bug
#define CATCH_CONFIG_DISABLE_STRINGIFICATION
// This disables expression stringification but works around the compiler bug

// Visual Studio 2022 spaceship operator issue
// REQUIRE((a <=> b) == 0);  // May not compile with MSVC
// Workaround: use clang-cl or avoid spaceship in assertions

// QNX/VxWorks C stdlib issues
#include <cfoo>           // Use C++ headers
std::foo_function();     // Always call qualified
// NOT: #include <foo.h> and foo_function();
```

### Catch2 Version-Specific Limitations
```cpp
// OrcaSlicer uses Catch2 v2 - these features are NOT available:
// SKIP() macro                          - Available in v3.3.0+
// Thread-safe assertions                - Available in v3.9.0+  
// BENCHMARK improvements                 - Many in v3.x
// testCasePartial events                - Available in v3.0.1+
// Multiple reporters                    - Available in v3.0.1+
// STATIC_CHECK macro                    - Available in v3.0.1+

// v2 Limitations to remember:
// - Sections can be re-run if last section fails
// - String matcher is "Contains" not "ContainsSubstring"
// - Limited benchmarking support compared to v3
// - No test sharding built-in
```

### Test Organization Best Practices

#### Project Structure Rules
1. **1:1 correspondence**: One test binary per library/module
2. **Hidden tests**: Use `[.]` or `[!benchmark]` tags for tests that shouldn't run by default
3. **Tag hierarchy**: Use consistent tagging scheme across the project
4. **File naming**: Follow `test_<feature>.cpp` pattern

#### CI/CD Integration
```bash
# Essential CI test command
./tests --order rand --warn NoAssertions --reporter junit::out=results.xml

# For coverage analysis
./tests --order rand --warn NoAssertions --reporter console --success

# For performance tracking
./tests --order rand --warn NoAssertions --durations yes
```

This comprehensive guide ensures robust, maintainable, and efficient testing practices for OrcaSlicer development with Claude Code, incorporating all critical knowledge from the official Catch2 documentation.