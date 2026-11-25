/* A minimal X11/Xlib.h, covering only what this translation unit calls. */
#ifndef CORAL_STUB_X11_XLIB_H
#define CORAL_STUB_X11_XLIB_H

#include <cstddef>

/* Xlib's own types. `Display` is opaque -- a caller holds a pointer and never looks inside one -- and the numeric
 * handles are unsigned longs on every platform Xlib supports, which is why they are spelled out rather than `int`. */
typedef struct _XDisplay Display;

typedef unsigned long Window;
typedef unsigned long Colormap;
typedef unsigned long XID;
typedef unsigned long XPointer;

/** Xlib's boolean, which is an int rather than bool: several entry points return it as a status, so making it a `bool`
 *  would be a different type from the one the real header declares. */
typedef int Bool;

/*
 * `None` is deliberately not defined by this header.
 *
 * `GlPlatformGlx.cpp` does `#undef None` immediately after including `<X11/Xlib.h>`, and that `#undef` runs *after* this
 * file -- so anything defined here is gone by the time the file uses it. On Linux the same thing happens with the real
 * Xlib, and `None` reaches the attribute lists from `<GL/glx.h>` instead. Defining it in the GLX stub is therefore both
 * what the real headers do and the only placement that survives.
 */

/* Where in `XSetWindowAttributes` each value-mask bit selects a member. */
#define CWOverrideRedirect (1L << 9)
#define CWEventMask (1L << 11)
#define CWColormap (1L << 13)

/** Event masks. Only the one the helper window subscribes to. */
#define StructureNotifyMask (1L << 17)

/** Colormap allocation: entries are allocated as they are needed. */
#define AllocNone 0

/** A window's class. */
#define InputOutput 1

/**
 * A visual, opaque.
 *
 * In Xlib this is `struct Visual`, whose members a caller may read. Declaring it incomplete here is closer to what this
 * file needs and is the stricter test: a caller that reached into a member would be relying on Xlib's layout, which is
 * the kind of thing that differs between platforms.
 */
typedef struct _XVisual Visual;

typedef struct {
  Colormap colormap;
  unsigned long event_mask;
} XSetWindowAttributes;

typedef struct {
  int width;
  int height;
  int x;
  int y;
} XWindowAttributes;

/**
 * The event an Xlib error handler is given.
 *
 * Only `error_code` is read by the caller; the rest of the fields are Xlib's own event layout and are
 * present so that a translation unit reading another field fails here for the same reason it would
 * matter on Linux. The real header declares them as a struct, not an opaque handle, because a handler
 * inspects `request_code` to tell a GLX protocol error from an unrelated one.
 */
typedef struct {
  int type;
  Display* display;
  XID resourceid;
  unsigned long serial;
  unsigned char error_code;
  unsigned char request_code;
  unsigned char minor_code;
} XErrorEvent;

/** Xlib's error handler signature, and the handler installed before the caller's. */
typedef int (*XErrorHandler)(Display* display, XErrorEvent* event);

#ifdef __cplusplus
extern "C" {
#endif

Display* XOpenDisplay(const char* displayName);
int XCloseDisplay(Display* display);

Colormap XCreateColormap(Display* display, Window window, Visual* visual, int alloc);
int XFreeColormap(Display* display, Colormap colormap);

Window XCreateWindow(Display* display, Window parent, int x, int y, unsigned int width, unsigned int height,
        unsigned int borderWidth, int depth, unsigned int windowClass, Visual* visual, unsigned long valuemask,
        XSetWindowAttributes* attributes);

int XGetWindowAttributes(Display* display, Window window, XWindowAttributes* attributes);

int XFree(void* data);

/** Installs a process-global error handler and returns the previous one. */
XErrorHandler XSetErrorHandler(XErrorHandler handler);

/** Flushes the output buffer and waits for the server to process it; `discard` drops queued events. */
int XSync(Display* display, Bool discard);

#ifdef __cplusplus
}
#endif

/*
 * The macros a caller uses to reach the screen's defaults.
 *
 * These are macros in Xlib because they index into a `struct Display` the caller is not meant to name, so they are macros
 * here too rather than inline functions: a stub that made them functions would accept a call with the wrong argument
 * count, which the real header rejects. They expand to expressions of the type the real ones return -- a `Visual*`, a
 * `Window`, an `int` -- so that a caller's own casts behave as they would on Linux.
 */
#define DefaultScreen(dpy) 0
#define DefaultVisual(dpy, screen) ((Visual*)0)
#define DefaultDepth(dpy, screen) 24
#define RootWindow(dpy, screen) ((Window)0)

#endif /* CORAL_STUB_X11_XLIB_H */
