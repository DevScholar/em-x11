/*
 * xt-hello -- smoke test for the libXt build inside em-x11.
 *
 * Round 1 (prior): just an empty Shell, to prove XtAppInitialize ->
 * XtRealizeWidget -> XtAppMainLoop walks through.
 *
 * Round 2 (now): Shell + one Core child, to exercise the pieces that
 * an empty Shell does not touch -- the geometry manager, child
 * XCreateWindow, parent->child expose propagation, and the Xt event
 * dispatcher's per-widget lookup. The child gets a distinct
 * background so you can see it's a separate window, not just the
 * shell's own background.
 */

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Core.h>
#include <stdio.h>

static void handle_input(Widget w, XtPointer client_data,
                         XEvent *event, Boolean *cont) {
    const char *who = (const char *)client_data;
    (void)w; (void)cont;
    switch (event->type) {
    case ButtonPress:
        printf("xt-hello/%s: ButtonPress button=%u at (%d,%d)\n",
               who, event->xbutton.button,
               event->xbutton.x, event->xbutton.y);
        break;
    case KeyPress:
        printf("xt-hello/%s: KeyPress keycode=%u state=0x%x\n",
               who, event->xkey.keycode, event->xkey.state);
        break;
    case Expose:
        printf("xt-hello/%s: Expose at (%d,%d) %dx%d\n",
               who, event->xexpose.x, event->xexpose.y,
               event->xexpose.width, event->xexpose.height);
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

    /* The child's size drives the shell: Xt's shell grows to fit its
     * single managed child, so we set width/height on the child, not
     * the shell. Background is a strong red so the child window is
     * obviously distinct from the shell frame. */
    Arg child_args[3];
    int n = 0;
    XtSetArg(child_args[n], XtNwidth,  400); n++;
    XtSetArg(child_args[n], XtNheight, 300); n++;
    XtSetArg(child_args[n], XtNbackground, 0xFFCC3333UL); n++;

    Widget pane = XtCreateManagedWidget("pane", coreWidgetClass,
                                        top, child_args, n);
    printf("xt-hello: created child pane=%p\n", (void*)pane); fflush(stdout);

    XtAddEventHandler(top,
                      ButtonPressMask | KeyPressMask,
                      False, handle_input, (XtPointer)"shell");
    XtAddEventHandler(pane,
                      ButtonPressMask | KeyPressMask | ExposureMask,
                      False, handle_input, (XtPointer)"pane");

    printf("xt-hello: calling XtRealizeWidget\n"); fflush(stdout);
    XtRealizeWidget(top);
    printf("xt-hello: shell realized; entering main loop\n"); fflush(stdout);
    XtAppMainLoop(app);
    return 0;
}
