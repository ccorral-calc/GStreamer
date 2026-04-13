cmake_minimum_required(VERSION 3.10)
project(GStreamer VERSION 1.0 LANGUAGES CXX)

# Enable C++17 standard
set(CMAKE_CXX_STANDARD 17)

# Optionally enable ASan
option(ASAN "Enable AddressSanitizer" OFF)
if(ASAN)
  message(STATUS "AdressSanitizer enabled")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address")
endif()

# Set global C++ flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-Map=output.map")

# Set C++ flags for Debug and Release builds
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -D_GLIBCXX_DEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")

# Find the PkgConfig tool
find_package(PkgConfig REQUIRED)

# Find the GStreamer library
pkg_check_modules(GST REQUIRED 
    gstreamer-1.0 
    gstreamer-app-1.0 
    gstreamer-video-1.0 
    gstreamer-base-1.0
)

set(SOURCES
    main.cpp
)

add_executable(GStreamer ${SOURCES})
target_include_directories(
    GStreamer
    PUBLIC ./include
    ${GST_INCLUDE_DIRS}
)
target_link_libraries(
    GStreamer
    PRIVATE ${GST_LIBRARIES}
)