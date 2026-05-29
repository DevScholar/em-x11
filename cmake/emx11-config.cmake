# em-x11 CMake config — installed alongside the port so downstream
# projects can use find_package(emx11).  Pattern matches SDL2's
# sdl2-config.cmake.
#
# Usage:
#   find_package(emx11 REQUIRED)
#   add_executable(myapp main.c)
#   target_link_libraries(myapp PRIVATE emx11::emx11)

set(EMX11_INCLUDE_DIRS "${EMSCRIPTEN_SYSROOT}/include")
set(EMX11_LIBRARIES "-sUSE_EMX11")

if(NOT TARGET emx11::emx11)
  add_library(emx11::emx11 INTERFACE IMPORTED)
  set_target_properties(emx11::emx11 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${EMX11_INCLUDE_DIRS}"
    INTERFACE_COMPILE_OPTIONS "-sUSE_EMX11"
    INTERFACE_LINK_LIBRARIES "-sUSE_EMX11")

  add_library(emx11::emx11-static INTERFACE IMPORTED)
  set_target_properties(emx11::emx11-static PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${EMX11_INCLUDE_DIRS}"
    INTERFACE_COMPILE_OPTIONS "-sUSE_EMX11"
    INTERFACE_LINK_LIBRARIES "-sUSE_EMX11")
endif()
