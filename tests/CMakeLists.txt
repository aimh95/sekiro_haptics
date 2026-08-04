add_executable(sekiro_haptics_tests
    test_main.cpp
    test_haptic_effect.cpp
    test_mock_backend.cpp
    test_scheduler.cpp
)
target_include_directories(sekiro_haptics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sekiro_haptics_tests PRIVATE sekiro_haptics)

add_test(NAME sekiro_haptics_unit_tests COMMAND sekiro_haptics_tests)
