# Catch2 reference

How to write and structure test code with Catch2 in OrcaSlicer. For where a test belongs, how to name and tag it, and how to build and run the suites, see [AGENTS.md](AGENTS.md).

OrcaSlicer uses **Catch2 v3.11.0**, vendored in `tests/catch2/`. Include it with the single-header convenience include:

```cpp
#include <catch2/catch_all.hpp>
```

## Critical rules

These three mistakes produce undefined behavior, crashes, or useless failure output rather than a normal test failure. Avoid them everywhere.

### 1. Never reuse a section name inside a loop

A repeated `SECTION` name in a loop makes Catch2's section tracking behave unpredictably. Use `DYNAMIC_SECTION` so each iteration is unique.

```cpp
// WRONG: same name every iteration
for (int i = 0; i < 3; ++i)
    SECTION("Same name") { REQUIRE(i >= 0); }

// CORRECT
for (int i = 0; i < 3; ++i)
    DYNAMIC_SECTION("Section " << i) { REQUIRE(i >= 0); }
```

### 2. Assertions are not thread-safe

Catch2 assertions are not thread-safe by default. A `REQUIRE`/`CHECK` from a spawned thread corrupts internal state or terminates the process. Collect results in the thread, assert on the main thread.

```cpp
// WRONG
std::thread t([&]{ REQUIRE(work() == expected); });

// CORRECT
std::atomic<int> passed{0};
std::thread t([&]{ if (work() == expected) passed++; });
t.join();
REQUIRE(passed == 1);
```

