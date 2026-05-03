add_executable(xt-hello xt-hello.c)

# Xt's PUBLIC link on emx11_static pulls the Xlib archive in too, but
# list both explicitly for clarity.
target_link_libraries(xt-hello PRIVATE Xt emx11_static)

target_compile_options(xt-hello PRIVATE ${EMX11_WARN_FLAGS})

target_link_options(xt-hello PRIVATE
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME=createXtHelloModule"
    "SHELL:-s ASYNCIFY=1"
    "SHELL:-s ENVIRONMENT=web,worker"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']"
    "SHELL:-s EXPORTED_FUNCTIONS=['_main','_emx11_push_button_event','_emx11_push_motion_event','_emx11_push_key_event','_emx11_push_expose_event','_emx11_push_map_request','_emx11_push_reparent_notify']"
    # Show all undefined symbols during the Xt bring-up, not just 15.
    "SHELL:-Wl,--error-limit=0"
)

set_target_properties(xt-hello PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/xt-hello"
)
