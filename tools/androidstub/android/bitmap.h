/* A minimal android/bitmap.h, covering only what this translation unit calls. */
#ifndef CORAL_STUB_ANDROID_BITMAP_H
#define CORAL_STUB_ANDROID_BITMAP_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AndroidBitmapInfo {
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  int32_t format;
  uint32_t flags;
} AndroidBitmapInfo;

enum {
  ANDROID_BITMAP_FORMAT_NONE = 0,
  ANDROID_BITMAP_FORMAT_RGBA_8888 = 1,
  ANDROID_BITMAP_FORMAT_RGB_565 = 4,
};

#define ANDROID_BITMAP_RESULT_SUCCESS 0
#define ANDROID_BITMAP_RESULT_BAD_PARAMETER (-1)
#define ANDROID_BITMAP_RESULT_JNI_EXCEPTION (-2)
#define ANDROID_BITMAP_RESULT_ALLOCATION_FAILED (-3)

int32_t AndroidBitmap_getInfo(void* env, void* bitmap, AndroidBitmapInfo* info);
int32_t AndroidBitmap_lockPixels(void* env, void* bitmap, void** addr);
int32_t AndroidBitmap_unlockPixels(void* env, void* bitmap);

#ifdef __cplusplus
}
#endif

#endif
