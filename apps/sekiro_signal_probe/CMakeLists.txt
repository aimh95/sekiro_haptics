add_executable(sekiro_haptics_signal_probe main.cpp)
target_link_libraries(sekiro_haptics_signal_probe PRIVATE sekiro_haptics_win32_process)
# XInput: polls the controller's own button state (read-only, no hooking)
# to auto-mark the precise instant the guard button is pressed -- see
# RunControllerGuardWatchThread() in main.cpp.
target_link_libraries(sekiro_haptics_signal_probe PRIVATE xinput9_1_0)
