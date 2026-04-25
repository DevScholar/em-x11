/*
 * xaw-hello -- smoke test for the libXaw build on top of em-x11.
 *
 * Minimal Athena-widget demo: Shell -> Label. If this links and
 * paints, it proves Xaw's widget-class registration, resource
 * converters, font/text drawing, and Xmu atom bootstrap all land on
 * top of our Xlib + Xt stack without a real X server.
 *
 * Next step after this passes is Command (Button) so we can exercise
 * Xaw's input path, then Box/Form to sanity-check the geometry
 * managers.
 */

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Xaw/Label.h>
#include <stdio.h>

int main(int argc, char **argv) {
    printf("xaw-hello: main entered\n"); fflush(stdout);

    XtAppContext app;
    Widget top = XtAppInitialize(&app, "XawHello",
                                 NULL, 0,
                                 &argc, argv,
                                 NULL, NULL, 0);
    if (!top) {
        fprintf(stderr, "xaw-hello: XtAppInitialize failed\n");
        return 1;
    }

    Arg args[4];
    int n = 0;
    XtSetArg(args[n], XtNlabel, "Hello, Athena!"); n++;
    XtSetArg(args[n], XtNwidth,  300); n++;
    XtSetArg(args[n], XtNheight, 120); n++;
    XtSetArg(args[n], XtNbackground, 0xFFEEEEEEUL); n++;

    Widget label = XtCreateManagedWidget("label", labelWidgetClass,
                                         top, args, n);
    (void)label;

    XtRealizeWidget(top);
    printf("xaw-hello: realized; entering main loop\n"); fflush(stdout);
    XtAppMainLoop(app);
    return 0;
}
