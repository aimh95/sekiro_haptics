add_executable(sekiro_trigger_lab main.cpp)
target_link_libraries(sekiro_trigger_lab PRIVATE
    sekiro_haptics
    sekiro_haptics_dualsense_transport
    hid
    winmm)
