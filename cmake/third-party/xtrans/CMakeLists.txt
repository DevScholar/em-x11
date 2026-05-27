# xtrans 1.6.0 — X11 transport abstraction.
#
# xtrans source files live directly in the root (no src/ include/
# split). The canonical public include path is <X11/Xtrans/Xtrans.h>,
# so we create the expected directory layout at configure time.
#
# On wasm all socket transports fail at runtime; libICE → SM → Xt
# degrades gracefully.

# Create the canonical include path for <X11/Xtrans/Xtrans.h>.
set(XTRANS_INCLUDE_STAGING ${CMAKE_CURRENT_SOURCE_DIR}/include/X11/Xtrans)
file(MAKE_DIRECTORY ${XTRANS_INCLUDE_STAGING})
# libICE etc. include <X11/Xtrans/transport.c> — xtrans exposes its
# .c files as includable sources so consumers can inline them.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/Xtrans.h
          ${CMAKE_CURRENT_SOURCE_DIR}/Xtransint.h
          ${CMAKE_CURRENT_SOURCE_DIR}/Xtrans.c
          ${CMAKE_CURRENT_SOURCE_DIR}/Xtranslcl.c
          ${CMAKE_CURRENT_SOURCE_DIR}/Xtranssock.c
          ${CMAKE_CURRENT_SOURCE_DIR}/Xtransutil.c
          ${CMAKE_CURRENT_SOURCE_DIR}/transport.c
     DESTINATION ${XTRANS_INCLUDE_STAGING})

# xtrans uses a unity-build: transport.c #includes Xtransint.h then
# pulls in every other .c file. Only transport.c is compiled directly.
set(XTRANS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/transport.c
)

add_library(xtrans STATIC ${XTRANS_SOURCES})

target_include_directories(xtrans
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(xtrans
    PRIVATE HAVE_CONFIG_H=1
            TRANS_CLIENT
            TRANS_SERVER
)

target_compile_options(xtrans
    PRIVATE
        -w
        -fvisibility=hidden
)

target_link_libraries(xtrans PUBLIC xorgproto)

set_target_properties(xtrans PROPERTIES OUTPUT_NAME "xtrans")
