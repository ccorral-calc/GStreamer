cmake_minimum_required(VERSION 3.10)
project(GStreamerProject VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

# 1. Find GStreamer (Required)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED 
    gstreamer-1.0 
    gstreamer-app-1.0 
    gstreamer-video-1.0 
    gstreamer-base-1.0
)

# 2. Find FFmpeg with Manual Fallback
pkg_check_modules(FFMPEG libavformat libavcodec libavutil libswscale)

if(NOT FFMPEG_FOUND)
    message(WARNING "pkg-config failed to find FFmpeg. Searching manually...")
    # Manual search for headers in the directory you identified
    find_path(AVFORMAT_INCLUDE_DIR NAMES libavformat/avformat.h
              PATHS /usr/include/x86_64-linux-gnu /usr/include)
    
    # Manual search for libraries
    find_library(AVFORMAT_LIB NAMES avformat PATHS /usr/lib/x86_64-linux-gnu)
    find_library(AVCODEC_LIB NAMES avcodec PATHS /usr/lib/x86_64-linux-gnu)
    find_library(AVUTIL_LIB NAMES avutil PATHS /usr/lib/x86_64-linux-gnu)

    set(FFMPEG_INCLUDE_DIRS ${AVFORMAT_INCLUDE_DIR})
    set(FFMPEG_LIBRARIES ${AVFORMAT_LIB} ${AVCODEC_LIB} ${AVUTIL_LIB})
endif()

# Helper function for GStreamer-only targets
function(setup_gst_target TARGET_NAME)
    target_include_directories(${TARGET_NAME} PRIVATE ./include ${GST_INCLUDE_DIRS})
    target_link_libraries(${TARGET_NAME} PRIVATE ${GST_LIBRARIES})
endfunction()

# --- Executables ---
add_executable(main main.cpp)
setup_gst_target(main)

add_executable(GStreamer_test gstreamer.cpp)
setup_gst_target(GStreamer_test)

add_executable(GStreamer_no_encoder gstreamer_no_encoder.cpp)
setup_gst_target(GStreamer_no_encoder)

add_executable(GStreamer_appsink gstreamer_appsink.cpp)
setup_gst_target(GStreamer_appsink)

# --- FFmpeg API Executable ---
add_executable(ffmpeg_api ffmpeg_API.cpp)
target_include_directories(ffmpeg_api PRIVATE 
    ./include 
    ${GST_INCLUDE_DIRS} 
    ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(ffmpeg_api PRIVATE 
    ${GST_LIBRARIES} 
    ${FFMPEG_LIBRARIES}
)
