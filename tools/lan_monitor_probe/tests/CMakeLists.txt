cmake_minimum_required(VERSION 3.16)
project(LanCameraProbeFixture LANGUAGES CXX)

add_library(fake_camera_sdk SHARED fake_camera_sdk.cpp)
target_compile_features(fake_camera_sdk PRIVATE cxx_std_17)
target_compile_definitions(fake_camera_sdk PRIVATE BAMBU_EXPORTS)
target_include_directories(fake_camera_sdk PRIVATE "${ORCA_SOURCE_ROOT}/src")
add_subdirectory("${ORCA_SOURCE_ROOT}/tools/lan_monitor_probe" probes)
