# CMake toolchain file for cross-compiling this project to a Windows
# (x86_64) target from a WSL/Linux host, using the mingw-w64 cross
# compiler (`sudo apt install mingw-w64 ninja-build`). Not used by the
# native Windows build (build/, configured directly by a Windows-side
# g++) -- this file exists purely so a WSL shell can produce the same
# Windows binaries without invoking the Windows-installed compiler across
# the WSL/Windows path boundary.
#
# Usage (from the repo root, in WSL):
#   cmake -S . -B build-wsl -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/wsl-mingw-w64-toolchain.cmake
#   cmake --build build-wsl
#
# Setting CMAKE_SYSTEM_NAME here (not just the compiler paths) is what
# makes CMake treat this as a real cross-compile -- without it, WIN32
# evaluates false during configure, and any fetched dependency that
# branches on WIN32 (e.g. hidapi) picks its Linux backend and starts
# looking for pkg-config/libusb/iconv/udev, none of which this project
# needs for its actual Windows target.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Statically link the MinGW runtime (libgcc/libstdc++/libwinpthread) so
# binaries built here carry no dependency on whichever MinGW runtime DLLs
# (if any) happen to be on PATH when they're later run -- this WSL-side
# apt mingw-w64 toolchain's runtime DLLs are a different build than the
# Windows-side WinGet-installed one used for build/, and the two are not
# guaranteed interchangeable.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static")

# Where the cross toolchain's own headers/libs live -- search there (and
# nowhere in the WSL host's own /usr) for libraries/includes, but still
# allow running host-native *programs* (e.g. code generators) if a future
# dependency needs one.
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
