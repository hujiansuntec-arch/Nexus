cmake_minimum_required(VERSION 3.10)
project(LibRPC VERSION 1.0.0 LANGUAGES CXX)

# CMake options
option(BUILD_SHARED_LIBS "Build shared library" ON)
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_EXAMPLES "Build example programs" ON)

# C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform detection
if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
    message(STATUS "Building for QNX platform")
    add_definitions(-D__QNXNTO__)
    set(QNX_BUILD ON)
else()
    message(STATUS "Building for ${CMAKE_SYSTEM_NAME} platform")
    set(QNX_BUILD OFF)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# Default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Find threads library (pthread)
find_package(Threads REQUIRED)

# Find rt library (realtime extensions)
if(NOT QNX_BUILD)
    find_library(RT_LIBRARY rt REQUIRED)
    message(STATUS "Found rt library: ${RT_LIBRARY}")
endif()

# Source files
set(LIBRPC_SOURCES
    src/NodeImpl.cpp
    src/UdpTransport.cpp
    src/SharedMemoryRegistry.cpp
    src/SharedMemoryTransportV3.cpp
    src/LargeDataChannel.cpp
)

# Header files (for IDE integration)
set(LIBRPC_HEADERS
    include/Node.h
    include/NodeImpl.h
    include/UdpTransport.h
    include/SharedMemoryRegistry.h
    include/SharedMemoryTransportV3.h
    include/LargeDataChannel.h
    include/LockFreeQueue.h
    include/qnx_compat.h
)

# Create library target
if(BUILD_SHARED_LIBS)
    add_library(rpc SHARED ${LIBRPC_SOURCES} ${LIBRPC_HEADERS})
    set_target_properties(rpc PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
        OUTPUT_NAME rpc
    )
    message(STATUS "Building shared library")
else()
    add_library(rpc STATIC ${LIBRPC_SOURCES} ${LIBRPC_HEADERS})
    set_target_properties(rpc PROPERTIES
        OUTPUT_NAME rpc
    )
    message(STATUS "Building static library")
endif()

# Link libraries
target_link_libraries(rpc 
    PUBLIC 
        Threads::Threads
)

if(NOT QNX_BUILD)
    target_link_libraries(rpc PUBLIC ${RT_LIBRARY})
endif()

# Position Independent Code for shared library
set_target_properties(rpc PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Install library
install(TARGETS rpc
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

# Install headers
install(FILES ${LIBRPC_HEADERS} DESTINATION include/librpc)

# Build tests
if(BUILD_TESTS)
    message(STATUS "Building test programs")
    
    # Test: In-process communication
    add_executable(test_inprocess test_inprocess.cpp)
    target_link_libraries(test_inprocess PRIVATE rpc)
    
    # Test: Duplex communication
    add_executable(test_duplex_v2 test_duplex_v2.cpp)
    target_link_libraries(test_duplex_v2 PRIVATE rpc)
    
    # Test: Heartbeat timeout
    add_executable(test_heartbeat_timeout test_heartbeat_timeout.cpp)
    target_link_libraries(test_heartbeat_timeout PRIVATE rpc)
    
    # Test: Cross-process service discovery
    add_executable(test_service_discovery test_service_discovery.cpp)
    target_link_libraries(test_service_discovery PRIVATE rpc)
    
    add_executable(test_cross_process_discovery test_cross_process_discovery.cpp)
    target_link_libraries(test_cross_process_discovery PRIVATE rpc)
    
    add_executable(test_cross_process_node_events test_cross_process_node_events.cpp)
    target_link_libraries(test_cross_process_node_events PRIVATE rpc)
    
    # Test: Large data sender/receiver (optional - may need API updates)
    # if(EXISTS ${PROJECT_SOURCE_DIR}/test_large_sender.cpp)
    #     add_executable(test_large_sender test_large_sender.cpp)
    #     target_link_libraries(test_large_sender PRIVATE rpc)
    # endif()
    # 
    # if(EXISTS ${PROJECT_SOURCE_DIR}/test_large_receiver.cpp)
    #     add_executable(test_large_receiver test_large_receiver.cpp)
    #     target_link_libraries(test_large_receiver PRIVATE rpc)
    # endif()
    
    # Install tests (optional)
    install(TARGETS 
        test_inprocess 
        test_duplex_v2
        test_heartbeat_timeout
        test_service_discovery
        test_cross_process_discovery
        test_cross_process_node_events
        RUNTIME DESTINATION bin/tests
        OPTIONAL
    )
endif()

# Build examples
if(BUILD_EXAMPLES)
    message(STATUS "Building example programs")
    # Add examples here if needed
endif()

# CPack configuration for packaging
set(CPACK_PACKAGE_NAME "librpc")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "High-performance cross-process RPC library")
set(CPACK_PACKAGE_VENDOR "LibRPC")

# Check if LICENSE file exists before setting
if(EXISTS "${PROJECT_SOURCE_DIR}/../LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/../LICENSE")
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/README.md")
    set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
endif()

include(CPack)

# Print configuration summary
message(STATUS "")
message(STATUS "=== LibRPC Configuration Summary ===")
message(STATUS "Version:           ${PROJECT_VERSION}")
message(STATUS "Platform:          ${CMAKE_SYSTEM_NAME}")
message(STATUS "QNX Build:         ${QNX_BUILD}")
message(STATUS "Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard:      C++${CMAKE_CXX_STANDARD}")
message(STATUS "Shared library:    ${BUILD_SHARED_LIBS}")
message(STATUS "Build tests:       ${BUILD_TESTS}")
message(STATUS "Build examples:    ${BUILD_EXAMPLES}")
message(STATUS "Install prefix:    ${CMAKE_INSTALL_PREFIX}")
message(STATUS "====================================")
message(STATUS "")
