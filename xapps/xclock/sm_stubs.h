/* SM stubs for em-x11 xclock */
#include <X11/Intrinsic.h>

/* Session management widget class */
#define sessionShellWidgetClass shellWidgetClass

/* SM resource names */
#define XtNjoinSession "joinSession"
#define XtNdieCallback "dieCallback"
#define XtNsaveCallback "saveCallback"

/* SM types */
typedef struct {
    Cardinal save_type;
    int shutdown;
    int fast;
    int dont_show;
    int save_success;
    int interactive;
} XtCheckpointTokenRec;
typedef XtCheckpointTokenRec *XtCheckpointToken;

/* SM function: XtOpenApplication wraps XtAppInitialize */
static inline Widget XtOpenApplication(XtAppContext *app_return,
    String app_class, XrmOptionDescRec *options, Cardinal num_options,
    int *argc, char **argv, String *fallback_resources,
    WidgetClass widget_class, Arg *args, Cardinal num_args)
{
    return XtAppInitialize(app_return, app_class, options, num_options,
        argc, argv, fallback_resources, args, num_args);
}
