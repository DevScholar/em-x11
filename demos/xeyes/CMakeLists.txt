set(XEYES_SRC_DIR "${CMAKE_SOURCE_DIR}/third-party/xeyes")

add_executable(xeyes
    ${XEYES_SRC_DIR}/xeyes.c
    ${XEYES_SRC_DIR}/Eyes.c
    ${XEYES_SRC_DIR}/transform.c
)

target_compile_definitions(xeyes PRIVATE HAVE_CONFIG_H=1)

# The upstream sources reach for quoted "Eyes.h", "EyesP.h", "transform.h",
# "eyes.bit", "eyesmask.bit", and "config.h" -- all of which live in the
# third-party tree (config.h is overlaid by scripts/fetch-third-party.sh).
target_include_directories(xeyes PRIVATE ${XEYES_SRC_DIR})

# xeyes uses Xt + Xmu (for CvtStringToBackingStore) + the shape stub that
# lives inside emx11.
target_link_libraries(xeyes PRIVATE Xmu Xt emx11_static)

# Upstream xeyes is -Wall clean on a real toolchain but uses a handful of
# patterns our strict warnings trip on (unused fn args in XtActionsRec
# callbacks, etc.). Don't treat third-party code as our own.
target_compile_options(xeyes PRIVATE -w)

target_link_options(xeyes PRIVATE
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME=createXeyesModule"
    "SHELL:-s ASYNCIFY=1"
    "SHELL:-s ENVIRONMENT=web"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']"
    "SHELL:-s EXPORTED_FUNCTIONS=['_main','_emx11_push_button_event','_emx11_push_motion_event','_emx11_push_key_event','_emx11_push_expose_event','_emx11_push_map_request']"
    "SHELL:-Wl,--error-limit=0"
)

set_target_properties(xeyes PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/xeyes"
)
