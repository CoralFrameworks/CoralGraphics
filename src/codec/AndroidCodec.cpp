/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(CORAL_BUILD_FOR_ANDROID)

#include <private/graphics/ImageCodecInternal.h>

#include <jni.h>

#include <core/jni/JniScopedRef.h>
#include <core/jni/JvmEnv.h>

#include <android/bitmap.h>

#include <cstdint>
#include <utility>
#include <vector>

#include <minilog/log.h>

/**
 * @file AndroidCodec.cpp
 *
 * Decoding through the platform, which on Android means `BitmapFactory`.
 *
 * ## Why the platform decoder rather than a linked one
 *
 * The shared object already carries libjpeg-turbo and libpng for the desktop builds that need them. On a device those
 * would be a second copy of two libraries the system is already running, in an `.so` that a Play download counts
 * against. The system's decoder also has the hardware paths -- and, on a real device, the formats the platform has
 * licensed decoders for and an open-source build does not.
 *
 * ## What this file can and cannot promise
 *
 * It is compiled only when the NDK is driving the build, and there is no NDK host here, so **nothing in it has been
 * run**. That is worth stating plainly rather than leaving to be discovered: the JNI call sequences are written from
 * the documented contracts, the channel order below is derived rather than measured, and the first device build should
 * treat this as untested. The one part that is checked on the host is that requesting the system backend where it does
 * not exist is refused rather than attempted -- see `isBackendAvailable` and the codec tests.
 *
 * ## Reaching the JVM from native code
 *
 * Through `libs/jni`, which owns the `JavaVM` for the whole project. This file used to carry a `JNI_OnLoad` of its own
 * to capture one, which cannot work: `JNI_OnLoad` belongs to the shared object that the platform loads, and the Android
 * application declares it in `android/coral-android/src/main/cpp/AndroidEnv.cpp`. Two definitions in one `.so` is a
 * duplicate symbol, so this file's copy had to go -- and with it the assumption that graphics is always the thing being
 * loaded, which stops being true the moment graphics is built into an application that has its own entry points.
 *
 * `JNIEnv` is per-thread and a decode can be called from any thread, so the environment is asked for per call rather
 * than cached in a member. `VirtualMachineEnv::getThreadEnvironment` does that and attaches a thread that is not
 * already attached; it reports a null environment when no `JavaVM` was ever registered, which is the case this file
 * turns into an error message rather than a crash.
 */

namespace graphics::codec_internal {
namespace {

/**
 * @return the `BitmapFactory` class, or null when it cannot be found.
 *
 * A local reference, so the caller must release it -- it is not a "well-known" class and there is no cache here,
 * because caching would be a global reference that outlives the JVM's own lifetime rules.
 */
jclass findBitmapFactory(JNIEnv* env) {
  return env->FindClass("android/graphics/BitmapFactory");
}

/** @return the `Bitmap` class, or null. */
jclass findBitmap(JNIEnv* env) {
  return env->FindClass("android/graphics/Bitmap");
}

/**
 * @return the `Config` enum constant for @p name, or null.
 *
 * A local reference like the classes above: the caller owns it, and it is passed straight into a `ScopedLocalObject`
 * which releases it when the scope ends.
 */
jobject findConfig(JNIEnv* env, const char* name) {
  jclass configClass = env->FindClass("android/graphics/Bitmap$Config");
  if (configClass == nullptr) return nullptr;

  const jfieldID field = env->GetStaticFieldID(configClass, name, "Landroid/graphics/Bitmap$Config;");
  jobject value = (field != nullptr) ? env->GetStaticObjectField(configClass, field) : nullptr;
  env->DeleteLocalRef(configClass);
  return value;
}

} // namespace anonymous

std::optional<DecodedImage> decodeWithSystem(const uint8_t* data, const size_t size, const DecodeOptions& options,
        std::string* outError) {
  const auto fail = [outError](const char* message) {
    if (outError != nullptr) *outError = message;
    return std::nullopt;
  };

  if (data == nullptr || size == 0) return fail("the system decoder was given no data");

  // The environment for *this* thread, from the module that owns the `JavaVM`. Null means the virtual machine was never
  // registered -- the library was loaded without the application's `JNI_OnLoad`, or by something that has no JVM.
  JNIEnv* env = JvmEnv::getThreadEnvironment();
  if (env == nullptr) return fail("no JVM is attached to this thread, so the system decoder cannot be reached");

  // --- BitmapFactory.decodeByteArray(bytes, offset, length) ---
  jclass factoryClass = findBitmapFactory(env);
  if (factoryClass == nullptr) return fail("android.graphics.BitmapFactory is not on this class path");
  ScopedLocalClass factory(env, factoryClass);

  const jmethodID decode = env->GetStaticMethodID(factory.obj(), "decodeByteArray",
          "([BII)Landroid/graphics/Bitmap;");
  if (decode == nullptr) return fail("BitmapFactory.decodeByteArray has an unexpected signature");

  // A copy of the input, because the array API has no way to borrow a native buffer. That is the cost of using the
  // platform decoder, and it is bounded by the size of the file being decoded.
  jbyteArray bytes = env->NewByteArray(static_cast<jsize>(size));
  if (bytes == nullptr) return fail("could not allocate the byte array for the encoded image");
  ScopedLocalByteArray byteArray(env, bytes);
  env->SetByteArrayRegion(byteArray.obj(), 0, static_cast<jsize>(size),
          reinterpret_cast<const jbyte*>(data));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return fail("copying the encoded image into a Java array failed");
  }

