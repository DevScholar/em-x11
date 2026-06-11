set(GLXGEARS_SRC_DIR "${CMAKE_SOURCE_DIR}/ignored-area/third-party/glxgears")

add_executable(glxgears
    ${GLXGEARS_SRC_DIR}/glxgears.c
    gl_displaylist.c
)

# Upstream is -Wall clean on a real toolchain but our strict flags
# (-Wall -Wextra) trip on a few patterns.
target_compile_options(glxgears PRIVATE -w)

# Pure Xlib + GLX. -lGLX pulls our glX* symbols; -lGL is the emscripten
# system lib that ships glBegin / glRotatef / matrix stack via
# LEGACY_GL_EMULATION (turned on below).
em_x11_finalize_demo(glxgears
    EXPORT_NAME createGlxgearsModule
    LIBS GLX X11 GL
    EXTRA_RUNTIME_METHODS stringToUTF8 specialHTMLTargets
)

target_link_options(glxgears PRIVATE
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
)
