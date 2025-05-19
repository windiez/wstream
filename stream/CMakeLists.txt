cmake_minimum_required(VERSION 3.15)
project(wstream)

cmake_policy(SET CMP0167 OLD)

find_package(nlohmann_json REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(SDL2 REQUIRED)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)
include_directories(${SDL2_INCLUDE_DIRS})

set(TARGET common-sdl)

add_library(${TARGET} STATIC
    common-sdl.h
    common-sdl.cpp
    )

target_include_directories(${TARGET} PUBLIC  ${SDL2_INCLUDE_DIRS})
target_link_libraries     (${TARGET} PRIVATE ${SDL2_LIBRARIES})

set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(${TARGET} PROPERTIES FOLDER "libs")

set(TARGET wstream)
add_executable(${TARGET} stream.cpp)

target_link_libraries(${TARGET} PRIVATE
    common
    common-sdl
    whisper
    nlohmann_json::nlohmann_json
    Boost::system
    ${CMAKE_THREAD_LIBS_INIT})

install(TARGETS ${TARGET} RUNTIME)
