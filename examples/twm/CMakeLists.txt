# Build recipe for the twm wasm artifact. No standalone demo harness
# lives in this directory (a pure-twm screen is just an empty checker
# with nothing to manage -- not a useful demo); the twm-session demo at
# examples/twm-session/ is the one that boots twm alongside xeyes + xcalc.
# Vite's auto-discovery gates on index.html so the absence here
# correctly hides this as a demo URL.
set(TWM_SRC_DIR "${CMAKE_SOURCE_DIR}/ignored-area/third-party/twm/src")

# Source list mirrors upstream Makefile.am's twm_SOURCES. session.c uses
# ICE/SM which we now build from real X.Org sources, so the session_stub is
# no longer needed.
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
    ${TWM_SRC_DIR}/session.c
    ${TWM_SRC_DIR}/twm.c
    ${TWM_SRC_DIR}/util.c
    # execvp/execv overrides now live in em_x11_libc_override,
    # linked transitively through X11 -> em_x11_libc_override.
)

target_compile_definitions(twm PRIVATE HAVE_CONFIG_H=1)

# config.h sits at third-party/twm/ (one level above src/). SMlib.h
# resolves from native/include/X11/SM/SMlib.h via the X11 PUBLIC include
# path. siconify.bm is included from icons.c with a quoted name so it
# resolves out of the source dir automatically.
target_include_directories(twm PRIVATE
    ${CMAKE_SOURCE_DIR}/ignored-area/third-party/twm
    ${TWM_SRC_DIR}
)

# Third-party code, not held to our warning standard. gram.c / lex.c
# in particular are auto-generated and notoriously noisy.
target_compile_options(twm PRIVATE -w)

# FS is needed for the runtime to stage /em-x11.twmrc into MEMFS via
# preRun before main() reads it (see src/runtime/twm-launch.ts). twm
# also needs a DestroyNotify hook so its frames clean up when a client
# wasm process exits.
em_x11_finalize_demo(twm
    EXPORT_NAME createTwmModule
    LIBS Xmu Xt Xext X11
    EXTRA_FUNCTIONS _em_x11_push_destroy_notify
)
