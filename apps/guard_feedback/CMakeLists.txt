add_executable(sekiro_guard_feedback main.cpp)
target_link_libraries(sekiro_guard_feedback PRIVATE
    sekiro_haptics
    sekiro_haptics_windows_audio
    sekiro_haptics_dualsense_transport
    sekiro_haptics_win32_process
    hid
    winmm)
