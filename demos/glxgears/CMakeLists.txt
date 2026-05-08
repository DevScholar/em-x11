set(GLXGEARS_SRC_DIR "${CMAKE_SOURCE_DIR}/third-party/glxgears")

add_executable(glxgears
    ${GLXGEARS_SRC_DIR}/glxgears.c
    gl_displaylist.c
)

# Pure Xlib + GLX, no Xt/Xmu/Xaw. Pull GLX from emx11_static (which
# now exports the glX* symbols from src/glx.c) and the GL fixed-pipeline
# stubs from emscripten's libGL via -lGL + LEGACY_GL_EMULATION.
target_link_libraries(glxgears PRIVATE emx11_static)

# Upstream is -Wall clean on a real toolchain but our strict flags
# (-Wall -Wextra) trip on a few patterns.
target_compile_options(glxgears PRIVATE -w)

target_link_options(glxgears PRIVATE
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME=createGlxgearsModule"
    # ASYNCIFY: the patched event_loop yields once per drawn frame via
    # emscripten_sleep(0). Without ASYNCIFY the wasm thread holds the
    # JS event loop forever and nothing paints.
    "SHELL:-s ASYNCIFY=1"
    # GL: bring in emscripten's libGL with full fixed-function pipeline
    # emulation (glBegin / glRotatef / glLight / glMaterial / matrix
    # stack), running on WebGL1. WebGL2 is incompatible with
    # LEGACY_GL_EMULATION's underlying shader emitter; we explicitly
    # opt out to keep the legacy pipeline path active.
    "SHELL:-s LEGACY_GL_EMULATION=1"
    "SHELL:-s FULL_ES2=0"
    "SHELL:-s USE_WEBGL2=0"
    "SHELL:-s GL_FFP_ONLY=1"
    # Display-list shim (gl_displaylist.c): emscripten's
    # LEGACY_GL_EMULATION omits glGenLists/glNewList/.../glCallList,
    # so we record-and-replay around them. --wrap= redirects calls
    # to glBegin / glEnd / glVertex3f / glNormal3f / glShadeModel
    # into __wrap_*; the originals stay reachable as __real_*.
    "SHELL:-Wl,--wrap=glBegin"
    "SHELL:-Wl,--wrap=glEnd"
    "SHELL:-Wl,--wrap=glVertex3f"
    "SHELL:-Wl,--wrap=glNormal3f"
    "SHELL:-Wl,--wrap=glShadeModel"
    "SHELL:-Wl,--wrap=glMaterialfv"
    "SHELL:-s ENVIRONMENT=web,worker"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToUTF8','specialHTMLTargets']"
    "SHELL:-s EXPORTED_FUNCTIONS=['_main','_emx11_push_button_event','_emx11_push_motion_event','_emx11_push_key_event','_emx11_push_expose_event','_emx11_push_map_request','_emx11_push_reparent_notify','_emx11_push_configure_notify']"
    "SHELL:-Wl,--error-limit=0"
)

set_target_properties(glxgears PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/glxgears"
)
