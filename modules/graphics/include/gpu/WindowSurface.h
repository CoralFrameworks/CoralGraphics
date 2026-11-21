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

#ifndef CORAL_GRAPHICS_GPU_WINDOWSURFACE_H
#define CORAL_GRAPHICS_GPU_WINDOWSURFACE_H

#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>

#include <gpu/ContextGroup.h>
#include <gpu/DrawList.h>

#include <backend/Device.h>

#include <gpu/Types.h>

#include <memory>
#include <vector>
#include <string>

namespace utils {
class JobSystem;
} // namespace utils

namespace graphics::gpu {

/**
 * Renders into a **native window** and presents, instead of rendering offscreen and reading back.
 *
 * ## Why this exists next to `Surface`
 *
 * `Surface` renders into a texture of its own and hands the result back with `snapshot()`, which is the right shape for
 * export, for tests, and for anything that composites the result elsewhere. It is the wrong shape for a preview: showing
 * that texture means a `glReadPixels` (which stalls until the GPU is idle), a CPU copy, and a second upload -- measured at
 * about ten frames a second on the demo scene. A window needs the frame to go **straight** to the display, and that is
 * what this class does: the RHI's swapchain is the target, and `present()` swaps rather than copies.
 *
 * ## Threading
 *
 * Like `Surface`, everything here is thread-affine: the device's context belongs to the thread that created it, so a
 * `WindowSurface` and every `Canvas` it hands out must stay on that one thread. A render loop owns one, on its own thread.
 *
 * ## What the native handle is, per platform
 *
 * | Platform | `nativeWindow` | `nativeDisplay` |
 * |---|---|---|
 * | Android | `ANativeWindow*` | unused |
 * | Windows | `HWND` | its `HDC` |
 * | Linux/X11 | `Window` | `Display*` |
 * | Linux/Wayland | `wl_surface*` (Vulkan); `nativeWindowExtra` is the `wl_egl_window*` for EGL | `wl_display*` |
 * | macOS/OpenGL | `NSView*` (used as the identity of the surface; CGL draws into the context's view) | unused; set `cglContext` |
 * | macOS/Metal, iOS | `CAMetalLayer*` | unused |
 *
 * ## The one limitation worth knowing before choosing this class
 *
 * The window's colour attachment has **no stencil**, so a path fill that needs the stencil-then-cover route is drawn
 * approximately (the renderer counts those in `pathsWithoutStencil`). A document that is mostly paths should render
 * offscreen with `Surface` and present the result; a live preview of photos, video and text is exactly this class's job.
 */
class WindowSurface final {
 public:
  /**
   * The window's render target, as the effect framework needs to see it.
   *
   * `coral::RenderPassChain::run` writes into a target this API names, and for a window that target has no colour texture --
   * only a pass and the default-framebuffer view. Handing these five fields over is how a chain can end at a window
   * instead of at an offscreen texture: `coral::RenderTarget::adoptWindow` takes them and **borrows** them, so the chain
   * never destroys a pass that belongs to this object.
   */
  struct RenderTargetView {
    uint32_t width = 0;
    uint32_t height = 0;
    backend::PixelFormat format = backend::PixelFormat::Undefined;
    backend::RenderPassHandle pass = backend::RenderPassHandle::invalid();
    backend::TextureViewHandle colorView = backend::TextureViewHandle::invalid();
  };
  struct Configuration {
    /** `ANativeWindow*` / `HWND` / `Window` / `wl_surface*` / `CAMetalLayer*`. Required. */
    void* nativeWindow = nullptr;
    /** `HDC` on Windows, `Display*` on Linux. Optional; the backends that need it can derive it from the window. */
    void* nativeDisplay = nullptr;
    /**
     * macOS: the `CGLContextObj` backing an `NSOpenGLContext`.
     *
     * CGL has no window-system call that creates a drawable: the application owns the `NSOpenGLContext` and
     * attaches it to its view, and the backend adopts that context here. The context must be current when the
     * surface is created and must stay on the thread that renders; the demo in `examples/graphics/mac` is the
     * reference caller. Ignored by every other platform.
     */
    void* cglContext = nullptr;
    /**
     * Wayland's second handle: @c nativeWindow is the `wl_surface*` (what Vulkan needs), and this is
     * the `wl_egl_window*` (what EGL needs). Null on every other platform.
     */
    void* nativeWindowExtra = nullptr;

