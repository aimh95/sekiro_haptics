add_executable(sekiro_haptic_lab main.cpp)
target_link_libraries(sekiro_haptic_lab PRIVATE
    sekiro_haptics
    sekiro_haptics_windows_audio
    sekiro_haptics_dualsense_transport
    hid
    winmm)

# The GUI launches this exe as a child process, so a missing runtime DLL shows
# up there as "the process died immediately" with nothing to read -- which is
# how it first appeared. Staging them next to the exe makes the tool runnable
# straight out of the build directory, which is the only way it is used.
if(MINGW)
    get_filename_component(SH_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        if(EXISTS "${SH_MINGW_BIN}/${dll}")
            add_custom_command(TARGET sekiro_haptic_lab POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${SH_MINGW_BIN}/${dll}" "$<TARGET_FILE_DIR:sekiro_haptic_lab>"
                COMMENT "staging ${dll} next to sekiro_haptic_lab")
        endif()
    endforeach()
endif()
