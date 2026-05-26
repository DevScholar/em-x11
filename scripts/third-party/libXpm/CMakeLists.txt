# libXpm 3.5.17 — XPM pixmap library built against em-x11.
#
# Pulled in because libXaw's Pixmap.c registers an XPM loader. The
# zpipe path (gzip decompression via a subprocess) is disabled in
# our config.h since wasm cannot spawn processes.

set(LIBXPM_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(LIBXPM_SOURCES
    ${LIBXPM_SRC_DIR}/Attrib.c
    ${LIBXPM_SRC_DIR}/CrBufFrI.c
    ${LIBXPM_SRC_DIR}/CrBufFrP.c
    ${LIBXPM_SRC_DIR}/CrDatFrI.c
    ${LIBXPM_SRC_DIR}/CrDatFrP.c
    ${LIBXPM_SRC_DIR}/CrIFrBuf.c
    ${LIBXPM_SRC_DIR}/CrIFrDat.c
    ${LIBXPM_SRC_DIR}/CrIFrP.c
    ${LIBXPM_SRC_DIR}/CrPFrBuf.c
    ${LIBXPM_SRC_DIR}/CrPFrDat.c
    ${LIBXPM_SRC_DIR}/CrPFrI.c
    ${LIBXPM_SRC_DIR}/Image.c
    ${LIBXPM_SRC_DIR}/Info.c
    ${LIBXPM_SRC_DIR}/RdFToBuf.c
    ${LIBXPM_SRC_DIR}/RdFToDat.c
    ${LIBXPM_SRC_DIR}/RdFToI.c
    ${LIBXPM_SRC_DIR}/RdFToP.c
    ${LIBXPM_SRC_DIR}/WrFFrBuf.c
    ${LIBXPM_SRC_DIR}/WrFFrDat.c
    ${LIBXPM_SRC_DIR}/WrFFrI.c
    ${LIBXPM_SRC_DIR}/WrFFrP.c
    ${LIBXPM_SRC_DIR}/create.c
    ${LIBXPM_SRC_DIR}/data.c
    ${LIBXPM_SRC_DIR}/hashtab.c
    ${LIBXPM_SRC_DIR}/misc.c
    ${LIBXPM_SRC_DIR}/parse.c
    ${LIBXPM_SRC_DIR}/rgb.c
    ${LIBXPM_SRC_DIR}/scan.c
)

add_library(Xpm STATIC ${LIBXPM_SOURCES})

target_include_directories(Xpm
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include/X11       # quoted "xpm.h" in XpmI.h
            ${LIBXPM_SRC_DIR}
)

target_compile_definitions(Xpm
    PRIVATE HAVE_CONFIG_H=1
)

target_compile_options(Xpm
    PRIVATE
        -w
        -fvisibility=hidden
)

target_link_libraries(Xpm PUBLIC X11)

set_target_properties(Xpm PROPERTIES OUTPUT_NAME "Xpm")
