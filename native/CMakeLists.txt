# The em-x11 Xlib implementation itself.
#
# Two build products:
#   - emx11_static:  archive (.a) for statically linking into demo wasm programs.
#                    This is the Vite-side build path.
#   - emx11_side:    Emscripten SIDE_MODULE (.wasm) for dlopen consumption,
#                    eventually by Pyodide / Tcl-Tk. Enabled via the
#                    EMX11_BUILD_SIDE_MODULE option (default OFF for now).

option(EMX11_BUILD_SIDE_MODULE "Also build em-x11 as an Emscripten SIDE_MODULE" OFF)
option(EMX11_HIDE_INTERNAL_SYMBOLS "Apply -fvisibility=hidden to emx11_static (smaller demo wasm). Turn OFF when consumers will relink into a side module that needs to export Xlib." ON)

set(EMX11_PUBLIC_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(EMX11_SOURCES
    src/display.c
    src/window.c
    src/gc.c
    src/drawing.c
    src/event.c
    src/atom.c
    src/property.c
    src/selection.c
    src/color.c
    src/error.c
    src/font.c
    src/wm.c
    src/xrm.c
    src/pixmap.c
    src/xt_stubs.c
    src/xaw_stubs.c
    src/tk_shims.c
    src/keysym_table.c
    src/bridges.c
    src/notifier.c
    src/ext/shape.c
)

# Regenerate keysym_table.c from keysymdef.h whenever the header changes.
# Keeps XStringToKeysym / XKeysymToString in sync with the full X11 set
# without having to hand-edit ~2000 entries. The awk script is committed;
# running it is a no-op on a clean checkout.
set(EMX11_KEYSYM_DEF "${EMX11_PUBLIC_HEADERS_DIR}/X11/keysymdef.h")
set(EMX11_KEYSYM_OUT "${CMAKE_CURRENT_SOURCE_DIR}/src/keysym_table.c")
set(EMX11_KEYSYM_AWK "${CMAKE_CURRENT_SOURCE_DIR}/src/gen_keysyms.awk")
find_program(AWK_EXECUTABLE NAMES awk gawk)
if(AWK_EXECUTABLE)
    add_custom_command(
        OUTPUT  "${EMX11_KEYSYM_OUT}"
        COMMAND "${AWK_EXECUTABLE}" -f "${EMX11_KEYSYM_AWK}" "${EMX11_KEYSYM_DEF}"
                > "${EMX11_KEYSYM_OUT}"
        DEPENDS "${EMX11_KEYSYM_DEF}" "${EMX11_KEYSYM_AWK}"
        COMMENT "Regenerating keysym_table.c from keysymdef.h"
        VERBATIM
    )
endif()

# --- Static archive -----------------------------------------------------------

add_library(emx11_static STATIC ${EMX11_SOURCES})

target_include_directories(emx11_static
    PUBLIC  ${EMX11_PUBLIC_HEADERS_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(emx11_static
    PRIVATE ${EMX11_WARN_FLAGS}
)
if(EMX11_HIDE_INTERNAL_SYMBOLS)
    target_compile_options(emx11_static PRIVATE -fvisibility=hidden)
endif()

# JS-side bridges live in src/bridges.c via EM_JS, so they're embedded
# in libemx11 itself (works under both static link and SIDE_MODULE dlopen).
# Older --js-library files under src/bindings/ are kept as documentation
# only; the build no longer references them.

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

    # Side module is consumed by libtk.so / _tkinter.so via dynamic linking,
    # so Xlib symbols MUST be exported. Default visibility (not hidden) plus
    # EXPORT_ALL=1 lets the auto-export pass at link time pick them up.
    target_compile_options(emx11_side
        PRIVATE ${EMX11_WARN_FLAGS}
                -fPIC
    )

    target_link_options(emx11_side
        PRIVATE
            "SHELL:-s SIDE_MODULE=1"
    )

    set_target_properties(emx11_side PROPERTIES
        OUTPUT_NAME "emx11"
        SUFFIX ".so"
        PREFIX "lib"
    )
endif()
