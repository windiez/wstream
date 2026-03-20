if (NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
endif()

# third-party
# SDL2 — not needed for WASM builds (browser Web Audio API is used instead)
if (NOT EMSCRIPTEN)
    find_package(SDL2 REQUIRED)
    string(STRIP "${SDL2_LIBRARIES}" SDL2_LIBRARIES)
    message(STATUS "SDL2_INCLUDE_DIRS = ${SDL2_INCLUDE_DIRS}")
    message(STATUS "SDL2_LIBRARIES    = ${SDL2_LIBRARIES}")
endif()

# common

set(TARGET common)

unset(COMMON_EXTRA_LIBS)

if (WHISPER_FFMPEG)
    # As of cmake 3.27, there is no official cmake support for FindFFmpeg.
    # Consequnelty we added a FindFFmpeg.cmake script the cmake subfolder:
    # whisper.cpp does not need the full ffmpeg libs, just AVFORMAT AVCODEC AVUTIL SWRESAMPLE
    # libswresample  performs highly optimized audio resampling, rematrixing and sample format conversion operations
    # libavcodec provides a generic encoding/decoding framework and contains multiple decoders and encoders for audio, video and subtitle streams, and several bitstream filters.
    # libavformat provides a generic framework for multiplexing and demultiplexing (muxing and demuxing) audio, video and subtitle streams.
    find_package(FFmpeg REQUIRED)

    if (NOT ${FFMPEG_FOUND})
        message(FATAL_ERROR "Cannot find ffmpeg libs/headers")
    endif()

    message(STATUS "Found ffmpeg libs:       ${FFMPEG_LIBRARIES}")
    message(STATUS "Found ffmpeg headers in: ${FFMPEG_INCLUDE_DIRS}")
    message(STATUS "ffmpeg definitions:      ${FFMPEG_DEFINITIONS}")
    message(STATUS "Found avformat           ${AVFORMAT_VERSION}")

    include_directories(${FFMPEG_INCLUDE_DIRS})
    add_compile_definitions(WHISPER_FFMPEG)

    list(APPEND COMMON_EXTRA_LIBS ${FFMPEG_LIBRARIES})

    set(COMMON_SOURCES_FFMPEG ffmpeg-transcode.cpp)
endif()

add_library(${TARGET} STATIC
    common.h
    common.cpp
    common-ggml.h
    common-ggml.cpp
    common-whisper.h
    common-whisper.cpp
    grammar-parser.h
    grammar-parser.cpp
    ${COMMON_SOURCES_FFMPEG}
    )

target_link_libraries(${TARGET} PRIVATE whisper ${COMMON_EXTRA_LIBS})

set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(${TARGET} PROPERTIES FOLDER "libs")

# add json lib
add_library(json_cpp INTERFACE)
target_include_directories(json_cpp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS ${TARGET} RUNTIME)