> Catch2 v3.9.0+ has opt-in thread-safe assertions via `CATCH_CONFIG_EXPERIMENTAL_THREAD_SAFE_ASSERTIONS`. OrcaSlicer does not enable that flag, so assertions remain non-thread-safe. See [Thread safety](#thread-safety) below for the full rule list.

### 3. Do not combine conditions with binary operators

Catch2 decomposes a single comparison to show both operands on failure. A `&&`/`||` inside one assertion collapses to `false` with no values. Split it.

```cpp
REQUIRE(a > 0 && b < 10);   // WRONG: prints "false"
REQUIRE(a > 0);             // CORRECT: each prints its operands
REQUIRE(b < 10);
```

## Test structure

```cpp
#include <catch2/catch_all.hpp>
#include "libslic3r/Point.hpp"

using namespace Slic3r;

TEST_CASE("Behavioral description", "[SubsystemTag]") {
    // ...
}
```

## Assertions

```cpp
// Stop the test on failure
REQUIRE(expression);
REQUIRE_FALSE(expression);

// Continue the test after failure (report all failures in the case)
CHECK(expression);
CHECK_FALSE(expression);

// Record the result without failing (for assumptions that may be violated)
CHECK_NOFAIL(expression);
```

### Exceptions

```cpp
REQUIRE_NOTHROW(function_call());
REQUIRE_THROWS(risky_function());
REQUIRE_THROWS_AS(function_call(), SpecificException);
REQUIRE_THROWS_WITH(function_call(), "Expected error message");
REQUIRE_THROWS_MATCHES(function_call(), SpecificException,
                       Catch::Matchers::Message("contains this"));
```

Prefer these over a hand-rolled `try`/`catch` with a bool flag.

## Matchers

```cpp
#include <catch2/matchers/catch_matchers.hpp>

// String matchers
using Catch::Matchers::StartsWith;
using Catch::Matchers::EndsWith;
using Catch::Matchers::ContainsSubstring;   // v2's "Contains" no longer exists
using Catch::Matchers::Equals;
using Catch::Matchers::Matches;             // regex

REQUIRE_THAT(result, StartsWith("Expected prefix"));
REQUIRE_THAT(result, ContainsSubstring("middle part"));
REQUIRE_THAT(result, Matches(".*pattern.*"));

// Float matchers - always prefer these over Approx
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

REQUIRE_THAT(v, WithinAbs(expected, 0.001));
REQUIRE_THAT(v, WithinRel(expected, 0.01));
REQUIRE_THAT(v, WithinULP(expected, 4));

// Combine: relative OR absolute (useful when the value can be near zero)
REQUIRE_THAT(v, WithinRel(expected, 0.001) || WithinAbs(0.0, 0.000001));
```

## Sections

Each `SECTION` re-runs the enclosing `TEST_CASE` body from the top, so setup declared before the sections is fresh for each one.

```cpp
TEST_CASE("Complex feature", "[Feature]") {
    SomeObject obj;   // rebuilt for every section

    SECTION("First scenario")  { REQUIRE(obj.method1() == expected_value); }
    SECTION("Second scenario") { REQUIRE(obj.method2() == other_expected); }
}
```

## BDD-style tests

`SCENARIO` / `GIVEN` / `WHEN` / `THEN` are aliases for `TEST_CASE` and `SECTION` with prefixed names. New tests should prefer a flat `TEST_CASE`; reserve BDD for genuine shared setup that branches into closely related variations (see the test-design guidance in [AGENTS.md](AGENTS.md)).

```cpp
SCENARIO("User performs an operation", "[UserStory]") {
    GIVEN("A setup condition") {
        GCodeWriter writer;
        WHEN("The user acts") {
            auto result = writer.some_operation();
            THEN("The outcome holds") {
                REQUIRE(result.size() > 0);
            }
        }
    }
}
```

## Generators

```cpp
// Value list
auto v = GENERATE(1, 3, 5, 7, 11, 13);

// Range
auto i = GENERATE(range(1, 10));   // 1..9

// From a variable (use GENERATE_REF / GENERATE_COPY for captured references)
std::vector<int> values = {1, 2, 3, 4, 5};
auto x = GENERATE_REF(from_range(values));

// Random
auto r = GENERATE(take(100, random(-1000, 1000)));
```

## Fixtures

```cpp
class GeometryFixture {
public:
    Point origin{0, 0};
    Point unit_x{1, 0};
};

TEST_CASE_METHOD(GeometryFixture, "Point operations", "[Geometry]") {
    REQUIRE(origin.distance_to(unit_x) == 1.0);
}
```

Persistent (`TEST_CASE_PERSISTENT_FIXTURE`, one instance for the whole case) and type-parameterized (`TEMPLATE_TEST_CASE_METHOD`) variants also exist; neither is used in the suite today.

## Advanced features

### Logging and control

```cpp
INFO("Persists until end of scope");
UNSCOPED_INFO("Survives beyond its scope");   // v2.7.0+
CAPTURE(some_variable, another_var);          // logs names and values

WARN("Warns without failing");
SKIP("Reason");        // marks the test skipped (v3.3.0+)
FAIL("Stops the test");
SUCCEED("Explicit success marker");
```

### Other macros

Available but currently unused in the suite; see the upstream docs for details.

- **Compile-time asserts**: `STATIC_REQUIRE` / `STATIC_CHECK` (v3.0.1+) check type traits at compile time.
- **Conditional blocks**: `CHECKED_IF` / `CHECKED_ELSE` record a branch condition without counting it as a failure.
- **Benchmarking** (v2.9.0+): `BENCHMARK("name") { return work(); };`, or `BENCHMARK_ADVANCED` when setup must be excluded from the measurement.

## Usage patterns in OrcaSlicer

Concrete shapes for exercising the codebase's own types. Test data is reached through the `TEST_DATA_DIR` define; always wrap it in `std::string(...)` before concatenating a path.

```cpp
// Geometry, with epsilon tolerance
TEST_CASE("Line operations", "[Geometry]") {
    Line line{{100000, 0}, {0, 0}};
    Line rotated(line);
    rotated.rotate(0.9 * EPSILON, {0, 0});
    REQUIRE(line.parallel_to(rotated));
}

// Config from an ini
TEST_CASE("Config loading", "[Config]") {
    DynamicPrintConfig config;
    REQUIRE_NOTHROW(config.load_from_ini(std::string(TEST_DATA_DIR) + "/test_config/sample.ini",
                                         ForwardCompatibilitySubstitutionRule::Disable));
    REQUIRE(config.has("layer_height"));
}

// File I/O
TEST_CASE("STL file parsing", "[FileFormat]") {
    TriangleMesh mesh;
    REQUIRE_NOTHROW(mesh.ReadSTLFile((std::string(TEST_DATA_DIR) + "/test_stl/20mmbox.stl").c_str()));
    REQUIRE_FALSE(mesh.empty());
    REQUIRE(mesh.volume() > 0);
}

// G-code emission, matched by token (see test_gcodewriter.cpp)
TEST_CASE("z_hop lifts the nozzle", "[GCodeWriter]") {
    GCodeWriter writer;
    writer.set_extruders({0});
    writer.set_extruder(0);
    writer.travel_to_z(10.0);
    writer.config.z_hop.values = {1.0};
    REQUIRE_THAT(writer.eager_lift(LiftType::NormalLift), Catch::Matchers::ContainsSubstring("Z11"));
}
```

### Custom string conversions

Give Catch2 a way to print a custom type on failure. The usual case is an `operator<<` overload:

```cpp
std::ostream& operator<<(std::ostream& os, const Point& p) {
    return os << "Point(" << p.x << ", " << p.y << ")";
}
```

When you cannot add `operator<<`, specialize `Catch::StringMaker<T>`. Enums can be registered with `CATCH_REGISTER_ENUM` (at global scope) and exceptions translated with `CATCH_TRANSLATE_EXCEPTION`; see the upstream docs for those.

## Command line

[AGENTS.md](AGENTS.md) covers the everyday commands (CTest, per-suite runs, tag filtering as CTest labels). The flags below are Catch2's own, available when you run a suite executable directly.

```bash
# Filtering
suite_tests "[Geometry]"                    # by tag
suite_tests "*geometry*"                    # by name pattern
suite_tests "~[Performance]"                # exclude a tag
suite_tests "[Geometry][Config],[Algorithm]"  # (Geometry AND Config) OR Algorithm

# Discovery
suite_tests --list-tests
suite_tests --list-tags
suite_tests --list-reporters

# Debugging a failure
suite_tests --break         # break into the debugger on failure
suite_tests --success       # show passing assertions too
suite_tests --durations yes # per-test timing
suite_tests --abort         # stop at the first failure
```

### Ordering and sharding

Run in random order so tests stay independent. For parallel shards, all shards must share one seed.

```bash
suite_tests --order rand --warn NoAssertions

suite_tests --order rand --shard-index 0 --shard-count 4 --rng-seed 0xBEEF
suite_tests --order rand --shard-index 1 --shard-count 4 --rng-seed 0xBEEF
# ...one invocation per shard index
```

### Reporters

```bash
suite_tests --reporter console    # default, human-readable
suite_tests --reporter compact
suite_tests --reporter xml        # Catch2 XML
suite_tests --reporter junit      # JUnit XML (CI)
suite_tests --reporter tap
suite_tests --reporter console --reporter junit::out=results.xml   # multiple at once
```

## Common pitfalls

### Floating-point comparison

Compare floats with the float matchers, never with `==`. New tests should prefer the `Within*` matchers over `Approx`. Many existing tests still use `Approx`, which works but is:

- **Asymmetric**: `Approx(10).epsilon(0.1) != 11.1` yet `Approx(11.1).epsilon(0.1) == 10`.
- **Double-only**: all math is done in `double`, which misbehaves for `float` inputs.
- **Relative by default**: `Approx(0) == X` holds only for `X == 0`.

Use `WithinAbs` near zero, `WithinRel` across magnitudes, `WithinULP` for the tightest check, or combine them. `Catch::StringMaker<double>::precision = 15;` widens printed precision.

### Exception testing

Use `REQUIRE_THROWS` / `REQUIRE_THROWS_AS` rather than a `try`/`catch` with a bool flag.

### Thread safety

Assertions are not thread-safe (see [Critical rule 2](#2-assertions-are-not-thread-safe)). The full list of macros that must stay on the main thread:

- **`REQUIRE` family**: throws in a spawned thread with no handler, terminating the process.
- **`CHECK` family**: can corrupt internal state.
- **`SKIP`, `FAIL`, `SUCCEED`**: unsafe even with v3's opt-in thread-safe assertions.
- **Message macros** (`INFO`, `CAPTURE`, `WARN`): unsafe.
- **`STATIC_REQUIRE` / `STATIC_CHECK`**: unsafe (rely on runtime registration).

### Path handling

Wrap `TEST_DATA_DIR` in `std::string(...)` before concatenating, or use `boost::filesystem`:

```cpp
std::string path = std::string(TEST_DATA_DIR) + "/model.obj";
```

### Memory

Prefer RAII and smart pointers so a failing assertion cleans up automatically.

## Compilation and performance flags

```cpp
#define CATCH_CONFIG_FAST_COMPILE             // ~20% faster compile, disables some features
#define CATCH_CONFIG_DISABLE_STRINGIFICATION  // works around the VS2017 raw-string bug
#define CATCH_CONFIG_WINDOWS_CRTDBG           // memory-leak detection (whole build)
```

The test build already defines `CATCH_CONFIG_FAST_COMPILE` (via `test_common` in `tests/CMakeLists.txt`).

## Platform-specific workarounds

- **MinGW/Cygwin** slow linking: build with `-fuse-ld=lld`.
- **Visual Studio 2017** raw-string-literal bug: define `CATCH_CONFIG_DISABLE_STRINGIFICATION` (disables expression stringification).
- **Visual Studio 2022** spaceship operator: `REQUIRE((a <=> b) == 0)` may not compile; use clang-cl or avoid `<=>` in assertions.

## Catch2 v3 notes

Available on v3.11.0: `SKIP()` (v3.3.0+), opt-in thread-safe assertions (v3.9.0+, not enabled here), built-in `BENCHMARK`, multiple simultaneous reporters (v3.0.1+), `STATIC_CHECK` (v3.0.1+), built-in sharding (`--shard-*`).

Two behavior notes: the string matcher is `ContainsSubstring` (v2's `Contains` is gone), and a section is re-run when a later sibling section fails (unchanged from v2).
