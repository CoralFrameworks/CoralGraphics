/* A minimal OpenGL/CGLTypes.h, covering only what this translation unit calls. */
#ifndef CORAL_STUB_CGLTYPES_H
#define CORAL_STUB_CGLTYPES_H

/*
 * CGL's handle types, all opaque.
 *
 * The real header declares them as pointers to incomplete structs, and nothing outside the framework looks inside one.
 * Reproducing that matters here rather than using `void*`: this file casts between `void*` and `CGLContextObj` in both
 * directions, and a `void*` typedef would make those casts no-ops that could hide a genuine argument mix-up.
 */
typedef struct _CGLContextObject* CGLContextObj;
typedef struct _CGLPixelFormatObject* CGLPixelFormatObj;
typedef struct _CGLPBufferObject* CGLPBufferObj;
typedef struct _CGLRendererInfoObject* CGLRendererInfoObj;

/** An attribute in a pixel-format request: either a key or one of its values, which is why it is one type. */
typedef unsigned int CGLPixelFormatAttribute;

/**
 * The framework's error type.
 *
 * `kCGLNoError` is zero and every failure is non-zero, which is what lets a call site compare against it directly.
 * The two failures this file distinguishes are no-error and a bad drawable; the rest exist because a switch over an
 * enum is easier to read than a comparison against a single value.
 */
typedef enum {
  kCGLNoError = 0,
  kCGLBadAttribute = 10000,
  kCGLBadProperty = 10001,
  kCGLBadPixelFormat = 10002,
  kCGLBadRendererInfo = 10003,
  kCGLBadContext = 10004,
  kCGLBadDrawable = 10005,
  kCGLBadDisplay = 10006,
  kCGLBadState = 10007,
  kCGLBadValue = 10008,
  kCGLBadMatch = 10009,
  kCGLBadEnumeration = 10010,
  kCGLBadOffScreen = 10011,
  kCGLBadFullScreen = 10012,
  kCGLBadWindow = 10013,
  kCGLBadAddress = 10014,
  kCGLBadCodeModule = 10015,
  kCGLBadAlloc = 10016,
  kCGLBadConnection = 10017,
} CGLError;

#endif /* CORAL_STUB_CGLTYPES_H */
