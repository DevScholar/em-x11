# xorgproto 2025.1 — X11 protocol headers (header-only).
#
# Provides the canonical upstream X11 protocol definitions that
# third-party X libraries (libICE, libSM) expect. em_x11's own Xlib
# headers take precedence for client-side APIs; this package only
# supplies the transport and protocol-level headers that em_x11
# doesn't implement (Xtrans).

add_library(xorgproto INTERFACE)
target_include_directories(xorgproto INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
