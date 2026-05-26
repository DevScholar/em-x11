add_executable(xt-hello xt-hello.c)

emx11_finalize_demo(xt-hello
    EXPORT_NAME createXtHelloModule
    LIBS Xt X11
)

target_compile_options(xt-hello PRIVATE ${EMX11_WARN_FLAGS})
