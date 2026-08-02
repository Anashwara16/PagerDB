# ==============================================================================
# KVStore - A Learning B+ Tree Key-Value Store
# ==============================================================================
#
# This CMakeLists.txt configures the build system for our key-value store.
# We use CMake because it's the de-facto standard for C++ projects and handles
# cross-platform compilation, dependency management, and testing integration.
#
# To build this project:
#   mkdir build && cd build
#   cmake ..
#   make
#
# To run tests:
#   make test
#   # or for verbose output:
#   ctest --output-on-failure
#
# ==============================================================================

# 3.21+ needed for CMakePresets.json schema version 3 (see CMakePresets.json)
cmake_minimum_required(VERSION 3.21)

# ------------------------------------------------------------------------------
# Project Definition
# ------------------------------------------------------------------------------
# VERSION: Semantic versioning (major.minor.patch)
# LANGUAGES: We only need C++ for this project
project(kvstore
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "A learning B+ tree key-value store implementation"
)

# ------------------------------------------------------------------------------
# C++ Standard Configuration
# ------------------------------------------------------------------------------
# We require C++23 for:
#   - std::expected (modern error handling)
#   - std::span (safe array views)
#   - std::string_view improvements
#   - Concepts (for cleaner template constraints, optional)
#   - std::shared_mutex (C++17, but better support in C++20)
#   - Designated initializers
#   - Three-way comparison operator (<=>)
#
# CMAKE_CXX_STANDARD_REQUIRED ensures the build fails if C++23 isn't available
# CMAKE_CXX_EXTENSIONS=OFF disables compiler-specific extensions (like GNU++)
# for better portability

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use libc++ with Clang for full C++23 support (std::expected, deducing this)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()

# ------------------------------------------------------------------------------
# Compiler Warnings
# ------------------------------------------------------------------------------
# Good warnings catch bugs early. These flags are for GCC and Clang:
#
#   -Wall          : Enable most warnings
#   -Wextra        : Enable extra warnings
#   -Wpedantic     : Strict ISO C++ compliance
#   -Werror        : Treat warnings as errors (comment out while learning)
#   -Wshadow       : Warn when a variable shadows another
#   -Wconversion   : Warn on implicit type conversions that may lose data
#   -Wnon-virtual-dtor : Warn if a class with virtual functions has a
#                        non-virtual destructor

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    # -Werror  # Uncomment once code is stable
    -Wshadow
    -Wconversion
    -Wnon-virtual-dtor
)

# ------------------------------------------------------------------------------
# Build Types
# ------------------------------------------------------------------------------
# CMAKE_BUILD_TYPE controls optimization and debug info:
#   Debug       : No optimization, full debug info (-O0 -g)
#   Release     : Full optimization, no debug (-O3 -DNDEBUG)
#   RelWithDebInfo : Optimization with debug info (-O2 -g -DNDEBUG)
#
# Default to Debug for development

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ------------------------------------------------------------------------------
# Sanitizers
# ------------------------------------------------------------------------------
# KVSTORE_SANITIZER selects compiler sanitizers, e.g.:
#   -DKVSTORE_SANITIZER=address,undefined   (the 'asan' preset)
#   -DKVSTORE_SANITIZER=thread              (the 'tsan' preset)
# Empty (default) means no sanitizer. Applied globally so the library, tests,
# and fetched dependencies are all instrumented consistently.

set(KVSTORE_SANITIZER "" CACHE STRING "Sanitizer list for -fsanitize= (empty = none)")

