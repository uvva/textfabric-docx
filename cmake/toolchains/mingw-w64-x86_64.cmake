# Cross-compile from Linux → Windows x64 using mingw-w64.
# Install on Ubuntu/Debian:   sudo apt install mingw-w64
# Install on Arch:            sudo pacman -S mingw-w64-gcc
# Install via Nix:            pkgs.pkgsCross.mingwW64.buildPackages.gcc

set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_tf_prefix x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${_tf_prefix}-gcc)
set(CMAKE_CXX_COMPILER ${_tf_prefix}-g++)
set(CMAKE_RC_COMPILER  ${_tf_prefix}-windres)
set(CMAKE_AR           ${_tf_prefix}-ar)
set(CMAKE_RANLIB       ${_tf_prefix}-ranlib)

# Search in the mingw sysroot, not the host Linux root.
set(CMAKE_FIND_ROOT_PATH /usr/${_tf_prefix})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Statically link the GCC runtimes so the resulting DLL has no
# mingw-specific .dll dependencies at run time.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
