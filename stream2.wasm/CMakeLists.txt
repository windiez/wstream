set(TARGET libstream)

add_executable(${TARGET}
    emscripten.cpp
    hyni_merge.h
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)

include(DefaultTargetOptions)

target_link_libraries(${TARGET} PRIVATE
    whisper
    common
)

target_compile_options(${TARGET} PRIVATE -std=c++17)

set_target_properties(${TARGET} PROPERTIES LINK_FLAGS " \
    --bind \
    -s USE_PTHREADS=1 \
    -s PTHREAD_POOL_SIZE=8 \
    -s INITIAL_MEMORY=1024MB \
    -s MAXIMUM_MEMORY=4096MB \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s FORCE_FILESYSTEM=1 \
    -s EXPORTED_RUNTIME_METHODS=\"['print', 'printErr', 'ccall', 'cwrap', 'FS_createDataFile']\" \
    ${EXTRA_FLAGS} \
    ")

# Copy all required web files
set(WEB_FILES
    helpers.js
    index.html
    server.py
)

# Copy all web files to output directory
foreach(file ${WEB_FILES})
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${file}
                  ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${file} COPYONLY)
endforeach()
