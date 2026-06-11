# em-x11 demo helper — one place for the boilerplate every wasm demo
# needs: MODULARIZE + EXPORT_ES6 + JSPI + event-pump exports + the
# emscripten-ports link so libraries resolve without the -lX11 hijack.
#
# Usage from a demo:
#
#   add_executable(myDemo myDemo.c)
#   emx11_finalize_demo(myDemo
#       EXPORT_NAME createMyDemoModule
#       LIBS Xaw Xmu Xt Xpm Xft Xrender fontconfig Xext X11
#   )
#
# Linking goes through the emscripten-ports script at
# tools/ports/emx11.py. The port finds the pre-built static archives
# in build/artifacts/ and returns their full filesystem paths to emcc,
# bypassing emscripten's map_to_js_libs hijack.
#
# LIBS is informational — it documents the link order for readers but
# doesn't control linking (the port script owns the full archive list).
# Each named lib that has a CMake target is added as a build-order
# dependency so archives are ready before the demo links.

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
#                     [PRELOAD_FILES <src@target> ...]
#                     [OUTPUT_DIR <dir>])
#
# Wires a demo executable for the em-x11 runtime: include path, port-based
# linking, Emscripten flags, and output location.
function(emx11_finalize_demo target)
    set(options "")
    set(one_value EXPORT_NAME OUTPUT_DIR HI_DPI)
    set(multi_value LIBS EXTRA_FUNCTIONS EXTRA_RUNTIME_METHODS PRELOAD_FILES)
    cmake_parse_arguments(EMX11_FD "${options}" "${one_value}" "${multi_value}" ${ARGN})

    if(NOT EMX11_FD_EXPORT_NAME)
        message(FATAL_ERROR "emx11_finalize_demo(${target}): EXPORT_NAME is required")
    endif()

    target_include_directories(${target} PRIVATE ${EMX11_INCLUDE_DIR})

    # Port-based linking. emcc loads the port script which returns the
    # full archive paths, avoiding the -lX11 -> libxlib.js hijack.
    # Only pass --use-port at link time — at compile time the port's
    # process_args() may return linker-only flags (--js-library,
    # --pre-js) that clang doesn't understand.
    set(_port_script "${CMAKE_SOURCE_DIR}/tools/ports/emx11.py")
    target_link_options(${target} PRIVATE
        "SHELL:--use-port=${_port_script}"
    )

    # Inject the JS bridge library so C EM_JS calls resolve at link time.
    # In the static-link path this overrides the EM_JS bodies in bridges.c.
    set(_js_lib "${CMAKE_SOURCE_DIR}/native/src/lib/library_emx11.js")
    if(EXISTS "${_js_lib}")
        target_link_options(${target} PRIVATE "SHELL:--js-library=${_js_lib}")
    endif()

    # HI_DPI compile option: controls devicePixelRatio scaling on the root
    # canvas.  Defaults to ON (HiDPI enabled).  Set HI_DPI OFF to revert to
    # the 1:1 backing store — useful when non-integer DPR (Windows
    # 125%/150%) causes antialiasing artifacts at window edges.
    # The cmake function injects a pre-js that sets
    # Module['emx11HiDpi']=false before the default Host IIFE runs, and
    # passes -DEMX11_HI_DPI=0 so C code can check the value.
    if(EMX11_FD_HI_DPI STREQUAL "OFF")
        target_compile_definitions(${target} PRIVATE EMX11_HI_DPI=0)
        set(_hi_dpi_pre "${CMAKE_SOURCE_DIR}/native/src/lib/hi-dpi-pre.js")
        if(EXISTS "${_hi_dpi_pre}")
            target_link_options(${target} PRIVATE "SHELL:--pre-js=${_hi_dpi_pre}")
        endif()
    endif()

    # Inject the default Host IIFE so Layer 1 (zero-JS) mode auto-creates
    # a Host on Module.canvas without user JS.  Set Module['emx11NoAutoStart']
    # to true if you want to provide your own Host via createEmX11.
    set(_host_bundle "${CMAKE_BINARY_DIR}/artifacts/emx11-default-host.js")
    if(EXISTS "${_host_bundle}")
        target_link_options(${target} PRIVATE "SHELL:--pre-js=${_host_bundle}")
    endif()

    # --preload-file embeds files into a .data package that Emscripten's
    # glue loads automatically before main(). Each entry is a <src>@<target>
    # pair where src is a build-machine path and target is the MEMFS path.
    foreach(pf IN LISTS EMX11_FD_PRELOAD_FILES)
        target_link_options(${target} PRIVATE "SHELL:--preload-file ${pf}")
    endforeach()

    # Propagate include paths from each named library target so headers
    # like <X11/Intrinsic.h> (from Xt) and <X11/Xaw/Command.h> (from Xaw)
    # resolve. add_dependencies ensures archives are built before the demo
    # links; the port script owns the actual link line.
    foreach(libname IN LISTS EMX11_FD_LIBS)
        if(TARGET ${libname})
            target_include_directories(${target} PRIVATE
                $<TARGET_PROPERTY:${libname},INTERFACE_INCLUDE_DIRECTORIES>
            )
            add_dependencies(${target} ${libname})
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
        "SHELL:-s JSPI=1"
        "SHELL:-s ENVIRONMENT=web,worker"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=[${_methods_csv}]"
        "SHELL:-s EXPORTED_FUNCTIONS=[${_functions_csv}]"
        # Show every undefined symbol when something fails to link, not
        # just the first 15. Wasted minutes early in the Xt bring-up
        # when this default bit us.
        "SHELL:-Wl,--error-limit=0"
        # poll.c pushback buffer: __wrap_read intercepts read() on non-
        # Display fds so poll() can return consumed bytes. fork.c vfork:
        # __wrap__exit longjmps back to the parent instead of killing the
        # wasm module. Both are defined in libX11.a; --wrap redirects
        # every call site to the wrapper, which chains to __real_*.
        "SHELL:-Wl,--wrap=read"
        "SHELL:-Wl,--wrap=_exit"
    )

    if(NOT EMX11_FD_OUTPUT_DIR)
        set(EMX11_FD_OUTPUT_DIR "${CMAKE_BINARY_DIR}/artifacts/${target}")
    endif()

    set_target_properties(${target} PROPERTIES
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY "${EMX11_FD_OUTPUT_DIR}"
    )
endfunction()
