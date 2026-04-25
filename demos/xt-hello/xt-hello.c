/*
 * xt-hello -- smoke test for the libXt build inside em-x11.
 *
 * Purpose: prove that Xt links against our emx11_static and its startup
 * path (XtAppInitialize -> XtRealizeWidget -> XtAppMainLoop) walks
 * through without hitting an unresolved Xlib symbol or an assertion
 * inside Xt. We deliberately use ONLY the Intrinsics -- no Xaw/Xmu --
 * so the test is a pure Xlib<->Xt boundary check.
 *
 * Expected visual: an X-managed top-level shell window at the
 * compositor's default position. No widgets inside, just the shell
 * background. Clicks and keys are printed via Xt event handlers so you
 * can confirm the Xt dispatcher is seeing Xlib events we push.
 */

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <stdio.h>

static void handle_input(Widget w, XtPointer client_data,
                         XEvent *event, Boolean *cont) {
    (void)w; (void)client_data; (void)cont;
    switch (event->type) {
    case ButtonPress:
        printf("xt-hello: ButtonPress button=%u at (%d,%d)\n",
               event->xbutton.button, event->xbutton.x, event->xbutton.y);
        break;
    case KeyPress:
        printf("xt-hello: KeyPress keycode=%u state=0x%x\n",
               event->xkey.keycode, event->xkey.state);
        break;
    default:
        break;
    }
}

int main(int argc, char **argv) {
    printf("xt-hello: main entered\n"); fflush(stdout);

    XtAppContext app;
    printf("xt-hello: calling XtAppInitialize\n"); fflush(stdout);
    Widget top = XtAppInitialize(&app, "XtHello",
                                 NULL, 0,
                                 &argc, argv,
                                 NULL, NULL, 0);
    printf("xt-hello: XtAppInitialize returned %p\n", (void*)top); fflush(stdout);
    if (!top) {
        fprintf(stderr, "xt-hello: XtAppInitialize failed\n");
        return 1;
    }

    /* Shell widgets derive their size from children (via the geometry
     * manager) or from XtNwidth/XtNheight resources. We have no
     * children, so we set both explicitly -- otherwise Xt issues a
     * fatal "zero width and/or height" on realize. */
    Arg args[2];
    XtSetArg(args[0], XtNwidth,  400);
    XtSetArg(args[1], XtNheight, 300);
    XtSetValues(top, args, 2);

    XtAddEventHandler(top,
                      ButtonPressMask | KeyPressMask,
                      False, handle_input, NULL);

    printf("xt-hello: calling XtRealizeWidget\n"); fflush(stdout);
    XtRealizeWidget(top);
    printf("xt-hello: shell realized; entering main loop\n"); fflush(stdout);
    XtAppMainLoop(app);
    return 0;
}
