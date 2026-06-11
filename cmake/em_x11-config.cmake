# em-x11 CMake config — installed alongside the port so downstream
# projects can use find_package(em_x11).  Pattern matches SDL2's
# sdl2-config.cmake.
#
# Usage:
#   find_package(em_x11 REQUIRED)
#   add_executable(myapp main.c)
#   target_link_libraries(myapp PRIVATE em_x11::em_x11)

set(EM_X11_INCLUDE_DIRS "${EMSCRIPTEN_SYSROOT}/include")
set(EM_X11_LIBRARIES "-sUSE_EM_X11")

if(NOT TARGET em_x11::em_x11)
  add_library(em_x11::em_x11 INTERFACE IMPORTED)
  set_target_properties(em_x11::em_x11 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${EM_X11_INCLUDE_DIRS}"
    INTERFACE_COMPILE_OPTIONS "-sUSE_EM_X11"
    INTERFACE_LINK_LIBRARIES "-sUSE_EM_X11")

  add_library(em_x11::em-x11-static INTERFACE IMPORTED)
  set_target_properties(em_x11::em-x11-static PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${EM_X11_INCLUDE_DIRS}"
    INTERFACE_COMPILE_OPTIONS "-sUSE_EM_X11"
    INTERFACE_LINK_LIBRARIES "-sUSE_EM_X11")
endif()
