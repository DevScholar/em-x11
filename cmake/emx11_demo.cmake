# em-x11 demo helper -- one place for the boilerplate every wasm demo
# needs: where to find libX11.a, the Emscripten link flags shared by
# every demo, and the event-pump function exports the runtime must keep.
#
# Usage from a demo:
#
#   add_executable(myDemo myDemo.c)
#   emx11_finalize_demo(myDemo
#       EXPORT_NAME createMyDemoModule
#       LIBS Xaw Xmu Xt Xpm Xft Xrender fontconfig Xext X11
#   )
#
# Each name in LIBS turns into a literal `-l<name>` on the wasm-ld
# command line -- exactly what an autotools-built X program emits. The
# helper also adds the matching `-L` (pointing at the artifacts dir
# where libX11.a / libXft.a / ... land), and registers a CMake build
# dependency on the same name so the archive is rebuilt before the
# demo tries to link it.
#
# LIBS order matters the same way it does to a regular linker: list the
# higher-level lib first (Xaw -> Xmu -> Xt -> Xpm -> Xft -> Xrender ->
# fontconfig -> Xext -> X11). Symbols in earlier archives reference
# symbols in later ones.

set(EMX11_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/native/include"
    CACHE INTERNAL "em-x11 public header root")
set(EMX11_LIB_DIR "${CMAKE_BINARY_DIR}/artifacts"
    CACHE INTERNAL "em-x11 static archive output dir")

# Hooks the JS runtime reaches into via ccall to deliver host events. If
# we don't pin them with EXPORTED_FUNCTIONS the LTO pass strips them.
set(EMX11_RUNTIME_HOOKS
    _main
    _emx11_push_button_event
    _emx11_push_motion_event
    _emx11_push_key_event
    _emx11_push_key_event_kc
    _emx11_install_keysym
    _emx11_push_expose_event
    _emx11_push_map_request
    _emx11_push_reparent_notify
    _emx11_push_configure_notify
    CACHE INTERNAL "Functions the host JS event router calls into via ccall"
)

# emx11_finalize_demo(<target>
#                     EXPORT_NAME <name>
#                     [LIBS <libname> ...]
#                     [EXTRA_FUNCTIONS <sym> ...]
#                     [EXTRA_RUNTIME_METHODS <name> ...]
#                     [OUTPUT_DIR <dir>])
#
# Adds em-x11's include path / library search dir / per-demo Emscripten
# settings, and emits `-l<libname>` for each entry in LIBS.
function(emx11_finalize_demo target)
    set(options "")
    set(one_value EXPORT_NAME OUTPUT_DIR)
    set(multi_value LIBS EXTRA_FUNCTIONS EXTRA_RUNTIME_METHODS)
    cmake_parse_arguments(EMX11_FD "${options}" "${one_value}" "${multi_value}" ${ARGN})

    if(NOT EMX11_FD_EXPORT_NAME)
        message(FATAL_ERROR "emx11_finalize_demo(${target}): EXPORT_NAME is required")
    endif()

    target_include_directories(${target} PRIVATE ${EMX11_INCLUDE_DIR})
    target_link_directories(${target} PRIVATE ${EMX11_LIB_DIR})

    # Per-library wiring. For each name in LIBS:
    #   - if a CMake target by that name exists (X11, Xt, Xmu, ...),
    #     hand it to target_link_libraries. CMake substitutes
    #     $<TARGET_FILE:foo> -- the absolute archive path -- which
    #     emcc treats as "not a -l flag" and passes through to wasm-ld
    #     unchanged. With proper build-order deps as a bonus.
    #   - if no such target exists, emit `-l<name>`. Reserved only for
    #     libs not in emcc's `map_to_js_libs` hijack list (see
    #     emscripten/tools/link.py:2660 `library_map`). X11 and GL are
    #     ON that list and would be silently eaten + replaced with
    #     emscripten's libxlib.js / libwebgl.js shims -- DON'T pass
    #     them as `-l` here, link the CMake target.
    #
    # The user-facing list still reads as a standard X link line
    # (`LIBS Xaw Xmu Xt Xpm Xft Xrender fontconfig Xext X11`); the
    # helper picks the right CMake plumbing per entry.
    set(_emsdk_provided GL m c dl pthread embind)
    foreach(libname IN LISTS EMX11_FD_LIBS)
        if(TARGET ${libname})
            target_link_libraries(${target} PRIVATE ${libname})
        elseif(libname IN_LIST _emsdk_provided)
            target_link_libraries(${target} PRIVATE "-l${libname}")
        else()
            message(WARNING
                "emx11_finalize_demo(${target}): no CMake target named '${libname}' "
                "and not on the known-emsdk list -- emitting -l${libname} and hoping.")
            target_link_libraries(${target} PRIVATE "-l${libname}")
        endif()
    endforeach()

    set(_runtime_methods ccall cwrap UTF8ToString FS ${EMX11_FD_EXTRA_RUNTIME_METHODS})
    set(_functions ${EMX11_RUNTIME_HOOKS} ${EMX11_FD_EXTRA_FUNCTIONS})

    # Build the ['_a','_b',...] literal Emscripten wants. list(JOIN)
    # handles the commas; we wrap each item in quotes manually.
    set(_methods_quoted "")
    foreach(m IN LISTS _runtime_methods)
        list(APPEND _methods_quoted "'${m}'")
    endforeach()
    list(JOIN _methods_quoted "," _methods_csv)

    set(_functions_quoted "")
    foreach(f IN LISTS _functions)
        list(APPEND _functions_quoted "'${f}'")
    endforeach()
    list(JOIN _functions_quoted "," _functions_csv)

    target_link_options(${target} PRIVATE
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_ES6=1"
        "SHELL:-s EXPORT_NAME=${EMX11_FD_EXPORT_NAME}"
        "SHELL:-s ASYNCIFY=1"
        "SHELL:-s ENVIRONMENT=web,worker"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=[${_methods_csv}]"
        "SHELL:-s EXPORTED_FUNCTIONS=[${_functions_csv}]"
        # Show every undefined symbol when something fails to link, not
        # just the first 15. Wasted minutes early in the Xt bring-up
        # when this default bit us.
        "SHELL:-Wl,--error-limit=0"
    )

    if(NOT EMX11_FD_OUTPUT_DIR)
        set(EMX11_FD_OUTPUT_DIR "${CMAKE_BINARY_DIR}/artifacts/${target}")
    endif()

    set_target_properties(${target} PROPERTIES
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY "${EMX11_FD_OUTPUT_DIR}"
    )
endfunction()
