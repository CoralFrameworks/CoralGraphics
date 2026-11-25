/* A minimal android/native_window.h, covering only what this translation unit calls. */
#ifndef CORAL_STUB_ANDROID_NATIVE_WINDOW_H
#define CORAL_STUB_ANDROID_NATIVE_WINDOW_H

// `<stdint.h>` rather than `<cstdint>`: the NDK's header is C, so a C compiler has to be able to include it.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque, as it is in the NDK: a caller only ever holds a pointer to one.
 *
 * The NDK's header defines this as a struct with the members laid out for the platform, and nothing outside
 * libandroid touches them. Declaring it opaque here is both closer to how it is used and a better test: a caller that
 * reached into a member would be reaching into something it should not, and would fail here rather than on a device.
 */
typedef struct ANativeWindow ANativeWindow;

/** The window's buffer width in pixels, or a negative value on error. */
int32_t ANativeWindow_getWidth(ANativeWindow* window);

/** The window's buffer height in pixels, or a negative value on error. */
int32_t ANativeWindow_getHeight(ANativeWindow* window);

#ifdef __cplusplus
}
#endif

#endif /* CORAL_STUB_ANDROID_NATIVE_WINDOW_H */
