add_executable(hello hello.c)

em_x11_finalize_demo(hello
    EXPORT_NAME createHelloModule
    LIBS X11
    EXTRA_FUNCTIONS _tick
)

target_link_options(hello PRIVATE
    "SHELL:-s JSPI_EXPORTS='tick,main,__main_argc_argv'"
)

target_compile_options(hello PRIVATE ${EM_X11_WARN_FLAGS})