    /**
     * Which native window system the two handles belong to.
     *
     * Linux needs this because X11 and Wayland both pass a `void*` window and display and the backend
     * cannot tell them apart. `Unknown` keeps the X11/GLX path for a window, which is what every
     * existing caller gets.
     */
    backend::NativeWindowSystem nativeWindowSystem = backend::NativeWindowSystem::Unknown;

    /** Required, because the facade allocates its targets to this size before the platform can be asked. */
    uint32_t width = 0;
    uint32_t height = 0;

    /**
     * MSAA sample count for the window's colour attachment; 1 disables it.
     *
     * Vulkan resolves the multisampled attachment into the swapchain image. The request is exact:
     * `make` fails when the backend or device does not support the count. OpenGL windows currently
     * refuse values above 1 because their resolve path targets a texture view rather than the default
     * framebuffer; use `gpu::Surface` for an MSAA offscreen canvas and present its snapshot instead.
     * This is the canvas path; `coral::RenderTarget::adoptWindow` currently assumes a single-sampled
     * window target.
     */
    uint32_t sampleCount = 1;

    /**
     * The group this window's device joins, or null for a device of its own.
     *
     * A window's device must share the group of the devices it exchanges textures with: that is what lets a chain running
     * on one thread sample a texture another thread rendered, with no copy. Create the group first, then its members.
     */
    std::shared_ptr<ContextGroup> group = nullptr;

    /**
     * Worker pool for the CPU stages, or null for single-threaded.
     *
     * This is the highest-leverage setting for a scene with antialiased shapes: coverage-field rasterisation is CPU work and
     * is parallel across shapes. Measured on `tools/probes/field_probe` with the Live demo's scene at 2528x1291: **11.18 ms**
     * single-threaded against **2.86 ms** with a pool -- about 3.9x, on a scene whose whole frame was 12.35 ms.
     *
     * **Borrowed**: the caller keeps it alive for as long as this surface, because the renderer holds the pointer.
     */
    utils::JobSystem* jobSystem = nullptr;

    backend::Backend preferred = backend::Backend::Null;
    backend::PresentMode presentMode = backend::PresentMode::Fifo;

    /**
     * Present the canvas in PQ (HDR10) instead of sRGB.
     *
     * The canvas still draws sRGB-encoded colours; the presentation pass converts them with the BT.2408
     * white point (203 cd/m^2) into a 10-bit PQ swapchain. When the surface offers no HDR10 format the
     * window falls back to sRGB and says so in `diagnostics()` -- refusing to open would be worse than a
     * correct SDR picture. `CORAL_HDR_OUTPUT=1` in the environment sets this, which is how a demo can
     * try it without a new API knob.
     */
    bool hdrOutput = false;

    /** Ask the backend for driver validation and debug output. */
    bool enableValidation = false;

    /** Forwarded every driver message, including the ones that end a failed `make`. */
    void (*diagnosticsCallback)(backend::MessageSeverity severity, const char* message, void* userData) = nullptr;
    void* diagnosticsUserData = nullptr;
  };

  /** @return null when no device could be created for @p configuration, with the reason in `diagnostics()`. */
  [[nodiscard]] static std::unique_ptr<WindowSurface> make(const Configuration& configuration);

  /**
   * Presents to a window through a device that already exists.
   *
   * @discussion
   * A Vulkan device cannot sample a texture another device made, so a presenter that draws the video
   * pipeline's textures into a window has to use the device they live on. This constructor keeps the
   * surface and the swapchain of this object and borrows @p device, which must have been created for
   * @p configuration.nativeWindow and must outlive the surface.
   */
  [[nodiscard]] static std::unique_ptr<WindowSurface> makeOn(backend::Device* device,
    const Configuration& configuration);

  ~WindowSurface();

  WindowSurface(const WindowSurface&) = delete;
  WindowSurface& operator=(const WindowSurface&) = delete;

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;

  /** The format the swapchain actually granted; this is what the window target and its pipelines use. */
  [[nodiscard]] backend::PixelFormat format() const noexcept;

  /** The sample count the window target actually uses; 1 when MSAA is off. */
  [[nodiscard]] uint32_t sampleCount() const noexcept;

  /** The GL implementation, as the backend describes it. */
  [[nodiscard]] std::string describeDevice() const;

