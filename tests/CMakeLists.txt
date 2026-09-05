add_executable(sekiro_haptics_tests
    test_main.cpp
    test_haptic_effect.cpp
    test_mock_backend.cpp
    test_scheduler.cpp
    test_dualsense_usb_report.cpp
    test_trace_parser.cpp
    test_replay_signal_source.cpp
    test_event_detector.cpp
    test_profile_resolution.cpp
    test_pipeline.cpp
    test_trace_metadata.cpp
    test_trace_validator.cpp
    test_expected_event_repository.cpp
    test_replay_comparator.cpp
    test_replay_determinism.cpp
    test_dualsense_legacy_backend.cpp
    test_replay_hardware_backend.cpp
    test_scheduler_duration.cpp
    test_aob_pattern.cpp
    test_aob_scanner.cpp
    test_rip_relative.cpp
    test_aob_positive_negative_fixtures.cpp
    test_address_resolver.cpp
    test_signature_profile_repository.cpp
    test_candidate_scanner.cpp
    # SEK-PROBE-001C -- disk-backed candidate scanner. OS-independent: only
    # depends on IProcessReader/IProcessMemoryMap/IProcessInspector, Fakes,
    # and real temporary files -- no Win32 API, builds/runs on any OS.
    test_candidate_storage.cpp
    test_disk_scan_plan.cpp
    test_disk_baseline_scan.cpp
    test_disk_first_filter.cpp
    test_disk_generation_chain.cpp
    test_disk_scan_resume.cpp
    # SEK-PROBE-001C-2 -- OS-independent command/state controller wiring
    # the disk-backed scanner into the probe CLI. Same portability as above.
    test_signal_probe_controller.cpp
    # SEK-PROBE-001D Stage A -- known-root resolver. Fake-based, OS-independent.
    test_sekiro_known_root_resolver.cpp
    # SEK-PROBE-001D Stage B -- raw combat reader + CLI wiring. Fake-based.
    test_sekiro_raw_combat_reader.cpp
    test_sekiro_combat_command_processor.cpp
    # SEK-PROBE-001D Stage C -- capture session + offline analyzer.
    test_sekiro_combat_capture_session.cpp
    test_sekiro_combat_capture_analyzer.cpp
)
target_include_directories(sekiro_haptics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sekiro_haptics_tests PRIVATE sekiro_haptics)

