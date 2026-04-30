set(TWM_SRC_DIR "${CMAKE_SOURCE_DIR}/third-party/twm/src")

# Source list mirrors upstream Makefile.am's twm_SOURCES minus session.c,
# which talks ICE/SM. We substitute session_stub.c (dropped into the
# twm source dir by the overlay) in its place.
add_executable(twm
    ${TWM_SRC_DIR}/add_window.c
    ${TWM_SRC_DIR}/cursor.c
    ${TWM_SRC_DIR}/deftwmrc.c
    ${TWM_SRC_DIR}/events.c
    ${TWM_SRC_DIR}/gc.c
    ${TWM_SRC_DIR}/gram.c
    ${TWM_SRC_DIR}/iconmgr.c
    ${TWM_SRC_DIR}/icons.c
    ${TWM_SRC_DIR}/lex.c
    ${TWM_SRC_DIR}/list.c
    ${TWM_SRC_DIR}/menus.c
    ${TWM_SRC_DIR}/parse.c
    ${TWM_SRC_DIR}/resize.c
    ${TWM_SRC_DIR}/session_stub.c
    ${TWM_SRC_DIR}/twm.c
    ${TWM_SRC_DIR}/util.c
)

target_compile_definitions(twm PRIVATE HAVE_CONFIG_H=1)

# config.h sits at third-party/twm/ (one level above src/), and the
# SMlib.h stub at third-party/twm/include/X11/SM/SMlib.h. Both need to
# resolve via quoted "config.h" and <X11/SM/SMlib.h>. siconify.bm is
# included from icons.c with a quoted name so it resolves out of the
# source dir automatically.
target_include_directories(twm PRIVATE
    ${CMAKE_SOURCE_DIR}/third-party/twm
    ${CMAKE_SOURCE_DIR}/third-party/twm/include
    ${TWM_SRC_DIR}
)

target_link_libraries(twm PRIVATE Xmu Xt emx11_static)

# Third-party code, not held to our warning standard. gram.c / lex.c
# in particular are auto-generated and notoriously noisy.
target_compile_options(twm PRIVATE -w)

target_link_options(twm PRIVATE
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME=createTwmModule"
    "SHELL:-s ASYNCIFY=1"
    "SHELL:-s ENVIRONMENT=web"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    # FS is needed for the runtime to stage /em-x11.twmrc into MEMFS via
    # preRun before main() reads it (see src/runtime/twm-launch.ts).
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','FS']"
    "SHELL:-s EXPORTED_FUNCTIONS=['_main','_emx11_push_button_event','_emx11_push_motion_event','_emx11_push_key_event','_emx11_push_expose_event','_emx11_push_map_request','_emx11_push_reparent_notify']"
    "SHELL:-Wl,--error-limit=0"
)

set_target_properties(twm PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/twm"
)
