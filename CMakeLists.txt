cmake_minimum_required(VERSION 3.20)
project(SekiroHaptics LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SEKIRO_HAPTICS_BUILD_APPS "Build the console test application" ON)
option(SEKIRO_HAPTICS_BUILD_TESTS "Build the unit test suite" ON)

add_library(sekiro_haptics STATIC
    src/HapticEffect.cpp
    src/Presets.cpp
    src/MockHapticBackend.cpp
    src/HapticScheduler.cpp
)
target_include_directories(sekiro_haptics PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(sekiro_haptics PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(sekiro_haptics PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(sekiro_haptics PRIVATE /W4)
else()
    target_compile_options(sekiro_haptics PRIVATE -Wall -Wextra)
endif()

if(SEKIRO_HAPTICS_BUILD_APPS)
    add_subdirectory(apps/console_test)
endif()

if(SEKIRO_HAPTICS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
