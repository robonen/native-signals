cmake_minimum_required(VERSION 3.28)

project(native-signals
    VERSION 0.1.0
    DESCRIPTION "Reactive programming suite for modern C++"
    LANGUAGES CXX)

include(CMakeDependentOption)

set(NSIG_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(NSIG_IS_TOP_LEVEL ON)
endif()

option(NSIG_BUILD_TESTS "Build the test suite" ${NSIG_IS_TOP_LEVEL})
option(NSIG_BUILD_BENCH "Build the benchmarks" ${NSIG_IS_TOP_LEVEL})
option(NSIG_BUILD_MODULE "Build the C++20 module interface (needs a module-capable toolchain)" OFF)

# ---------------------------------------------------------------------------
# The library: header-only, consumed as native-signals::native-signals
# ---------------------------------------------------------------------------

add_library(native-signals INTERFACE)
add_library(native-signals::native-signals ALIAS native-signals)

target_include_directories(native-signals INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(native-signals INTERFACE cxx_std_23)

# ---------------------------------------------------------------------------
# Optional: `import nsig;`
# ---------------------------------------------------------------------------

if(NSIG_BUILD_MODULE)
    add_library(native-signals-module STATIC)
    add_library(native-signals::module ALIAS native-signals-module)
    target_sources(native-signals-module
        PUBLIC FILE_SET CXX_MODULES FILES src/nsig.cppm)
    target_link_libraries(native-signals-module PUBLIC native-signals)
    target_compile_features(native-signals-module PUBLIC cxx_std_23)
    set_target_properties(native-signals-module PROPERTIES CXX_SCAN_FOR_MODULES ON)

    add_executable(nsig_module_demo examples/module_demo.cpp)
    target_link_libraries(nsig_module_demo PRIVATE native-signals-module)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(NSIG_BUILD_TESTS)
    enable_testing()

    add_executable(nsig_conformance tests/conformance.cpp)
    target_link_libraries(nsig_conformance PRIVATE native-signals)
    add_test(NAME conformance COMMAND nsig_conformance)

    add_executable(nsig_static_graph tests/static_graph.cpp)
    target_link_libraries(nsig_static_graph PRIVATE native-signals)
    add_test(NAME static_graph COMMAND nsig_static_graph)

    add_executable(nsig_adapt tests/adapt.cpp)
    target_link_libraries(nsig_adapt PRIVATE native-signals)
    add_test(NAME adapt COMMAND nsig_adapt)

    add_executable(nsig_expr tests/expr.cpp)
    target_link_libraries(nsig_expr PRIVATE native-signals)
    add_test(NAME expr COMMAND nsig_expr)

    add_executable(nsig_async tests/async.cpp)
    target_link_libraries(nsig_async PRIVATE native-signals)
    add_test(NAME async COMMAND nsig_async)

    add_executable(nsig_single_header tests/single_header.cpp)
    target_link_libraries(nsig_single_header PRIVATE native-signals)
    target_include_directories(nsig_single_header PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/single_include)
    add_test(NAME single_header COMMAND nsig_single_header)

    foreach(t nsig_conformance nsig_async nsig_static_graph nsig_expr nsig_adapt nsig_single_header)
        target_compile_options(${t} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(NSIG_IS_TOP_LEVEL)
    enable_testing()
    add_executable(nsig_todo examples/todo.cpp)
    target_link_libraries(nsig_todo PRIVATE native-signals)

    # The ESP32 example builds as a host simulation here: same reactive graph,
    # stubbed peripherals, and the embedded build flags (-fno-exceptions,
    # single global runtime) so those stay tested.
    add_executable(nsig_controller_io examples/controller_io.cpp)
    target_link_libraries(nsig_controller_io PRIVATE native-signals)
    target_compile_definitions(nsig_controller_io PRIVATE NSIG_THREAD_LOCAL=)
    target_compile_options(nsig_controller_io PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Os>)
    add_test(NAME controller_io COMMAND nsig_controller_io)

    add_executable(nsig_esp32_fan examples/esp32_fan_controller.cpp)
    target_link_libraries(nsig_esp32_fan PRIVATE native-signals)
    target_compile_definitions(nsig_esp32_fan PRIVATE NSIG_THREAD_LOCAL=)
    target_compile_options(nsig_esp32_fan PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Os -fno-exceptions -fno-rtti>)
    add_test(NAME esp32_fan_sim COMMAND nsig_esp32_fan)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(NSIG_BUILD_BENCH)
    add_executable(nsig_bench bench/propagate.cpp)
    target_link_libraries(nsig_bench PRIVATE native-signals)
    target_compile_options(nsig_bench PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-O2>)
    target_compile_definitions(nsig_bench PRIVATE NDEBUG)

    # Reactive graph vs. a hand-written controller, same outputs asserted.
    add_executable(nsig_bench_embedded bench/embedded.cpp)
    target_link_libraries(nsig_bench_embedded PRIVATE native-signals)
    target_compile_options(nsig_bench_embedded PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-O2>)
    target_compile_definitions(nsig_bench_embedded PRIVATE NDEBUG NSIG_THREAD_LOCAL=)
    if(NSIG_BUILD_TESTS)
        add_test(NAME reactive_matches_handwritten COMMAND nsig_bench_embedded --verify)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS native-signals EXPORT native-signals-targets)
install(DIRECTORY include/nsig DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT native-signals-targets
    FILE native-signals-targets.cmake
    NAMESPACE native-signals::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/native-signals)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/native-signals-config-version.cmake"
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)
