add_executable(hello hello.c)

target_link_libraries(hello PRIVATE emx11_static)

target_compile_options(hello PRIVATE ${EMX11_WARN_FLAGS})

# Emscripten-specific link settings:
#   MODULARIZE + EXPORT_ES6 -> TS `import()`-able factory
#   ASYNCIFY                -> XNextEvent can emscripten_sleep without
#                              restructuring client code
#   EXPORTED_RUNTIME_METHODS -> ccall/cwrap for event bridge injection
#   EXPORTED_FUNCTIONS       -> keep our event-push hooks alive
target_link_options(hello PRIVATE
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME=createHelloModule"
    "SHELL:-s ASYNCIFY=1"
    "SHELL:-s ENVIRONMENT=web"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']"
    "SHELL:-s EXPORTED_FUNCTIONS=['_main','_emx11_push_button_event','_emx11_push_motion_event','_emx11_push_key_event','_emx11_push_expose_event','_emx11_push_map_request','_emx11_push_reparent_notify']"
)

set_target_properties(hello PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/hello"
)
