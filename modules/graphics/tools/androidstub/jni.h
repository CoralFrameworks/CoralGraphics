/*
 * A minimal jni.h, enough to compile a JNI caller on a host without the NDK. Not the NDK's header.
 *
 * The functions are members of `JNIEnv` and `JavaVM`, which is how the NDK's own C++ jni.h presents them. The real
 * entry points take the environment explicitly, so each member hides that behind a file-scope pointer -- which is
 * exactly what the NDK does, and is why a call site reads `env->FindClass("...")` with no environment argument.
 *
 * Matching that shape is what makes this check worth running: a call with the wrong arguments, or to a name that does
 * not exist, fails here exactly as it would on a device.
 */
#ifndef CORAL_STUB_JNI_H
#define CORAL_STUB_JNI_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>

typedef int32_t jint;
typedef int64_t jlong;
typedef uint8_t jboolean;
typedef int8_t jbyte;
typedef jbyte* jbyteArray;
typedef void* jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef jobject jthrowable;
typedef jobject jarray;
typedef jobject jfieldID;
typedef jobject jmethodID;
typedef jint jsize;

struct JNIEnv;
struct JavaVM;

typedef struct JavaVMAttachArgs {
  jint version;
  char* name;
  jobject group;
} JavaVMAttachArgs;

#define JNI_VERSION_1_6 0x00010006
#define JNI_OK 0
#define JNI_ERR (-1)
#define JNI_EDETACHED (-2)
#define JNI_FALSE 0
#define JNI_TRUE 1

namespace coral_jni_stub {
extern jint (*fnGetVersion)(JNIEnv*);
extern jclass (*fnFindClass)(JNIEnv*, const char*);
extern jmethodID (*fnGetMethodID)(JNIEnv*, jclass, const char*, const char*);
extern jmethodID (*fnGetStaticMethodID)(JNIEnv*, jclass, const char*, const char*);
extern jfieldID (*fnGetStaticFieldID)(JNIEnv*, jclass, const char*, const char*);
extern jobject (*fnGetStaticObjectField)(JNIEnv*, jclass, jfieldID);
extern jobject (*fnCallObjectMethod)(JNIEnv*, jobject, jmethodID, ...);
extern jobject (*fnCallStaticObjectMethod)(JNIEnv*, jclass, jmethodID, ...);
extern jint (*fnCallIntMethod)(JNIEnv*, jobject, jmethodID, ...);
extern jbyteArray (*fnNewByteArray)(JNIEnv*, jsize);
extern void (*fnSetByteArrayRegion)(JNIEnv*, jbyteArray, jsize, jsize, const jbyte*);
extern jsize (*fnGetArrayLength)(JNIEnv*, jarray);
extern jboolean (*fnIsSameObject)(JNIEnv*, jobject, jobject);
extern jboolean (*fnExceptionCheck)(JNIEnv*);
extern void (*fnExceptionClear)(JNIEnv*);
extern void (*fnDeleteLocalRef)(JNIEnv*, jobject);
extern jobject (*fnNewLocalRef)(JNIEnv*, jobject);
extern jobject (*fnNewGlobalRef)(JNIEnv*, jobject);
extern void (*fnDeleteGlobalRef)(JNIEnv*, jobject);
extern jint (*fnGetEnv)(JavaVM*, void**, jint);
extern jint (*fnAttachCurrentThread)(JavaVM*, JNIEnv**, void*);
extern jint (*fnDetachCurrentThread)(JavaVM*);
} // namespace coral_jni_stub

struct JNIEnv {
  jint GetVersion() { return coral_jni_stub::fnGetVersion(this); }
  jclass FindClass(const char* name) { return coral_jni_stub::fnFindClass(this, name); }
  jmethodID GetMethodID(jclass clazz, const char* name, const char* sig) {
    return coral_jni_stub::fnGetMethodID(this, clazz, name, sig);
  }
  jmethodID GetStaticMethodID(jclass clazz, const char* name, const char* sig) {
    return coral_jni_stub::fnGetStaticMethodID(this, clazz, name, sig);
  }
  jfieldID GetStaticFieldID(jclass clazz, const char* name, const char* sig) {
    return coral_jni_stub::fnGetStaticFieldID(this, clazz, name, sig);
  }
  jobject GetStaticObjectField(jclass clazz, jfieldID field) {
    return coral_jni_stub::fnGetStaticObjectField(this, clazz, field);
  }
  jobject CallObjectMethod(jobject object, jmethodID method, ...) {
    return coral_jni_stub::fnCallObjectMethod(this, object, method);
  }
  jobject CallStaticObjectMethod(jclass clazz, jmethodID method, ...) {
    return coral_jni_stub::fnCallStaticObjectMethod(this, clazz, method);
  }
  jint CallIntMethod(jobject object, jmethodID method, ...) {
    return coral_jni_stub::fnCallIntMethod(this, object, method);
  }
  jbyteArray NewByteArray(jsize length) { return coral_jni_stub::fnNewByteArray(this, length); }
  void SetByteArrayRegion(jbyteArray array, jsize start, jsize len, const jbyte* buffer) {
    coral_jni_stub::fnSetByteArrayRegion(this, array, start, len, buffer);
  }
  jsize GetArrayLength(jarray array) { return coral_jni_stub::fnGetArrayLength(this, array); }
  jboolean IsSameObject(jobject a, jobject b) { return coral_jni_stub::fnIsSameObject(this, a, b); }
  jboolean ExceptionCheck() { return coral_jni_stub::fnExceptionCheck(this); }
  void ExceptionClear() { coral_jni_stub::fnExceptionClear(this); }
  void DeleteLocalRef(jobject object) { coral_jni_stub::fnDeleteLocalRef(this, object); }
  // The three below are what `libs/jni`'s scoped references are built on: `JavaLocalRef` takes a second reference to
  // an object it did not create, and `JavaGlobalRef` promotes one out of the local frame.
  jobject NewLocalRef(jobject object) { return coral_jni_stub::fnNewLocalRef(this, object); }
  jobject NewGlobalRef(jobject object) { return coral_jni_stub::fnNewGlobalRef(this, object); }
  void DeleteGlobalRef(jobject object) { coral_jni_stub::fnDeleteGlobalRef(this, object); }
};

struct JavaVM {
  jint GetEnv(void** env, jint version) { return coral_jni_stub::fnGetEnv(this, env, version); }

  // ## Two forms of AttachCurrentThread, because there are two jni.h shapes
  //
  // The NDK's C++ header declares the environment as `JNIEnv**`; a JDK's declares it as `void**`. Which one a caller
  // compiles against is decided by `__ANDROID__`, and this check runs on an MSVC host where that macro is not defined --
  // so `libs/jni`'s `VirtualMachineEnv::getThreadEnvironment` takes the `void**` branch here even though the sources
  // being checked are the Android ones. Accepting both is what lets the same header serve the check and stay faithful
  // to the NDK's shape.
  jint AttachCurrentThread(JNIEnv** env, void* args) {
    return coral_jni_stub::fnAttachCurrentThread(this, env, args);
  }
  jint AttachCurrentThread(void** env, void* args) {
    return coral_jni_stub::fnAttachCurrentThread(this, reinterpret_cast<JNIEnv**>(env), args);
  }

  jint DetachCurrentThread() { return coral_jni_stub::fnDetachCurrentThread(this); }
};

#define JNIEXPORT __declspec(dllexport)
#define JNICALL __stdcall

#endif