  jobject bitmap = env->CallStaticObjectMethod(factory.obj(), decode, byteArray.obj(), 0, static_cast<jint>(size));
  if (env->ExceptionCheck() || bitmap == nullptr) {
    // The platform throws for a malformed image. Clearing is required: leaving the exception pending would make the
    // next JNI call on this thread fail for a reason that has nothing to do with what it was asked to do.
    env->ExceptionClear();
    return fail("the platform decoder rejected the image");
  }
  ScopedLocalObject decoded(env, bitmap);

  // --- the bitmap's own description ---
  jclass bitmapClass = findBitmap(env);
  if (bitmapClass == nullptr) return fail("android.graphics.Bitmap is not on this class path");
  ScopedLocalClass bitmapRef(env, bitmapClass);

  const jmethodID getWidth = env->GetMethodID(bitmapRef.obj(), "getWidth", "()I");
  const jmethodID getHeight = env->GetMethodID(bitmapRef.obj(), "getHeight", "()I");
  const jmethodID getConfig = env->GetMethodID(bitmapRef.obj(), "getConfig",
          "()Landroid/graphics/Bitmap$Config;");
  if (getWidth == nullptr || getHeight == nullptr || getConfig == nullptr) {
    return fail("android.graphics.Bitmap does not have the methods this needs");
  }

  const auto width = static_cast<int32_t>(env->CallIntMethod(decoded.obj(), getWidth));
  const auto height = static_cast<int32_t>(env->CallIntMethod(decoded.obj(), getHeight));
  if (width <= 0 || height <= 0) return fail("the platform decoder returned an empty bitmap");

  // Anything that is not already ARGB_8888 is copied into it. A paletted or 565 bitmap can be read directly, but the
  // channel order and the bit packing then differ from case to case, and one extra copy is the cheaper mistake.
  const bool isArgb8888 = [&] {
    ScopedLocalObject config(env, env->CallObjectMethod(decoded.obj(), getConfig));
    if (config.is_null()) return false;
    ScopedLocalObject wanted(env, findConfig(env, "ARGB_8888"));
    if (wanted.is_null()) return false;
    return env->IsSameObject(config.obj(), wanted.obj()) == JNI_TRUE;
  }();

  // The bitmap whose pixels are read: the decoded one, or the converted copy when the format differed. The copy owns
  // its reference for as long as this scope lasts, and `source` is a borrowed handle to whichever of the two it is.
  jobject source = decoded.obj();
  ScopedLocalObject converted(env, static_cast<jobject>(nullptr));
  if (!isArgb8888) {
    ScopedLocalObject wanted(env, findConfig(env, "ARGB_8888"));
    if (wanted.is_null()) return fail("android.graphics.Bitmap$Config.ARGB_8888 is missing");

    const jmethodID copy = env->GetMethodID(bitmapRef.obj(), "copy",
            "(Landroid/graphics/Bitmap$Config;Z)Landroid/graphics/Bitmap;");
    if (copy == nullptr) return fail("Bitmap.copy has an unexpected signature");

    // `false` for mutable: nothing writes to this bitmap, and an immutable one can be shared by the platform. Moving
    // the new reference into `converted` transfers ownership, so there is exactly one owner and no second release.
    converted = ScopedLocalObject(env, env->CallObjectMethod(decoded.obj(), copy, wanted.obj(), JNI_FALSE));
    if (converted.is_null() || env->ExceptionCheck()) {
      env->ExceptionClear();
      return fail("converting the decoded bitmap to ARGB_8888 failed");
    }
    source = converted.obj();
  }

  // --- AndroidBitmap_lockPixels, the only way to see the bytes ---
  AndroidBitmapInfo info {};
  if (AndroidBitmap_getInfo(env, source, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
    return fail("AndroidBitmap_getInfo failed");
  }
  if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
    return fail("the bitmap is not in the RGBA_8888 format this reads");
  }

  void* pixels = nullptr;
  if (AndroidBitmap_lockPixels(env, source, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS || pixels == nullptr) {
    return fail("AndroidBitmap_lockPixels failed");
  }

  // ## The channel order, derived rather than assumed
  //
  // Android packs ARGB_8888 into a `uint32_t` as `0xAARRGGBB`, and the pixel buffer is that word in the machine's byte
  // order -- little-endian on every device this runs on. So the bytes in memory are
  //
  //     byte 0 = blue, byte 1 = green, byte 2 = red, byte 3 = alpha
  //
  // which is BGRA, not RGBA despite the format's name. Reading it as RGBA would swap red and blue, which is the kind
  // of mistake that looks like a colour-space problem rather than an indexing one.
  //
  // `stride` is in bytes and is not always `width * 4`: the platform is free to pad rows, and assuming otherwise
  // produces an image that shears progressively down its height.
  //
  // Both of those are now the job of `argb8888ToRgba`, which is compiled on every platform so that the tests can reach
  // it. Leaving it inline here would put the only real logic in this file beyond the reach of anything but a device, and
  // the two mistakes it is prone to -- the channel order and the stride -- are exactly the ones a type-check cannot see.
  DecodedImage image;
  image.info = ImageInfo(width, height, ColorType::kRGBA_8888, AlphaType::kPremultiplied);
  image.pixels = argb8888ToRgba(static_cast<const uint8_t*>(pixels), width, height, info.stride);
  if (image.pixels.empty()) {
    AndroidBitmap_unlockPixels(env, source);
    return fail("the decoded bitmap describes a buffer that cannot be laid out");
  }

  AndroidBitmap_unlockPixels(env, source);

  (void) options;
  return image;
}

} // namespace graphics::codec_internal

#endif // CORAL_BUILD_FOR_ANDROID
