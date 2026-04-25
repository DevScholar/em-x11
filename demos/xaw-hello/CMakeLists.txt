add_executable(xaw-hello xaw-hello.c)

# Link order matters: Xaw depends on Xmu+Xpm+Xt, Xt on emx11.
target_link_libraries(xaw-hello PRIVATE Xaw Xmu Xpm Xt emx11_static)

target_compile_options(xaw-hello PRIVATE ${EMX11_WARN_FLAGS})

target_link_options(xaw-hello PRIVATE
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME=createXawHelloModule"
    "SHELL:-s ASYNCIFY=1"
    "SHELL:-s ENVIRONMENT=web"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']"
    "SHELL:-s EXPORTED_FUNCTIONS=['_main','_emx11_push_button_event','_emx11_push_motion_event','_emx11_push_key_event','_emx11_push_expose_event','_emx11_push_map_request']"
    "SHELL:-Wl,--error-limit=0"
)

set_target_properties(xaw-hello PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/xaw-hello"
)
