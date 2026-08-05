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
)
target_include_directories(sekiro_haptics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sekiro_haptics_tests PRIVATE sekiro_haptics)
target_compile_definitions(sekiro_haptics_tests PRIVATE
    SH_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

if(TARGET sekiro_haptics_dualsense_transport)
    target_sources(sekiro_haptics_tests PRIVATE test_dualsense_transport.cpp)
    target_link_libraries(sekiro_haptics_tests PRIVATE sekiro_haptics_dualsense_transport)
endif()

add_test(NAME sekiro_haptics_unit_tests COMMAND sekiro_haptics_tests)
