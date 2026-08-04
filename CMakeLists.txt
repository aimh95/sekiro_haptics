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
    src/IHapticBackend.cpp
    src/MockHapticBackend.cpp
    src/HapticScheduler.cpp
    src/IDualSenseTransport.cpp
    src/DualSenseUsbReport.cpp
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

# HIDAPI-based USB transport spike for a real DualSense controller. Kept as
# its own target so the core library and MockHapticBackend-based tests never
# need to depend on HIDAPI or real hardware (see docs/ARCHITECTURE.md).
option(SEKIRO_HAPTICS_BUILD_DUALSENSE_TRANSPORT "Build the HIDAPI-based DualSense USB transport" ON)

if(SEKIRO_HAPTICS_BUILD_DUALSENSE_TRANSPORT)
    include(FetchContent)
    FetchContent_Declare(
        hidapi
        GIT_REPOSITORY https://github.com/libusb/hidapi.git
        GIT_TAG hidapi-0.15.0
    )
    # Build HIDAPI as a static library so consumers don't need to ship/copy
    # an extra DLL next to the console app / test binaries for this spike.
    # Scoped (non-cache) so it only affects HIDAPI's own option() defaults.
    set(BUILD_SHARED_LIBS OFF)
    set(HIDAPI_WITH_TESTS OFF)
    FetchContent_MakeAvailable(hidapi)

    add_library(sekiro_haptics_dualsense_transport STATIC
        src/HidApiDualSenseTransport.cpp
    )
    target_include_directories(sekiro_haptics_dualsense_transport PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_link_libraries(sekiro_haptics_dualsense_transport
        PUBLIC sekiro_haptics
        PRIVATE hidapi::hidapi
    )

    if(MSVC)
        target_compile_options(sekiro_haptics_dualsense_transport PRIVATE /W4)
    else()
        target_compile_options(sekiro_haptics_dualsense_transport PRIVATE -Wall -Wextra)
    endif()
endif()

if(SEKIRO_HAPTICS_BUILD_APPS)
    add_subdirectory(apps/console_test)
    if(TARGET sekiro_haptics_dualsense_transport)
        add_subdirectory(apps/dualsense_rumble_test)
    endif()
endif()

if(SEKIRO_HAPTICS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
