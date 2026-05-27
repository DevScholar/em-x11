# libSM 1.2.6 — X Session Management library built against em-x11.
#
# Depends on libICE for transport. On wasm SM connections always fail
# (no session manager in the browser), so Xt falls back gracefully.

set(LIBSM_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(LIBSM_SOURCES
    ${LIBSM_SRC_DIR}/sm_auth.c
    ${LIBSM_SRC_DIR}/sm_client.c
    ${LIBSM_SRC_DIR}/sm_error.c
    ${LIBSM_SRC_DIR}/sm_genid.c
    ${LIBSM_SRC_DIR}/sm_manager.c
    ${LIBSM_SRC_DIR}/sm_misc.c
    ${LIBSM_SRC_DIR}/sm_process.c
)

add_library(SM STATIC ${LIBSM_SOURCES})

target_include_directories(SM
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include/X11/SM
            ${LIBSM_SRC_DIR}
)

target_compile_definitions(SM
    PRIVATE HAVE_CONFIG_H=1
)

target_compile_options(SM
    PRIVATE
        -w
        -fvisibility=hidden
)

target_link_libraries(SM PUBLIC ICE xorgproto)

set_target_properties(SM PROPERTIES OUTPUT_NAME "SM")