if(KVSTORE_SANITIZER)
    message(STATUS "Sanitizers enabled: ${KVSTORE_SANITIZER}")
    add_compile_options(-fsanitize=${KVSTORE_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${KVSTORE_SANITIZER})
endif()

# ------------------------------------------------------------------------------
# Include Directories
# ------------------------------------------------------------------------------
# This tells the compiler where to find header files when you write:
#   #include "kvstore/storage/file.h"
#
# The PUBLIC keyword means any target linking against kvstore will also
# get this include path.

include_directories(${CMAKE_SOURCE_DIR}/include)

# ------------------------------------------------------------------------------
# Source Files
# ------------------------------------------------------------------------------
# We list source files explicitly rather than using GLOB because:
#   1. CMake re-runs automatically when CMakeLists.txt changes
#   2. GLOBbing can miss new files until you manually re-run cmake
#   3. Explicit lists document what's in the project
#
# As you implement more modules, add their .cc files here.

set(KVSTORE_SOURCES
    src/storage/file.cc
    src/storage/page.cc
    src/storage/pager.cc
    src/storage/buffer_pool.cc
    src/btree/node.cc
    src/btree/btree.cc
    src/btree/cursor.cc
    src/wal/log_record.cc
    src/wal/log_writer.cc
    src/wal/log_reader.cc
    src/txn/lock_manager.cc
    src/db.cc
    # src/storage/buffer_pool.cc
    # src/btree/btree.cc
    # src/btree/node.cc
    # src/wal/log_writer.cc
    # src/wal/log_reader.cc
    # src/db.cc
)

# ------------------------------------------------------------------------------
# Library Target
# ------------------------------------------------------------------------------
# We create a STATIC library (linked directly into executables).
# Alternatives:
#   SHARED : Dynamic library (.so on Linux)
#   OBJECT : Just compiled objects, not archived
#
# STATIC is simpler for learning projects.

add_library(kvstore_lib STATIC ${KVSTORE_SOURCES})

# Make headers available to users of this library
target_include_directories(kvstore_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Link pthread for std::thread/mutex/etc.
# On Linux, threading requires explicit linking to pthread
find_package(Threads REQUIRED)
target_link_libraries(kvstore_lib PUBLIC Threads::Threads)

# ------------------------------------------------------------------------------
# Testing with GoogleTest (Optional)
# ------------------------------------------------------------------------------
# GoogleTest (gtest) is the most popular C++ testing framework.
# Set KVSTORE_BUILD_TESTS=ON to enable tests.
#
# To build with tests:
#   cmake -DKVSTORE_BUILD_TESTS=ON ..
#
# If GoogleTest isn't available locally, FetchContent will download it.

option(KVSTORE_BUILD_TESTS "Build the test suite" OFF)

if(KVSTORE_BUILD_TESTS)
    message(STATUS "Building tests is enabled")
    
    # Try to find GoogleTest locally first
    find_package(GTest QUIET)
    
    if(NOT GTest_FOUND)
        message(STATUS "GoogleTest not found locally, fetching from GitHub...")
        include(FetchContent)
        
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0  # Use a specific release for reproducibility
        )
        
        # Prevent gtest from overriding our compiler/linker settings
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        
        FetchContent_MakeAvailable(googletest)
    endif()
    
    # Enable CTest integration (allows 'make test' and 'ctest')
    enable_testing()
    
    # --------------------------------------------------------------------------
    # Test Executables
    # --------------------------------------------------------------------------
    # Each test file becomes its own executable. This is cleaner than one big
    # test binary because:
    #   1. Failures are easier to isolate
    #   2. Tests can run in parallel
    #   3. You can run individual test executables directly
    #
    # To add a test: create tests/<name>.cc and append <name> here.

    set(KVSTORE_TESTS
        types_test
        file_test
        page_test
        pager_test
        buffer_pool_test
        buffer_pool_concurrent_test
        wal_test
        recovery_test
        crash_test
        node_test
        btree_test
        cursor_test
        btree_property_test
        btree_concurrent_test
        db_test
        db_concurrent_test
        txn_test
    )

    foreach(test_name IN LISTS KVSTORE_TESTS)
        add_executable(${test_name} tests/${test_name}.cc)
        target_link_libraries(${test_name} kvstore_lib GTest::gtest_main)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
else()
    message(STATUS "Building tests is disabled. Use -DKVSTORE_BUILD_TESTS=ON to enable.")
endif()

# ------------------------------------------------------------------------------
# Benchmarks with Google Benchmark (Optional, Phase 6)
# ------------------------------------------------------------------------------
# Set KVSTORE_BUILD_BENCH=ON (the 'bench' preset: Release + benchmarks) to
# build bench/db_bench and, when a system sqlite3 is available, the
# bench/sqlite_bench baseline. One-command run: tools/run_bench.sh.

option(KVSTORE_BUILD_BENCH "Build the benchmark suite" OFF)

if(KVSTORE_BUILD_BENCH)
    message(STATUS "Building benchmarks is enabled")

    find_package(benchmark QUIET)

    if(NOT benchmark_FOUND)
        message(STATUS "Google Benchmark not found locally, fetching from GitHub...")
        include(FetchContent)

        # Don't build benchmark's own tests, and don't let its -Werror trip
        # on our globally applied warning flags.
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

        FetchContent_Declare(
            googlebenchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG        v1.9.1  # Pinned for reproducibility
        )
        FetchContent_MakeAvailable(googlebenchmark)
    endif()

    add_executable(db_bench bench/db_bench.cc)
    target_link_libraries(db_bench kvstore_lib benchmark::benchmark)

    # SQLite baseline (docs/BENCHMARKS.md). Uses the system library; the
    # target simply doesn't exist where sqlite3 isn't installed.
    find_library(KVSTORE_SQLITE3_LIB sqlite3)
    find_path(KVSTORE_SQLITE3_INCLUDE sqlite3.h)
    if(KVSTORE_SQLITE3_LIB AND KVSTORE_SQLITE3_INCLUDE)
        message(STATUS "SQLite3 found: ${KVSTORE_SQLITE3_LIB} - building sqlite_bench")
        add_executable(sqlite_bench bench/sqlite_bench.cc)
        target_include_directories(sqlite_bench PRIVATE ${KVSTORE_SQLITE3_INCLUDE})
        target_link_libraries(sqlite_bench benchmark::benchmark ${KVSTORE_SQLITE3_LIB})
        # bench_util.h lives in bench/
        target_include_directories(sqlite_bench PRIVATE ${CMAKE_SOURCE_DIR}/bench)
    else()
        message(STATUS "SQLite3 not found - skipping sqlite_bench baseline")
    endif()
    target_include_directories(db_bench PRIVATE ${CMAKE_SOURCE_DIR}/bench)
endif()

# ------------------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------------------
message(STATUS "")
message(STATUS "KVStore Configuration Summary:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  Source Dir:   ${CMAKE_SOURCE_DIR}")
message(STATUS "  Binary Dir:   ${CMAKE_BINARY_DIR}")
message(STATUS "")


# ------------------------------------------------------------------------------
# Demo Executable (for manual testing)
# ------------------------------------------------------------------------------
# Named kvstore_demo so the kvstore_test name stays free for test binaries.
add_executable(kvstore_demo src/main.cc)
target_link_libraries(kvstore_demo kvstore_lib)

# ------------------------------------------------------------------------------
# Inspector Tool
# ------------------------------------------------------------------------------
# Read-only on-disk inspector: meta dump, tree structure, leaf-chain check.
add_executable(kvstore_inspect tools/kvstore_inspect.cc)
target_link_libraries(kvstore_inspect kvstore_lib)
