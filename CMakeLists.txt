cmake_minimum_required(VERSION 3.20)
project(em-x11 C)

# Enforce Emscripten toolchain. The `emcmake` wrapper sets CMAKE_TOOLCHAIN_FILE
# to Emscripten.cmake; if someone runs bare cmake, fail loudly.
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "em-x11 must be configured through emcmake. "
        "From inside WSL / Linux: emcmake cmake -S . -B build")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output artifacts into a predictable location that Vite can serve.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts")

# Default warning flags for our own code. Third-party code opts out.
set(EMX11_WARN_FLAGS -Wall -Wextra -Wno-unused-parameter)

add_subdirectory(native)
add_subdirectory(third-party/libXt)
add_subdirectory(demos/hello)
add_subdirectory(demos/xt-hello)
