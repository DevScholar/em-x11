# em-x11's Xlib implementation, split into the same archive set a real X
# system ships, so demos and downstream consumers can link with the
# standard `-lX11 -lXext -lXrender -lXft -lfontconfig -lGLX` flags
# instead of one fat `emx11_static` blob.
#
# Build products:
#   - libX11.a         (CMake target: X11)         Xlib core + Xt/Xaw stubs + Tk shims
#   - libXext.a        (CMake target: Xext)        XShape
#   - libXrender.a     (CMake target: Xrender)     XRender
#   - libfontconfig.a  (CMake target: fontconfig)  fontconfig shim
#   - libXft.a         (CMake target: Xft)         Xft
#   - libGLX.a         (CMake target: GLX)         glX* (libGL itself comes from emscripten)
#   - libemx11.so      (CMake target: emx11_side)  single SIDE_MODULE for Pyodide dlopen
#
# The static archives expose the standard X11 include path through their
# INTERFACE_INCLUDE_DIRECTORIES, so in-tree consumers (third-party libXt
# etc.) and demos can either link the CMake target directly or just
# reach for `-lXft` after wiring `-L` at the emx11 binary dir.

option(EMX11_BUILD_SIDE_MODULE "Also build em-x11 as an Emscripten SIDE_MODULE" OFF)
option(EMX11_HIDE_INTERNAL_SYMBOLS "Apply -fvisibility=hidden to the static archives (smaller demo wasm). Turn OFF when consumers will relink into a side module that needs to export Xlib." ON)

set(EMX11_PUBLIC_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Sources per archive. Anything an Xlib stub references that is itself
# Xlib (XCreateRegion, XKeysymToString, XCopyPlane, ...) lives in
# libX11.a -- the files are organised per xorg/libX11 conventions.
set(EMX11_X11_SOURCES
    src/display.c
    src/window.c
    src/gc.c
    src/drawing.c
    src/event.c
    src/event_queue.c
    src/event_send.c
    src/event_keysym.c
    src/atom.c
    src/property.c
    src/selection.c
    src/color.c
    src/error.c
    src/font.c
    src/wm.c
    src/xrm.c
    src/pixmap.c
    src/xim.c
    src/Region.c
    src/Text16.c
    src/ImText.c
    src/Cursor.c
    src/xkb.c
    src/keysym_table.c
    src/bridges.c
    src/notifier.c
)
set(EMX11_XEXT_SOURCES       src/ext/shape.c)
set(EMX11_XRENDER_SOURCES    src/ext/xrender.c)
set(EMX11_FONTCONFIG_SOURCES src/fontconfig.c)
set(EMX11_XFT_SOURCES        src/xft.c)
set(EMX11_GLX_SOURCES        src/glx.c)

# Combined list used only by the side-module path -- Pyodide dlopens one
# .so, so everything still needs to land in a single artifact there.
set(EMX11_ALL_SOURCES
    ${EMX11_X11_SOURCES}
    ${EMX11_XEXT_SOURCES}
    ${EMX11_XRENDER_SOURCES}
    ${EMX11_FONTCONFIG_SOURCES}
    ${EMX11_XFT_SOURCES}
    ${EMX11_GLX_SOURCES}
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

# --- Static archives ----------------------------------------------------------

# emx11_add_static_lib(<target> <output_name> <sources...>)
# -- adds a STATIC library with our standard include dirs, warnings, and
# visibility knobs. Output name drops any prefix so files are libX11.a,
# libXext.a, ... -- exactly what a standard `-l<name>` resolves to.
function(emx11_add_static_lib target output_name)
    add_library(${target} STATIC ${ARGN})

    target_include_directories(${target}
        PUBLIC  ${EMX11_PUBLIC_HEADERS_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_options(${target} PRIVATE ${EMX11_WARN_FLAGS})
    if(EMX11_HIDE_INTERNAL_SYMBOLS)
        target_compile_options(${target} PRIVATE -fvisibility=hidden)
    endif()

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${output_name}"
        PREFIX "lib"
    )
endfunction()

emx11_add_static_lib(X11        X11        ${EMX11_X11_SOURCES})
emx11_add_static_lib(Xext       Xext       ${EMX11_XEXT_SOURCES})
emx11_add_static_lib(Xrender    Xrender    ${EMX11_XRENDER_SOURCES})
emx11_add_static_lib(fontconfig fontconfig ${EMX11_FONTCONFIG_SOURCES})
emx11_add_static_lib(Xft        Xft        ${EMX11_XFT_SOURCES})
emx11_add_static_lib(GLX        GLX        ${EMX11_GLX_SOURCES})

# Inter-archive PUBLIC link so in-tree CMake consumers that just say
# `target_link_libraries(foo PRIVATE Xft)` still get the full transitive
# chain (Xft -> Xrender -> X11). For demos that go through bare
# `-lX11 -lXft -lXrender ...` this is a no-op; the linker handles it.
target_link_libraries(Xext       PUBLIC X11)
target_link_libraries(Xrender    PUBLIC X11)
target_link_libraries(fontconfig PUBLIC X11)
target_link_libraries(Xft        PUBLIC Xrender fontconfig X11)
target_link_libraries(GLX        PUBLIC X11)

# JS-side bridges live in src/bridges.c via EM_JS, so they're embedded in
# libX11.a itself (works under both static link and SIDE_MODULE dlopen).

# --- Side module (Pyodide path; disabled by default) --------------------------
#
# Pyodide dlopens one .so, so the side-module path keeps producing a
# single fat libemx11.so containing every source above. The split only
# applies to the static archive consumers.

if(EMX11_BUILD_SIDE_MODULE)
    add_library(emx11_side SHARED ${EMX11_ALL_SOURCES})

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
