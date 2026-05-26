# libXmu 1.2.1 — X miscellaneous utility library built against em-x11.
#
# Same pattern as libXt / libXaw: skip upstream autotools, compile the
# .c list directly, use a hand-written config.h. Xaw pulls Xmu in for
# resource converters and drawing helpers.

set(LIBXMU_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(LIBXMU_SOURCES
    ${LIBXMU_SRC_DIR}/AllCmap.c
    ${LIBXMU_SRC_DIR}/Atoms.c
    ${LIBXMU_SRC_DIR}/ClientWin.c
    ${LIBXMU_SRC_DIR}/Clip.c
    ${LIBXMU_SRC_DIR}/CloseHook.c
    ${LIBXMU_SRC_DIR}/CmapAlloc.c
    ${LIBXMU_SRC_DIR}/CrCmap.c
    ${LIBXMU_SRC_DIR}/CrPixFBit.c
    ${LIBXMU_SRC_DIR}/CursorName.c
    ${LIBXMU_SRC_DIR}/CvtCache.c
    ${LIBXMU_SRC_DIR}/CvtStdSel.c
    ${LIBXMU_SRC_DIR}/DefErrMsg.c
    ${LIBXMU_SRC_DIR}/DelCmap.c
    ${LIBXMU_SRC_DIR}/DisplayQue.c
    ${LIBXMU_SRC_DIR}/Distinct.c
    ${LIBXMU_SRC_DIR}/DrRndRect.c
    ${LIBXMU_SRC_DIR}/DrawLogo.c
    ${LIBXMU_SRC_DIR}/EditresCom.c
    ${LIBXMU_SRC_DIR}/ExtAgent.c
    ${LIBXMU_SRC_DIR}/FToCback.c
    ${LIBXMU_SRC_DIR}/GetHost.c
    ${LIBXMU_SRC_DIR}/GrayPixmap.c
    ${LIBXMU_SRC_DIR}/Initer.c
    ${LIBXMU_SRC_DIR}/LocBitmap.c
    ${LIBXMU_SRC_DIR}/Lookup.c
    ${LIBXMU_SRC_DIR}/LookupCmap.c
    ${LIBXMU_SRC_DIR}/Lower.c
    ${LIBXMU_SRC_DIR}/RdBitF.c
    ${LIBXMU_SRC_DIR}/ScrOfWin.c
    ${LIBXMU_SRC_DIR}/ShapeWidg.c
    ${LIBXMU_SRC_DIR}/StdCmap.c
    ${LIBXMU_SRC_DIR}/StrToBS.c
    ${LIBXMU_SRC_DIR}/StrToBmap.c
    ${LIBXMU_SRC_DIR}/StrToCurs.c
    ${LIBXMU_SRC_DIR}/StrToGrav.c
    ${LIBXMU_SRC_DIR}/StrToJust.c
    ${LIBXMU_SRC_DIR}/StrToLong.c
    ${LIBXMU_SRC_DIR}/StrToOrnt.c
    ${LIBXMU_SRC_DIR}/StrToShap.c
    ${LIBXMU_SRC_DIR}/StrToWidg.c
    ${LIBXMU_SRC_DIR}/UpdMapHint.c
    ${LIBXMU_SRC_DIR}/VisCmap.c
    ${LIBXMU_SRC_DIR}/WidgetNode.c
    ${LIBXMU_SRC_DIR}/Xct.c
)

add_library(Xmu STATIC ${LIBXMU_SOURCES})

target_include_directories(Xmu
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/include/X11/Xmu   # quoted "Atoms.h" etc.
            ${LIBXMU_SRC_DIR}
)

target_compile_definitions(Xmu
    PRIVATE HAVE_CONFIG_H=1
)

target_compile_options(Xmu
    PRIVATE
        -w
        -fvisibility=hidden
)

target_link_libraries(Xmu PUBLIC Xt X11)

set_target_properties(Xmu PROPERTIES OUTPUT_NAME "Xmu")