target_compile_definitions(sekiro_haptics_tests PRIVATE
    SH_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

if(TARGET sekiro_haptics_dualsense_transport)
    target_sources(sekiro_haptics_tests PRIVATE test_dualsense_transport.cpp)
    target_link_libraries(sekiro_haptics_tests PRIVATE sekiro_haptics_dualsense_transport)
endif()

# Win32-only: the real Win32ProcessReader/ExecutableIdentity tests, and
# every integration test that spawns a real helper process to attach to
# (Windows process/pipe APIs have no portable equivalent here -- see
# docs/05-process-access.md). Everything else in this test binary builds
# and runs on any OS.
if(TARGET sekiro_haptics_win32_process)
    target_sources(sekiro_haptics_tests PRIVATE
        test_process_memory_map.cpp
        test_win32_process_reader.cpp
        test_win32_process_reader_integration.cpp
        test_executable_identity.cpp
        test_aob_scanner_integration.cpp
        test_address_resolver_integration.cpp
        test_signal_probe_integration.cpp
        # SEK-PROBE-001C-2 -- disk-backed scanner end-to-end integration
        # test, reusing process_reader_helper_main.cpp unmodified. Win32-only
        # (spawns and attaches to a real process) -- written and registered
        # here, but never executed in the Linux development sandbox. Must be
        # run on a real Windows machine before being reported as verified.
        test_disk_candidate_scanner_integration.cpp
    )
    target_link_libraries(sekiro_haptics_tests PRIVATE sekiro_haptics_win32_process)

    # Test-only helper process for the real Win32ProcessReader and
    # AobScanner/RipRelative/disk-scanner integration tests above -- a
    # small, separate, harmless executable with known byte patterns at
    # known addresses. Not part of the production project. Links
    # sekiro_haptics only for its public include directory
    # (kAobScanChunkBytes) -- it does not otherwise use the library.
    add_executable(sekiro_haptics_process_reader_helper process_reader_helper_main.cpp)
    target_link_libraries(sekiro_haptics_process_reader_helper PRIVATE sekiro_haptics)

    target_compile_definitions(sekiro_haptics_tests PRIVATE
        SH_PROCESS_READER_HELPER_EXE="$<TARGET_FILE:sekiro_haptics_process_reader_helper>"
        SH_PROCESS_READER_HELPER_EXE_NAME="$<TARGET_FILE_NAME:sekiro_haptics_process_reader_helper>")
    add_dependencies(sekiro_haptics_tests sekiro_haptics_process_reader_helper)
endif()

# SEK-PROBE-001C-LINUX-E2E -- Linux-only, real-child-process disk-backed
# scanner verification. Native Linux stands in for the real Windows/Sekiro
# attach this sandbox cannot perform: a real forked child process, real
# process_vm_readv()/proc-maps-backed IProcessReader/IProcessInspector/
# IProcessMemoryMap implementation (LinuxChildProcessReader), and the real,
# unmodified SignalProbeScanController/DiskCandidateScanner/CandidateStorage/
# ScanManifest production code. Test-only -- never linked into any
# production/Windows target, and never promoted to a Linux/Proton product
# backend (see docs/06-signal-discovery-probe.md).
if(UNIX AND NOT APPLE)
    # Real, separate, harmless child process with a known mmap arena --
    # not part of the production project, not a stand-in for any real game.
    add_executable(sekiro_haptics_linux_signal_probe_helper linux_signal_probe_helper.cpp)

    # Test-only Linux process adapter + shared E2E scenario logic, used by
    # both the CTest-registered integration test and the manual runner
    # below. Links sekiro_haptics for the real scanner/controller code it
    # drives -- this library is never linked into any Windows target.
    add_library(sekiro_haptics_linux_probe_test_support STATIC
        LinuxChildProcessReader.cpp
        LinuxProbeE2EScenario.cpp
    )
    target_include_directories(sekiro_haptics_linux_probe_test_support PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(sekiro_haptics_linux_probe_test_support PUBLIC sekiro_haptics)
    if(MSVC)
        target_compile_options(sekiro_haptics_linux_probe_test_support PRIVATE /W4)
    else()
        target_compile_options(sekiro_haptics_linux_probe_test_support PRIVATE -Wall -Wextra)
    endif()
    target_compile_definitions(sekiro_haptics_linux_probe_test_support PRIVATE
        SH_LINUX_SIGNAL_PROBE_HELPER_EXE="$<TARGET_FILE:sekiro_haptics_linux_signal_probe_helper>")
    add_dependencies(sekiro_haptics_linux_probe_test_support sekiro_haptics_linux_signal_probe_helper)

    target_sources(sekiro_haptics_tests PRIVATE test_disk_candidate_scanner_linux_e2e.cpp)
    target_link_libraries(sekiro_haptics_tests PRIVATE sekiro_haptics_linux_probe_test_support)
    target_compile_definitions(sekiro_haptics_tests PRIVATE
        SH_LINUX_SIGNAL_PROBE_HELPER_EXE="$<TARGET_FILE:sekiro_haptics_linux_signal_probe_helper>")
    add_dependencies(sekiro_haptics_tests sekiro_haptics_linux_signal_probe_helper)

    # Manual, non-CTest runner for a human (or Claude) to run directly at a
    # realistic scale (default 256 MiB) -- see
    # docs/06-signal-discovery-probe.md for exact invocation instructions.
    add_executable(sekiro_haptics_linux_probe_e2e linux_probe_e2e_main.cpp)
    target_include_directories(sekiro_haptics_linux_probe_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(sekiro_haptics_linux_probe_e2e PRIVATE sekiro_haptics_linux_probe_test_support)
    target_compile_definitions(sekiro_haptics_linux_probe_e2e PRIVATE
        SH_LINUX_SIGNAL_PROBE_HELPER_EXE="$<TARGET_FILE:sekiro_haptics_linux_signal_probe_helper>")
    add_dependencies(sekiro_haptics_linux_probe_e2e sekiro_haptics_linux_signal_probe_helper)
endif()

add_test(NAME sekiro_haptics_unit_tests COMMAND sekiro_haptics_tests)
