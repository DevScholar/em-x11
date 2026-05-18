add_executable(hello hello.c)

emx11_finalize_demo(hello
    EXPORT_NAME createHelloModule
    LIBS X11
)

target_compile_options(hello PRIVATE ${EMX11_WARN_FLAGS})
