set(XCALC_SRC_DIR "${CMAKE_SOURCE_DIR}/third-party/xcalc")

# xcalc is an Xaw-based scientific calculator. Upstream ships a flat
# tarball (see scripts/fetch-third-party.sh layout=app) so all sources
# sit at the root of third-party/xcalc/.
add_executable(xcalc
    ${XCALC_SRC_DIR}/xcalc.c
    ${XCALC_SRC_DIR}/actions.c
    ${XCALC_SRC_DIR}/math.c
)

target_compile_definitions(xcalc PRIVATE HAVE_CONFIG_H=1)

# xcalc's .c files use quoted "xcalc.h" and "actions.h" from the same dir;
# config.h is overlaid at the root of third-party/xcalc/.
target_include_directories(xcalc PRIVATE ${XCALC_SRC_DIR})

target_link_libraries(xcalc PRIVATE Xaw Xmu Xpm Xt emx11_static)

# Third-party code: opt out of our strict warnings.
target_compile_options(xcalc PRIVATE -w)

target_link_options(xcalc PRIVATE
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME=createXcalcModule"
    "SHELL:-s ASYNCIFY=1"
    "SHELL:-s ENVIRONMENT=web"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    # FS needed so the runtime can stage /usr/lib/X11/app-defaults/XCalc into
    # MEMFS via preRun before Xt's XtResolvePathname looks for it. Without
    # app-defaults, xcalc's button layout collapses to widget defaults and
    # the UI is unreadable.
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','FS']"
    "SHELL:-s EXPORTED_FUNCTIONS=['_main','_emx11_push_button_event','_emx11_push_motion_event','_emx11_push_key_event','_emx11_push_expose_event','_emx11_push_map_request']"
    "SHELL:-Wl,--error-limit=0"
)

set_target_properties(xcalc PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/xcalc"
)
