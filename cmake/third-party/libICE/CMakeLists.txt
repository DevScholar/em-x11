# libICE 1.1.2 — X Inter-Client Exchange library.
#
# SM transport layer. On wasm there is no real IPC; all socket ops
# are emscripten stubs that fail gracefully. SM falls back to "no
# session manager" at runtime, which is fine — SM is optional in Xt.
#
# Depends on xorgproto for Xtrans and base protocol headers, not on
# em_x11's Xlib (libICE doesn't use the X11 client API).

set(LIBICE_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(LIBICE_SOURCES
    ${LIBICE_SRC_DIR}/accept.c
    ${LIBICE_SRC_DIR}/authutil.c
    ${LIBICE_SRC_DIR}/connect.c
    ${LIBICE_SRC_DIR}/error.c
    ${LIBICE_SRC_DIR}/getauth.c
    ${LIBICE_SRC_DIR}/iceauth.c
    ${LIBICE_SRC_DIR}/icetrans.c
    ${LIBICE_SRC_DIR}/listen.c
    ${LIBICE_SRC_DIR}/listenwk.c
    ${LIBICE_SRC_DIR}/locking.c
    ${LIBICE_SRC_DIR}/misc.c
    ${LIBICE_SRC_DIR}/ping.c
    ${LIBICE_SRC_DIR}/process.c
    ${LIBICE_SRC_DIR}/protosetup.c
    ${LIBICE_SRC_DIR}/register.c
    ${LIBICE_SRC_DIR}/replywait.c
    ${LIBICE_SRC_DIR}/setauth.c
    ${LIBICE_SRC_DIR}/shutdown.c
    ${LIBICE_SRC_DIR}/watch.c
)

add_library(ICE STATIC ${LIBICE_SOURCES})

target_include_directories(ICE
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include/X11/ICE
            ${LIBICE_SRC_DIR}
)

target_compile_definitions(ICE
    PRIVATE HAVE_CONFIG_H=1
            ICE_t=ICE_TCP_Trans  # config.h fallback; any value works, transport fails at runtime
)

target_compile_options(ICE
    PRIVATE
        -w
        -fvisibility=hidden
)

target_link_libraries(ICE PUBLIC xtrans xorgproto)

set_target_properties(ICE PROPERTIES OUTPUT_NAME "ICE")
