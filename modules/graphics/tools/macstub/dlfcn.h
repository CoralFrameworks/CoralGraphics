/* A minimal dlfcn.h, covering only what this translation unit calls.
 *
 * On Linux this is `libdl`, which `GlPlatformGlx.cpp` uses to reach GL 1.1 entry points that `libGL` exports directly
 * rather than through `glXGetProcAddressARB`. Since glibc 2.34 `dlopen` and `dlsym` live in libc itself, so no extra
 * link line is needed and none is added.
 */
#ifndef CORAL_STUB_DLFCN_H
#define CORAL_STUB_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

/** Resolve symbols as they are used rather than when the object is loaded. */
#define RTLD_LAZY 0x00001
/** Do not make the object's symbols available to anything loaded afterwards. */
#define RTLD_LOCAL 0x00000

void* dlopen(const char* fileName, int flags);

void* dlsym(void* handle, const char* symbol);

char* dlerror(void);

int dlclose(void* handle);

#ifdef __cplusplus
}
#endif

#endif /* CORAL_STUB_DLFCN_H */
