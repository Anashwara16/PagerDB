cmake_minimum_required(VERSION 3.16)
project(kvstore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler warnings
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Library sources
set(KVSTORE_SOURCES
    src/storage/file.cc
    src/storage/pager.cc
)

# Main library
add_library(kvstore ${KVSTORE_SOURCES})
target_include_directories(kvstore PUBLIC ${PROJECT_SOURCE_DIR}/include)

# Option to build tests
option(BUILD_TESTS "Build tests" ON)

if(BUILD_TESTS)
    find_package(GTest QUIET)
    
    if(GTest_FOUND)
        message(STATUS "Found system GoogleTest")
    else()
        message(STATUS "System GoogleTest not found, trying FetchContent...")
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    enable_testing()

    set(TEST_SOURCES
        tests/test_util.cc
        tests/file_test.cc
        tests/pager_test.cc
    )

    add_executable(kvstore_tests ${TEST_SOURCES})
    target_link_libraries(kvstore_tests PRIVATE kvstore GTest::gtest_main)
    target_include_directories(kvstore_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

    include(GoogleTest)
    gtest_discover_tests(kvstore_tests)
endif()
