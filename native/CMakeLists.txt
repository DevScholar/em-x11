# em-x11's Xlib implementation, split into the same archive set a real X
# system ships, so demos and downstream consumers can link with the
# standard `-lX11 -lXext -lXrender -lXft -lfontconfig -lGLX` flags
# instead of one fat `em_x11_static` blob.
#
# Build products:
#   - libX11.a         (CMake target: X11)         Xlib core + Xt/Xaw stubs + Tk shims
#   - libXext.a        (CMake target: Xext)        XShape
#   - libXrender.a     (CMake target: Xrender)     XRender
#   - libfontconfig.a  (CMake target: fontconfig)  fontconfig shim
#   - libXft.a         (CMake target: Xft)         Xft
#   - libGLX.a         (CMake target: GLX)         glX* (libGL itself comes from emscripten)
#   - libem_x11.so      (CMake target: em_x11_side)  single SIDE_MODULE for Pyodide dlopen
#
# The static archives expose the standard X11 include path through their
# INTERFACE_INCLUDE_DIRECTORIES, so in-tree consumers (third-party libXt
# etc.) and demos can either link the CMake target directly or just
# reach for `-lXft` after wiring `-L` at the em_x11 binary dir.

option(EM_X11_BUILD_SIDE_MODULE "Also build em-x11 as an Emscripten SIDE_MODULE" OFF)
option(EM_X11_HIDE_INTERNAL_SYMBOLS "Apply -fvisibility=hidden to the static archives (smaller demo wasm). Turn OFF when consumers will relink into a side module that needs to export Xlib." ON)

