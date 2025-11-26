# QNX Toolchain File for CMake
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/qnx.cmake ..

# Set system name
set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 7.1)

# Set QNX paths (adjust these based on your QNX installation)
set(QNX_HOST "$ENV{QNX_HOST}")
set(QNX_TARGET "$ENV{QNX_TARGET}")

if(NOT QNX_HOST)
    set(QNX_HOST "/opt/qnx710/host/linux/x86_64")
endif()

if(NOT QNX_TARGET)
    set(QNX_TARGET "/opt/qnx710/target/qnx7")
endif()

message(STATUS "QNX_HOST: ${QNX_HOST}")
message(STATUS "QNX_TARGET: ${QNX_TARGET}")

# Set architecture (can be overridden)
if(NOT CMAKE_SYSTEM_PROCESSOR)
    set(CMAKE_SYSTEM_PROCESSOR aarch64le)  # or x86_64, armv7, etc.
endif()

message(STATUS "Target architecture: ${CMAKE_SYSTEM_PROCESSOR}")

# Set compiler paths
set(CMAKE_C_COMPILER ${QNX_HOST}/usr/bin/qcc)
set(CMAKE_CXX_COMPILER ${QNX_HOST}/usr/bin/q++)
set(CMAKE_AR ${QNX_HOST}/usr/bin/ntoaarch64-ar)
set(CMAKE_RANLIB ${QNX_HOST}/usr/bin/ntoaarch64-ranlib)

# Compiler flags for QNX
set(CMAKE_C_FLAGS_INIT "-Vgcc_ntoaarch64le")
set(CMAKE_CXX_FLAGS_INIT "-Vgcc_ntoaarch64le -lang-c++")

# Set sysroot
set(CMAKE_SYSROOT ${QNX_TARGET})
set(CMAKE_FIND_ROOT_PATH ${QNX_TARGET})

# Search for programs in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Search for libraries and headers in the target directories
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# QNX specific definitions
add_definitions(-D__QNXNTO__)
add_definitions(-D_QNX_SOURCE)

# Additional QNX include paths
include_directories(
    ${QNX_TARGET}/usr/include
    ${QNX_TARGET}/usr/include/c++/v1
)

# Additional QNX library paths
link_directories(
    ${QNX_TARGET}/aarch64le/lib
    ${QNX_TARGET}/aarch64le/usr/lib
)

message(STATUS "QNX toolchain configured successfully")