  /** The device this window renders through. An effect context must be created from it to share textures with the chain. */
  [[nodiscard]] backend::Device* device() const noexcept;

  /**
   * Transfers ownership of a device created by `make` to the caller.
   *
   * The surface keeps using the device but will no longer delete it; the caller must keep it alive
   * until this surface is destroyed. Returns null for `makeOn` (borrowed) surfaces or after a
   * previous release.
   */
  [[nodiscard]] backend::Device* releaseDevice();

  /** The render target a chain can be run into. Valid until the next `resize` or destruction. */
  /** True when this device adopted a share context from its group, so its textures are the group's. */
  [[nodiscard]] bool sharedWithGroup() const noexcept;

  /**
   * Where the last presented frame's time went, in milliseconds.
   *
   * The breakdown is the renderer's own, and it is what turns "26 fps" into an answerable question: `precompile` and `record`
   * are CPU, `execute` is the driver's work for the frame, and `fields` is coverage-field generation. A frame rate that stays
   * flat when the window is resized is CPU in one of the first two; one that scales with pixels is `execute` or `fields`.
   */
  [[nodiscard]] FrameStages lastFrameStages() const;

  /**
   * Command buffers (GPU render passes) the last present/presentRecordings produced.
   *
   * 0 when the path does not report it. A plan frame wants 1.
   */
  [[nodiscard]] uint32_t lastFrameSubmitCount() const noexcept;
  /** Draw calls and batches of the last presented frame. */
  [[nodiscard]] uint32_t lastFrameDrawCallCount() const noexcept;
  [[nodiscard]] uint32_t lastFrameBatchCount() const noexcept;

  /** GPU time of the last completed frame, in milliseconds; 0 without timestamp queries. */
  [[nodiscard]] double lastFrameGpuMilliseconds() const;

  /** Bytes of texture and buffer memory the device holds live (VRAM residency). */
  [[nodiscard]] uint64_t residentBytes() const;

  /** The present mode this window's swapchain actually got, which may differ from what was asked for. */
  [[nodiscard]] backend::PresentMode presentMode() const noexcept;

  /**
   * The colour space the swapchain presents in.
   *
   * The canvas draws sRGB-encoded values into a UNORM target, so this is `Srgb` today; the surface's
   * wide-gamut/HDR support is reported separately by
   * `Device::getCapabilities().displayP3Swapchain`/`hdr10Swapchain`, which is what an HDR output stage
   * would negotiate against.
   */
  [[nodiscard]] backend::SwapchainColorSpace displayColorSpace() const noexcept;

  [[nodiscard]] RenderTargetView renderTarget() const noexcept;

  /** Why `make` returned null, or why the last frame failed. Empty when nothing went wrong. */
  [[nodiscard]] std::string diagnostics() const;

  /**
   * Opens a frame and returns the canvas to draw it with.
   *
   * The canvas is valid until the matching `present()`. Its target is the swapchain image, so what is drawn reaches the
   * screen when the frame is presented -- there is no read back in this path at all.
   *
   * @return null when a frame is already open, or when the device could not acquire one (a destroyed window, a lost
   *         device); `diagnostics()` then names the reason.
   */
  [[nodiscard]] graphics::Canvas* beginFrame();

  /**
   * Draws a texture another producer owns, between the canvas's own draws.
   *
   * Same contract as `Surface::drawExternalImage`: the view is borrowed for the frame, and the command is
   * appended to the recording `beginFrame` opened, so "background, video, chrome" is call order.
   */
  bool drawExternalImage(const DrawList::ExternalImage& image, const Rect& deviceBounds,
    const Paint& paint);

  /**
   * Submits the frame and presents it.
   *
   * On a `Fifo` swapchain this blocks until the display is ready for a new frame, which is the vertical sync a preview
   * wants and is also what bounds a render loop's rate.
   */
  bool present();

  /**
   * Presents the last canvas recording again without recording it.
   *
   * The swapchain image is acquired normally; only the recording step is skipped. Returns false when
   * there is no retained recording or the device refused the frame.
   */
  bool presentRetained();

  /** Presents a frame that only rendered @p damage into the persistent UI target. */
  bool presentDamage(const Rect& damage);

