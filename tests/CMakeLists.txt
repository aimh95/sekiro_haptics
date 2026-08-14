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
    test_win32_process_reader.cpp
    test_win32_process_reader_integration.cpp
    test_executable_identity.cpp
    test_aob_pattern.cpp
    test_aob_scanner.cpp
    test_rip_relative.cpp
    test_aob_positive_negative_fixtures.cpp
    test_aob_scanner_integration.cpp
    test_address_resolver.cpp
    test_address_resolver_integration.cpp
    test_signature_profile_repository.cpp
)
target_include_directories(sekiro_haptics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sekiro_haptics_tests PRIVATE sekiro_haptics)

# Test-only helper process for the real Win32ProcessReader and
# AobScanner/RipRelative integration tests above -- a small, separate,
# harmless executable with known byte patterns at known addresses. Not
# part of the production project. Links sekiro_haptics only for its public
# include directory (kAobScanChunkBytes) -- it does not otherwise use the
# library.
add_executable(sekiro_haptics_process_reader_helper process_reader_helper_main.cpp)
target_link_libraries(sekiro_haptics_process_reader_helper PRIVATE sekiro_haptics)

target_compile_definitions(sekiro_haptics_tests PRIVATE
    SH_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    SH_PROCESS_READER_HELPER_EXE="$<TARGET_FILE:sekiro_haptics_process_reader_helper>"
    SH_PROCESS_READER_HELPER_EXE_NAME="$<TARGET_FILE_NAME:sekiro_haptics_process_reader_helper>")
add_dependencies(sekiro_haptics_tests sekiro_haptics_process_reader_helper)

if(TARGET sekiro_haptics_dualsense_transport)
    target_sources(sekiro_haptics_tests PRIVATE test_dualsense_transport.cpp)
    target_link_libraries(sekiro_haptics_tests PRIVATE sekiro_haptics_dualsense_transport)
endif()

add_test(NAME sekiro_haptics_unit_tests COMMAND sekiro_haptics_tests)
