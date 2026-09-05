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
    src/DualSenseLegacyBackend.cpp
    src/HapticScheduler.cpp
    src/IDualSenseTransport.cpp
    src/DualSenseUsbReport.cpp
    src/Json.cpp
    src/events/ManualLabelEventDetector.cpp
    src/signals/IGameSignalSource.cpp
    src/signals/ReplaySignalSource.cpp
    src/signals/VectorSignalSource.cpp
    src/presets/PresetRepository.cpp
    src/presets/MappingRepository.cpp
    src/replay/TraceJsonl.cpp
    src/replay/TraceMetadata.cpp
    src/replay/TraceValidator.cpp
    src/replay/ExpectedEventRepository.cpp
    src/replay/ReplayComparator.cpp
    src/pipeline/ReplayPipeline.cpp
    src/process/AobPattern.cpp
    src/process/AobScanner.cpp
    src/process/RipRelative.cpp
    src/process/SignatureProfileRepository.cpp
    src/process/AddressResolver.cpp
    src/process/IProcessMemoryMap.cpp
    src/process/IProcessInspector.cpp
    src/process/IProcessReader.cpp
    src/process/CandidateScanner.cpp
    src/process/CandidateScanShared.cpp
    src/process/ScanManifest.cpp
    src/process/CandidateStorage.cpp
    src/process/DiskCandidateScanner.cpp
    src/process/SignalProbeScanController.cpp
    src/process/SignalProbeCommandProcessor.cpp
    src/process/DiscoverySession.cpp
    # SEK-PROBE-001D Stage A -- targeted AOB-anchored root resolver, built
    # entirely on AobScanner/RipRelative (both unchanged). OS-independent:
    # only depends on IProcessReader/IProcessInspector/ExecutableIdentity's
    # plain data types, no Win32 API.
    src/process/SekiroKnownRootResolver.cpp
    # SEK-PROBE-001D Stage B -- HP/Posture raw combat reader, built on
    # Stage A. Same OS-independence.
    src/process/SekiroRawCombatReader.cpp
    # SEK-PROBE-001D Stage C -- bounded delta-only capture + offline
    # correlation analyzer, both OS-independent.
    src/process/SekiroCombatCaptureSession.cpp
    src/process/SekiroCombatCaptureAnalyzer.cpp
    # SEK-PROBE-001D CLI wiring (combat-plan/combat-resolve/combat-status/
    # combat-capture/combat-mark/combat-stop/combat-analyze/combat-export),
    # same layering as SignalProbeScanController/SignalProbeCommandProcessor.
    src/process/SekiroCombatSessionController.cpp
    src/process/SekiroCombatCommandProcessor.cpp
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

# Win32-only process-access implementations (real OpenProcess/ReadProcessMemory
# calls, real CNG file hashing) -- kept in their own target, separate from
# the core sekiro_haptics library, so everything in process/ that only
# depends on the IProcessReader/IProcessMemoryMap/IProcessInspector
# interfaces plus test Fakes (AobScanner, AddressResolver, CandidateScanner,
# the disk-backed scanner added in SEK-PROBE-001C, and all of their tests)
# builds and runs on any OS, including non-Windows development machines.
# Real Sekiro attachment, the signal-probe CLI, and every integration test
# that spawns a real helper process to attach to remain Windows-only --
# see docs/05-process-access.md.
if(WIN32)
    add_library(sekiro_haptics_win32_process STATIC
        src/process/Win32Api.cpp
        src/process/Win32ProcessReader.cpp
        src/process/ExecutableIdentity.cpp
    )
    target_include_directories(sekiro_haptics_win32_process PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_link_libraries(sekiro_haptics_win32_process PUBLIC sekiro_haptics)
    # bcrypt: Windows' built-in cryptography API (CNG), used only for the
    # SHA-256 executable-identity hash in src/process/ExecutableIdentity.cpp.
    # No third-party crypto library added.
    target_link_libraries(sekiro_haptics_win32_process PRIVATE bcrypt)

    if(MSVC)
        target_compile_options(sekiro_haptics_win32_process PRIVATE /W4)
    else()
        target_compile_options(sekiro_haptics_win32_process PRIVATE -Wall -Wextra)
    endif()
endif()

# HIDAPI-based USB transport spike for a real DualSense controller. Kept as
# its own target so the core library and MockHapticBackend-based tests never
# need to depend on HIDAPI or real hardware (see docs/01-architecture.md).
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
    add_subdirectory(apps/replay_cli)
    if(TARGET sekiro_haptics_win32_process)
        add_subdirectory(apps/sekiro_signal_probe)
    endif()
    if(TARGET sekiro_haptics_dualsense_transport)
        add_subdirectory(apps/dualsense_rumble_test)
        add_subdirectory(apps/replay_hardware)
    endif()
endif()

if(SEKIRO_HAPTICS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
