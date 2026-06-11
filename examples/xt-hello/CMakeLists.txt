add_executable(xt-hello xt-hello.c)

em_x11_finalize_demo(xt-hello
    EXPORT_NAME createXtHelloModule
    LIBS Xt X11
)

target_compile_options(xt-hello PRIVATE ${EM_X11_WARN_FLAGS})