  /**
   * Presents a display list recorded on another thread.
   *
   * This is the phase-2 UI path: the UI thread records into a `gpu::Recorder` that carries no device
   * context, and the render thread (which owns this surface and its context) submits that list into
   * the persistent UI target, blits it to the acquired swapchain image and presents. The list must
   * stay alive until the call returns; the shared_ptr overload is the safe default.
   *
   * Thread-affine like the rest of this class: call it on the thread that created the surface and
   * owns the device. It performs the acquire, UI-target resize and swapchain recovery that
   * `beginFrame`/`present` would otherwise do.
   */
  bool presentRecording(const std::shared_ptr<const DrawList>& list);

  /**
   * Presents a frame assembled from several lists, in the order given.
   *
   * The first list clears the persistent UI target and the rest load it, so the picture is exactly what
   * drawing them in sequence into one canvas would produce -- without copying them into one. This is the
   * window-platform half of the retained UI's frame plan: the content recorded through `beginFrame` (see
   * `currentRecording`) followed by one list per repaint boundary.
   *
   * @return false when the device refused it or when no list has any drawing.
   */
  bool presentRecordings(const std::vector<std::shared_ptr<const DrawList>>& lists);

  /**
   * One list of a plan-built frame, with the state the frame has to honour for it.
   *
   * A plan frames hands the window a content list plus one list per repaint boundary, and a boundary
   * that is a scrolled viewport carries a translation and a clip. The swapchain path applies them
   * exactly as the offscreen `Surface::submitLists` does.
   */
  struct ListItem {
    std::shared_ptr<const DrawList> list;
    /** Device-pixel clip; an empty rectangle means the whole target. */
    Rect clip{};
    /** Device-pixel translation applied to the list. */
    Point translation{};
    float opacity = 1.0f;
  };

  /**
   * Presents a plan-built frame, each list with its own translation and clip, in one command buffer.
   *
   * The content recorded through `beginFrame` comes first (as `presentRecordings` does), then the
   * items. A whole-pixel translation and any clip are baked into a copy of the item's list so every
   * list of the frame shares one command buffer; an item that cannot be expressed that way -- a
   * fractional translation, a shaped clip mask, an external image -- makes the call return false, and
   * the caller is expected to fold the lists into the content instead of drawing them at the wrong
   * place.
   */
  bool presentListItems(const std::vector<ListItem>& items);

  /**
   * The list `beginFrame` has been recording into, so a caller can hand it back as one of the lists of a
   * multi-list frame. Null when no frame is open.
   */
  [[nodiscard]] std::shared_ptr<const DrawList> currentRecording() const;

  /**
   * Presents a recorded list that only repaints @p damage (device pixels).
   *
   * The persistent UI target keeps its previous content outside @p damage (`LoadOp::Load` plus a
   * scissor), which is what makes a damage-only packet cheap without clearing the page. An empty
   * rectangle is equivalent to the overload above (full repaint, `LoadOp::Clear`).
   */
  bool presentRecording(const std::shared_ptr<const DrawList>& list, const Rect& damage);

  /** Convenience overload for a list the caller keeps alive across the call. */
  bool presentRecording(const DrawList& list);

  /** Copies the persistent UI target into the acquired swapchain image. */
  bool blitToSwapchain(bool waitAcquire);

  /**
   * Reallocates the render target and the swapchain for a new window size.
   *
   * The caller owns the window's events, so it is the caller that calls this: watching for a resize needs a platform event
   * loop, which this layer deliberately does not have.
   */
  bool resize(uint32_t width, uint32_t height);

  /**
   * Re-renders the last presented frame into an offscreen target and reads it back.
   *
   * This is the screenshot path, and it is deliberately a *re-render* rather than a read of the swapchain image: a
   * window's back buffer is not a texture this API can name, and its contents after a swap are undefined. The recording is
   * still available, so drawing it once more into a readable target gives the same picture by construction.
   */
  bool readback(Bitmap* out);

 private:
  WindowSurface();

  bool presentRecordingImpl(const DrawList& list, const Rect* damage = nullptr);

  /** Creates the semaphores, the surface, the swapchain and the window targets of @p device. */
  bool initialize(backend::Device* device, bool ownsDevice, const Configuration& configuration);

  struct Impl;
  std::unique_ptr<Impl> mImpl;
 private:
  bool presentRecordingsImpl(const std::vector<const DrawList*>& lists, const Rect* damage,
    const std::vector<ListItem>* states = nullptr);
};

} // namespace graphics::gpu

#endif // CORAL_GRAPHICS_GPU_WINDOWSURFACE_H

