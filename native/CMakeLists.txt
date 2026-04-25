# The em-x11 Xlib implementation itself.
#
# Two build products:
#   - emx11_static:  archive (.a) for statically linking into demo wasm programs.
#                    This is the Vite-side build path.
#   - emx11_side:    Emscripten SIDE_MODULE (.wasm) for dlopen consumption,
#                    eventually by Pyodide / Tcl-Tk. Enabled via the
#                    EMX11_BUILD_SIDE_MODULE option (default OFF for now).

option(EMX11_BUILD_SIDE_MODULE "Also build em-x11 as an Emscripten SIDE_MODULE" OFF)

set(EMX11_PUBLIC_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(EMX11_SOURCES
    src/display.c
    src/window.c
    src/gc.c
    src/drawing.c
    src/event.c
    src/atom.c
    src/property.c
    src/color.c
    src/error.c
    src/font.c
    src/wm.c
    src/xrm.c
    src/ext/shape.c
)

# --- Static archive -----------------------------------------------------------

add_library(emx11_static STATIC ${EMX11_SOURCES})

target_include_directories(emx11_static
    PUBLIC  ${EMX11_PUBLIC_HEADERS_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(emx11_static
    PRIVATE ${EMX11_WARN_FLAGS}
            -fvisibility=hidden
)

# Hook JS-side library code (addToLibrary) into the emscripten link step.
# Consumers that link emx11_static inherit this via target_link_options.
set(EMX11_JS_LIBRARY "${CMAKE_SOURCE_DIR}/src/bindings/emx11.library.js")
target_link_options(emx11_static
    INTERFACE
        "SHELL:--js-library ${EMX11_JS_LIBRARY}"
)

set_target_properties(emx11_static PROPERTIES
    OUTPUT_NAME "emx11"
)

# --- Side module (Pyodide path; disabled by default) --------------------------

if(EMX11_BUILD_SIDE_MODULE)
    add_library(emx11_side SHARED ${EMX11_SOURCES})

    target_include_directories(emx11_side
        PUBLIC  ${EMX11_PUBLIC_HEADERS_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_options(emx11_side
        PRIVATE ${EMX11_WARN_FLAGS}
                -fvisibility=hidden
                -fPIC
    )

    target_link_options(emx11_side
        PRIVATE
            "SHELL:-s SIDE_MODULE=1"
            "SHELL:-s EXPORT_ALL=0"
    )

    set_target_properties(emx11_side PROPERTIES
        OUTPUT_NAME "emx11"
        SUFFIX ".wasm"
        PREFIX "lib"
    )
endif()
