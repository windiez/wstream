cmake_minimum_required(VERSION 3.14)
project("wstream" C CXX)
project("wstream" VERSION 0.0.1)
include(CheckIncludeFileCXX)

set(SOVERSION 1)

# WITH CUDA:
# cmake .. -DGGML_CUDA=ON -DGGML_CUDA_F16=ON -DGGML_CUDA_GRAPHS=ON -DGGML_NATIVE=ON \
# -DGGML_LTO=ON -DGGML_BLAS=ON -DGGML_OPENMP=ON -DBUILD_SHARED_LIBS=OFF \
# -DCMAKE_BUILD_TYPE=Release -GNinja -DGGML_BLAS_VENDOR=OpenBLAS

# NO CUDA:
# cmake .. -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=ON -DGGML_OPENMP=ON \
# -DGGML_VULKAN=ON -DGGML_CUDA=OFF -DGGML_LTO=ON -DBUILD_SHARED_LIBS=OFF -GNinja

find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
    message("Found ccache ${CCACHE_FOUND}")
    message("Using ccache to speed up compilation")
    set(ENV{CCACHE_CPP2} "yes")
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_FOUND})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_FOUND})
endif(CCACHE_FOUND)

set(CMAKE_WARN_UNUSED_CLI YES)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

if (NOT XCODE AND NOT MSVC AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "RelWithDebInfo")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Detect CPU architecture and set march/mcpu flags
set(MARCH_NATIVE "")  # Default to empty; fallback to -march=native later

if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    # Detect Apple M1/M2/M3 CPUs
    execute_process(
        COMMAND sysctl -n machdep.cpu.brand_string
        OUTPUT_VARIABLE APPLE_CPU_BRAND
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "Detected Apple CPU: ${APPLE_CPU_BRAND}")

    if(APPLE_CPU_BRAND MATCHES "M3")
        set(MARCH_NATIVE "-mcpu=apple-m3")
    elseif(APPLE_CPU_BRAND MATCHES "M2")
        set(MARCH_NATIVE "-mcpu=apple-m2")
    elseif(APPLE_CPU_BRAND MATCHES "M1")
        set(MARCH_NATIVE "-mcpu=apple-m1")
    else()
        message(WARNING "Unknown Apple Silicon CPU, using default flags")
    endif()

