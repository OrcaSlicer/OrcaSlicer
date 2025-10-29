# How to Test

This wiki page describes how to build and run tests on Linux. It should eventually provide guidance on how to add tests for a new feature.

## Build Tests

Can be built when you are building Orca Slicer binary by including the `-t` flag for `build_linux.sh`:

```
build_linux.sh -st
```

Test binaries will then appear under `build/tests`. As of this writing, not all tests will be built.

## Run Unit Tests

### Run All

```
ctest --test-dir build/tests
```

### Run a Specific Set

```
ctest --test-dir build/tests/slic3rutils
```