set(EM_X11_PUBLIC_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Sources per archive, organised to mirror the xorg source tree layout:
#
#   dix/        — xserver/dix/        (device-independent X layer)
#   Xext/       — xserver/Xext/       (built-in extensions: SHAPE, ...)
#   render/     — xserver/render/     (RENDER extension)
#   xkb/        — xserver/xkb/        (XKB extension)
#   glx/        — xserver/glx/        (GLX extension)
#   libX11/     — libX11/src/         (Xlib client API drawing + text + Xrm)
#   em_x11/      — em-x11–specific     (wasm bridges, browser glue, etc.)
#
# Files in em_x11/ have no direct xorg counterpart.
set(EM_X11_X11_SOURCES
    em_x11/display.c
    dix/window.c
    dix/gc.c
    libX11/FillRct.c
    libX11/DrRect.c
    libX11/FillRcts.c
    libX11/DrLine.c
    libX11/DrLines.c
    libX11/DrSegs.c
    libX11/DrPoint.c
    libX11/DrPoints.c
    libX11/DrArc.c
    libX11/DrArcs.c
    libX11/FillArc.c
    libX11/FillArcs.c
    libX11/FillPoly.c
    libX11/Clear.c
    libX11/ClearArea.c
    libX11/CopyArea.c
    libX11/CopyPlane.c
    libX11/PutImage.c
    dix/events.c
    em_x11/event_queue.c
    em_x11/event_send.c
    em_x11/event_keysym.c
    dix/atom.c
    dix/property.c
    dix/selection.c
    dix/colormap.c
    em_x11/error.c
    dix/dixfonts.c
    em_x11/wm.c
    libX11/Xrm.c
    dix/pixmap.c
    em_x11/xim.c
    dix/region.c
    libX11/Text16.c
    libX11/ImText.c
    dix/cursor.c
    xkb/xkb.c
    em_x11/keysym_table.c
    em_x11/bridges.c
    em_x11/poll.c
    em_x11/threading.c
    em_x11/signal.c
    em_x11/fork.c
)
set(EM_X11_XEXT_SOURCES       Xext/shape.c)
set(EM_X11_XRENDER_SOURCES    render/render.c)
set(EM_X11_FONTCONFIG_SOURCES em_x11/fontconfig.c)
set(EM_X11_XFT_SOURCES        em_x11/xft.c)
set(EM_X11_GLX_SOURCES        glx/glx.c)

# Combined list used only by the side-module path -- Pyodide dlopens one
# .so, so everything still needs to land in a single artifact there.
set(EM_X11_ALL_SOURCES
    ${EM_X11_X11_SOURCES}
    ${EM_X11_XEXT_SOURCES}
    ${EM_X11_XRENDER_SOURCES}
    ${EM_X11_FONTCONFIG_SOURCES}
    ${EM_X11_XFT_SOURCES}
    ${EM_X11_GLX_SOURCES}
)

# --- Static archives ----------------------------------------------------------

# em_x11_add_static_lib(<target> <output_name> <sources...>)
# -- adds a STATIC library with our standard include dirs, warnings, and
# visibility knobs. Output name drops any prefix so files are libX11.a,
# libXext.a, ... -- exactly what a standard `-l<name>` resolves to.
function(em_x11_add_static_lib target output_name)
    add_library(${target} STATIC ${ARGN})

    target_include_directories(${target}
        PUBLIC  ${EM_X11_PUBLIC_HEADERS_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/em_x11
                ${CMAKE_CURRENT_SOURCE_DIR}/libX11
    )

    target_compile_options(${target} PRIVATE ${EM_X11_WARN_FLAGS} -fPIC)
    if(EM_X11_HIDE_INTERNAL_SYMBOLS)
        target_compile_options(${target} PRIVATE -fvisibility=hidden)
    endif()

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${output_name}"
        PREFIX "lib"
    )
endfunction()

em_x11_add_static_lib(X11        X11        ${EM_X11_X11_SOURCES})
em_x11_add_static_lib(Xext       Xext       ${EM_X11_XEXT_SOURCES})
em_x11_add_static_lib(Xrender    Xrender    ${EM_X11_XRENDER_SOURCES})
em_x11_add_static_lib(fontconfig fontconfig ${EM_X11_FONTCONFIG_SOURCES})
em_x11_add_static_lib(Xft        Xft        ${EM_X11_XFT_SOURCES})
em_x11_add_static_lib(GLX        GLX        ${EM_X11_GLX_SOURCES})

# Inter-archive PUBLIC link so in-tree CMake consumers that just say
# `target_link_libraries(foo PRIVATE Xft)` still get the full transitive
# chain (Xft -> Xrender -> X11). For demos that go through bare
# `-lX11 -lXft -lXrender ...` this is a no-op; the linker handles it.
target_link_libraries(Xext       PUBLIC X11)
target_link_libraries(Xrender    PUBLIC X11)
target_link_libraries(fontconfig PUBLIC X11)
target_link_libraries(Xft        PUBLIC Xrender fontconfig X11)
target_link_libraries(GLX        PUBLIC X11)

# JS-side bridges live in em_x11/bridges.c via EM_JS, so they're embedded in
# libX11.a itself (works under both static link and SIDE_MODULE dlopen).

# --- Side modules (Pyodide path; disabled by default) --------------------------
#
# Real X ships split shared libraries (libX11.so, libXext.so, …) with proper
# NEEDED chains.  We mirror that: each sub-library builds its own SIDE_MODULE.
# Pyodide's dlopen cascades through the NEEDED graph at load time, exactly
# like ld.so does on Linux.

if(EM_X11_BUILD_SIDE_MODULE)

    # em_x11_add_side_module(<target> <output_name> <sources...>)
    # -- adds a SHARED library target for the side-module (dlopen) path with
    # standard include dirs, -fPIC, and -sSIDE_MODULE=1.
    function(em_x11_add_side_module target output_name)
        add_library(${target} SHARED ${ARGN})

        target_include_directories(${target}
            PUBLIC  ${EM_X11_PUBLIC_HEADERS_DIR}
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/em_x11
                    ${CMAKE_CURRENT_SOURCE_DIR}/libX11
        )

        target_compile_options(${target}
            PRIVATE ${EM_X11_WARN_FLAGS}
                    -fPIC
        )

        target_link_options(${target}
            PRIVATE
                "SHELL:-s SIDE_MODULE=1"
                "SHELL:-s ERROR_ON_UNDEFINED_SYMBOLS=0"
        )

        set_target_properties(${target} PROPERTIES
            OUTPUT_NAME "${output_name}"
            SUFFIX ".so"
            PREFIX "lib"
        )
    endfunction()

    # Split side modules, matching the static-archive split and real X's
    # shared-library layout.  The target_link_libraries calls create the
    # NEEDED entries so Pyodide's recursive dlopen auto-cascades.
    #
    #   libX11.so              base, no X deps
    #   libXext.so        NEEDED libX11.so
    #   libXrender.so     NEEDED libX11.so
    #   libfontconfig.so  NEEDED libX11.so
    #   libXft.so         NEEDED libXrender.so libfontconfig.so libX11.so

    em_x11_add_side_module(em_x11_X11_side        X11         ${EM_X11_X11_SOURCES})

    em_x11_add_side_module(em_x11_Xext_side       Xext        ${EM_X11_XEXT_SOURCES})
    target_link_libraries(em_x11_Xext_side PRIVATE em_x11_X11_side)

    em_x11_add_side_module(em_x11_Xrender_side    Xrender     ${EM_X11_XRENDER_SOURCES})
    target_link_libraries(em_x11_Xrender_side PRIVATE em_x11_X11_side)

    em_x11_add_side_module(em_x11_fontconfig_side fontconfig  ${EM_X11_FONTCONFIG_SOURCES})
    target_link_libraries(em_x11_fontconfig_side PRIVATE em_x11_X11_side)

    em_x11_add_side_module(em_x11_Xft_side        Xft         ${EM_X11_XFT_SOURCES})
    target_link_libraries(em_x11_Xft_side
        PRIVATE em_x11_Xrender_side em_x11_fontconfig_side em_x11_X11_side)

    # GLX is deliberately skipped — pyodide-tk doesn't use OpenGL.

    add_custom_target(side-modules ALL
        DEPENDS em_x11_X11_side em_x11_Xext_side em_x11_Xrender_side
                em_x11_fontconfig_side em_x11_Xft_side)

endif()