elseif(UNIX AND NOT APPLE)
    find_program(LSCPU lscpu)
    if(LSCPU)
        execute_process(
            COMMAND ${LSCPU}
            OUTPUT_VARIABLE LSCPU_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        string(REGEX MATCH "Model name:[^\n]+" CPU_LINE "${LSCPU_OUTPUT}")
        string(REGEX REPLACE ".*Model name:[ \t]*" "" CPU_MODEL "${CPU_LINE}")

        message(STATUS "Detected CPU model: ${CPU_MODEL}")

        if(CPU_MODEL MATCHES "Zen 2")
            set(MARCH_NATIVE "-march=znver2")
        elseif(CPU_MODEL MATCHES "Zen 3")
            set(MARCH_NATIVE "-march=znver3")
        elseif(CPU_MODEL MATCHES "Zen 4")
            set(MARCH_NATIVE "-march=znver4")
        elseif(CPU_MODEL MATCHES "Alder Lake")
            set(MARCH_NATIVE "-march=alderlake")
        elseif(CPU_MODEL MATCHES "Ryzen AI 9 HX 370")
            set(MARCH_NATIVE "-march=znver5")
        endif()
    endif()
endif()

if(MARCH_NATIVE STREQUAL "")
    set(MARCH_NATIVE "-march=native")
endif()

message(STATUS "Using architecture-specific flags: ${MARCH_NATIVE}")

string(
    APPEND opts
        "$<$<AND:$<CONFIG:RELEASE>,$<COMPILE_LANGUAGE:CXX>>:-Wall;-Wno-deprecated-declarations;-O3;${MARCH_NATIVE};-std=c++20;-fPIC;-DBOOST_BIND_GLOBAL_PLACEHOLDERS;-flto;-fomit-frame-pointer;>"
        "$<$<AND:$<CONFIG:DEBUG>,$<COMPILE_LANGUAGE:CXX>>:-Wall;-Wno-deprecated-declarations;-O0;-g3;-std=c++20;-D_GLIBCXX_DEBUG;-fPIC;-DBOOST_BIND_GLOBAL_PLACEHOLDERS>"
        "$<$<AND:$<CONFIG:RELWITHDEBINFO>,$<COMPILE_LANGUAGE:CXX>>:-Wall;-Wno-deprecated-declarations;-O3;${MARCH_NATIVE};-g;-std=c++20;-fPIC;-DBOOST_BIND_GLOBAL_PLACEHOLDERS;-flto;-fomit-frame-pointer;>"
        "$<$<AND:$<CONFIG:RELEASE>,$<COMPILE_LANGUAGE:C>>:-Wall;-Wno-deprecated-declarations;-O3;${MARCH_NATIVE};-fPIC;-DBOOST_BIND_GLOBAL_PLACEHOLDERS;-flto;-fomit-frame-pointer;>"
        "$<$<AND:$<CONFIG:DEBUG>,$<COMPILE_LANGUAGE:C>>:-Wall;-Wno-deprecated-declarations;-O0;-g3;-D_GLIBCXX_DEBUG;-fPIC;-DBOOST_BIND_GLOBAL_PLACEHOLDERS>"
        "$<$<AND:$<CONFIG:RELWITHDEBINFO>,$<COMPILE_LANGUAGE:C>>:-Wall;-Wno-deprecated-declarations;-O3;${MARCH_NATIVE};-g;-fPIC;-DBOOST_BIND_GLOBAL_PLACEHOLDERS;-flto;-fomit-frame-pointer;>"
)
add_compile_options("${opts}")

if (EMSCRIPTEN)
    set(BUILD_SHARED_LIBS_DEFAULT OFF)

    option(WHISPER_WASM_SINGLE_FILE "whisper: embed WASM inside the generated whisper.js" ON)

    # TODO: without these, we get the following error:
    #       wasm-ld: error: --shared-memory is disallowed by whisper.cpp.o because it was not compiled with 'atomics' or 'bulk-memory' features.
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -pthread")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s TOTAL_STACK=5242880")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -s TOTAL_STACK=5242880")

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated")

    # Add this near the top of your CMakeLists.txt, before any project() calls
    # Set default Emscripten options
    set(WHISPER_WASM_SINGLE_FILE ON CACHE BOOL "Embed WASM in JS file")
    set(GGML_VULKAN OFF CACHE BOOL "Disable Vulkan for WASM")
    set(GGML_OPENMP OFF CACHE BOOL "Disable OpenMP for WASM")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static linking for WASM")

    # Disable all GPU backends
    set(GGML_CUDA OFF CACHE BOOL "Disable CUDA for WASM")
    set(GGML_METAL OFF CACHE BOOL "Disable Metal for WASM")
    set(GGML_OPENCL OFF CACHE BOOL "Disable OpenCL for WASM")

    # Disable CPU optimizations not available in WASM
    set(GGML_BLAS OFF CACHE BOOL "Disable BLAS for WASM")
    set(GGML_OPENBLAS OFF CACHE BOOL "Disable OpenBLAS for WASM")

    message(STATUS "Configuring for Emscripten build")

    add_subdirectory(ggml)
    add_subdirectory(src)
    add_subdirectory(common)
    add_subdirectory(stream2.wasm)

else()
    if (MINGW)
        set(BUILD_SHARED_LIBS_DEFAULT OFF)
    else()
        set(BUILD_SHARED_LIBS_DEFAULT ON)
    endif()

    add_subdirectory(ggml)
    add_subdirectory(src)
    add_subdirectory(common)
    add_subdirectory(stream)

endif()
