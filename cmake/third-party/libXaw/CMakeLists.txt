# libXaw 1.0.16 — Athena Widgets built against em-x11 + libXt.
#
# Like libXt, we skip upstream autotools and drive the build directly.
# Config knobs come from the hand-written config.h next to this file;
# public headers live under include/X11/Xaw.
#
# Warnings are silenced wholesale: upstream Xaw has its own style
# (lots of old-style casts, shadowed locals) that is not worth
# fighting through.

set(LIBXAW_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(LIBXAW_SOURCES
    ${LIBXAW_SRC_DIR}/Actions.c
    ${LIBXAW_SRC_DIR}/AllWidgets.c
    ${LIBXAW_SRC_DIR}/AsciiSink.c
    ${LIBXAW_SRC_DIR}/AsciiSrc.c
    ${LIBXAW_SRC_DIR}/AsciiText.c
    ${LIBXAW_SRC_DIR}/Box.c
    ${LIBXAW_SRC_DIR}/Command.c
    ${LIBXAW_SRC_DIR}/Converters.c
    ${LIBXAW_SRC_DIR}/Dialog.c
    ${LIBXAW_SRC_DIR}/DisplayList.c
    ${LIBXAW_SRC_DIR}/Form.c
    ${LIBXAW_SRC_DIR}/Grip.c
    ${LIBXAW_SRC_DIR}/Label.c
    ${LIBXAW_SRC_DIR}/List.c
    ${LIBXAW_SRC_DIR}/MenuButton.c
    ${LIBXAW_SRC_DIR}/MultiSink.c
    ${LIBXAW_SRC_DIR}/MultiSrc.c
    ${LIBXAW_SRC_DIR}/OS.c
    ${LIBXAW_SRC_DIR}/Paned.c
    ${LIBXAW_SRC_DIR}/Panner.c
    ${LIBXAW_SRC_DIR}/Pixmap.c
    ${LIBXAW_SRC_DIR}/Porthole.c
    ${LIBXAW_SRC_DIR}/Repeater.c
    ${LIBXAW_SRC_DIR}/Scrollbar.c
    ${LIBXAW_SRC_DIR}/Simple.c
    ${LIBXAW_SRC_DIR}/SimpleMenu.c
    ${LIBXAW_SRC_DIR}/Sme.c
    ${LIBXAW_SRC_DIR}/SmeBSB.c
    ${LIBXAW_SRC_DIR}/SmeLine.c
    ${LIBXAW_SRC_DIR}/StripChart.c
    ${LIBXAW_SRC_DIR}/Text.c
    ${LIBXAW_SRC_DIR}/TextAction.c
    ${LIBXAW_SRC_DIR}/TextPop.c
    ${LIBXAW_SRC_DIR}/TextSink.c
    ${LIBXAW_SRC_DIR}/TextSrc.c
    ${LIBXAW_SRC_DIR}/TextTr.c
    ${LIBXAW_SRC_DIR}/Tip.c
    ${LIBXAW_SRC_DIR}/Toggle.c
    ${LIBXAW_SRC_DIR}/Tree.c
    ${LIBXAW_SRC_DIR}/Vendor.c
    ${LIBXAW_SRC_DIR}/Viewport.c
    ${LIBXAW_SRC_DIR}/XawI18n.c
    ${LIBXAW_SRC_DIR}/XawIm.c
    ${LIBXAW_SRC_DIR}/XawInit.c
)

add_library(Xaw STATIC ${LIBXAW_SOURCES})

target_include_directories(Xaw
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}                 # config.h
            ${LIBXAW_SRC_DIR}                           # Private.h, XawI18n.h
)

target_compile_definitions(Xaw
    PRIVATE HAVE_CONFIG_H=1
)

target_compile_options(Xaw
    PRIVATE
        -w
        -fvisibility=hidden
)

target_link_libraries(Xaw PUBLIC Xmu Xpm Xt X11)

set_target_properties(Xaw PROPERTIES OUTPUT_NAME "Xaw")
