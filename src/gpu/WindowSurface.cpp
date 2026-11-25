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

#include <gpu/WindowSurface.h>

#include <algorithm>

#include <gpu/Recorder.h>

#include "ExternalImageRecord.h"

#include <private/gpu/ContextImpl.h>
#include <private/gpu/ListBake.h>

#include <backend/RHI.h>

#include <minilog/log.h>

namespace graphics {

/** The images a recording references, indexed by `CommandState::image` (see `GraphiteCanvasBackend.cpp`). */
[[nodiscard]] const std::vector<Bitmap>& recordedImages(const gpu::DrawList* list);

} // namespace graphics

namespace graphics::gpu {

struct WindowSurface::Impl {
  std::shared_ptr<ContextImpl> context;
  /** The group this device joined, so its destruction can be accounted for. */
  std::shared_ptr<ContextGroup> group;
  backend::Device* device = nullptr;
  /** True when this object created the device and therefore must destroy it. */
  bool ownsDevice = false;

  backend::SurfaceHandle surface = backend::SurfaceHandle::invalid();
  backend::SwapchainHandle swapchain = backend::SwapchainHandle::invalid();
  backend::SemaphoreHandle acquireSemaphore = backend::SemaphoreHandle::invalid();
  backend::SemaphoreHandle renderFinishedSemaphore = backend::SemaphoreHandle::invalid();

  /** The window target: its colour attachment is the swapchain image, named by the default-framebuffer view. */
  std::vector<RenderTarget> targets;
  uint32_t activeTarget = 0;
  bool hasFrame = false;
  RenderTarget target;
  /** The readable offscreen target `readback` re-renders into, created on first use. */
  RenderTarget scratch;

  std::shared_ptr<DrawList> recording;
  /** The last display list handed to `presentRecording`; `readback` re-renders it. */
  std::shared_ptr<const DrawList> lastExternalRecording;
  std::shared_ptr<graphics::Canvas> canvas;
  /** The last canvas recording and its images, replayed by `presentRetained`. */
  std::vector<Bitmap> retainedImages;
  bool retainedValid = false;
  /** The UI renders into this sampled target; the swapchain image is a blit of it. */
  RenderTarget uiTarget;
  RenderTarget swapchainTarget;

  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sampleCount = 1;
  /** Command buffers the last present produced; a plan frame lends several lists, one each today. */
  uint32_t lastSubmitCount = 0;
  backend::PresentMode presentMode = backend::PresentMode::Fifo;
  /**
   * True when this frame's drawable came from a display link rather than from `beginFrame`.
   *
   * The acquire semaphore must not be waited on such a frame: no acquire command ran, because the
   * drawable was already handed over. Waiting on a semaphore nobody signalled would block the first
   * frame forever -- the failure this flag exists to make impossible.
   */
  bool displayLinkFrame = false;
  /** True when the targets carry PQ numbers; drives the presentation pass and every recreated target. */
  bool pqOutput = false;
  /** What the caller asked the swapchain for; the granted value is `colorSpace`. */
  backend::SwapchainColorSpace requestedColorSpace = backend::SwapchainColorSpace::Srgb;
  /** The colour space the swapchain actually granted. */
  backend::SwapchainColorSpace colorSpace = backend::SwapchainColorSpace::Srgb;
  /** Diffuse white for the PQ/EDR presentation, in cd/m^2. */
  float sdrWhiteNits = 203.0f;
  /** The linear 16F target between the canvas and the display encoding; only for a managed output. */
  RenderTarget linearTarget;
  bool frameOpen = false;
  /** True when this device adopted a share context from its group. */
  bool sharedWithGroup = false;
  std::string diagnostics;

  /** The semaphore a submission must wait on, or an invalid one when the frame was display-link driven. */
  [[nodiscard]] backend::SemaphoreHandle acquireWait() const noexcept {
    return displayLinkFrame ? backend::SemaphoreHandle::invalid() : acquireSemaphore;
  }
};

WindowSurface::WindowSurface() : mImpl(std::make_unique<Impl>()) {}

WindowSurface::~WindowSurface() {
  if (mImpl == nullptr) return;

  // ## The order here is load-bearing, and getting it wrong is a use-after-free
  //
  // The context owns the renderer, and the renderer holds the device it was initialised with. Deleting the device while
  // the context is still alive leaves the renderer pointing at freed memory -- which is exactly what the first version of
  // this destructor did, and the crash landed in `~GpuRenderer` rather than here. So: idle, release the targets, drop the
  // context (which destroys the renderer), and only then release the RHI objects and the device this object created.
  // ## The canvas goes first, and that is the whole bug this destructor had
  //
  // The `Canvas` a `beginFrame` hands out holds a `shared_ptr` to the same `ContextImpl`, so resetting the context while a
  // canvas is alive only drops one reference: the renderer is *not* destroyed here, the device is deleted afterwards, and when
  // the canvas finally goes the renderer's destructor calls `waitIdle` on a freed device. Measured as an access violation in
  // `~GpuRenderer`, and only on the path where a frame was opened and never presented.
  mImpl->canvas.reset();
  mImpl->recording.reset();

  // Drop this surface's current binding before the context/device go away. A GLX context left
  // current on the UI thread made the next Wayland window's EGL surface fail to acquire a frame.
  if (mImpl->device != nullptr) {
    (void)mImpl->device->releaseCurrent();
  }

  if (mImpl->context != nullptr) {
    mImpl->context->waitIdle();
    if (mImpl->acquireSemaphore.valid()) mImpl->device->destroySemaphore(mImpl->acquireSemaphore);
    if (mImpl->renderFinishedSemaphore.valid()) mImpl->device->destroySemaphore(mImpl->renderFinishedSemaphore);
    mImpl->acquireSemaphore = backend::SemaphoreHandle::invalid();
    mImpl->renderFinishedSemaphore = backend::SemaphoreHandle::invalid();
    if (mImpl->scratch.isValid()) mImpl->context->destroyRenderTarget(mImpl->scratch);
    if (mImpl->linearTarget.isValid()) mImpl->context->destroyRenderTarget(mImpl->linearTarget);
    for (RenderTarget& slot: mImpl->targets) mImpl->context->destroyRenderTarget(slot);
    mImpl->targets.clear();
    mImpl->target = RenderTarget {};
    mImpl->scratch = RenderTarget {};
    mImpl->target = RenderTarget {};
    mImpl->context.reset();
  }
  if (mImpl->device != nullptr) {
    if (mImpl->swapchain.valid()) mImpl->device->destroySwapchain(mImpl->swapchain);
    if (mImpl->surface.valid()) mImpl->device->destroySurface(mImpl->surface);
    if (mImpl->ownsDevice) delete mImpl->device;
    mImpl->device = nullptr;
  }
  if (mImpl->group != nullptr) {
    mImpl->group->unregisterDevice();
    mImpl->group.reset();
  }
}

std::unique_ptr<WindowSurface> WindowSurface::make(const Configuration& configuration) {
  if (configuration.nativeWindow == nullptr || configuration.width == 0 || configuration.height == 0) {
    return nullptr;
  }

  auto surface = std::unique_ptr<WindowSurface>(new WindowSurface());
  Impl& impl = *surface->mImpl;

  // ## The window is handed to the *device*, not to a render target
  //
  // The backend creates its context against this surface, which is what makes the default framebuffer exist at all: an
  // offscreen context has none, and a pass onto it would be a pass onto nothing. Everything after this point reuses the
  // RHI's swapchain -- `createSwapchain`, `beginFrame`, `endFrame` -- rather than a second presentation path.
  backend::SurfaceDescriptor descriptor;
  descriptor.nativeWindow = configuration.nativeWindow;
  descriptor.nativeDisplay = configuration.nativeDisplay;
  descriptor.nativeWindowExtra = configuration.nativeWindowExtra;
  descriptor.nativeWindowSystem = configuration.nativeWindowSystem;
  descriptor.cglContext = configuration.cglContext;
  descriptor.width = configuration.width;
  descriptor.height = configuration.height;
  descriptor.presentMode = configuration.presentMode;

  backend::DeviceConfiguration deviceConfiguration;
  deviceConfiguration.preferredBackend = configuration.preferred;
  deviceConfiguration.allowFallback = true;
  deviceConfiguration.appName = "coral-window";
  deviceConfiguration.surface = &descriptor;
  deviceConfiguration.enableValidation = configuration.enableValidation;
  // The share context has to be named before the device exists: EGL and GLX take it at creation, WGL as the first call after.
  if (configuration.group != nullptr) {
    deviceConfiguration.shareContext = configuration.group->shareContext();
  }

  backend::Device* device = nullptr;
  const backend::Result created = backend::createDevice(deviceConfiguration, &device);
  if (!backend::isSuccess(created) || device == nullptr) {
    impl.diagnostics = "no device could be created for this window";
    return nullptr;
  }

  impl.device = device;
  impl.ownsDevice = true;
  impl.sharedWithGroup = deviceConfiguration.shareContext != nullptr;
  impl.group = configuration.group;
  if (configuration.group != nullptr) {
    // Registration is what makes the *first* device the root the others share with; a device joining a group that already has
    // one is simply counted. The backend is taken from the device because the factory may have fallen back to another one.
    if (!configuration.group->registerDevice(device->nativeContext(), device->getInfo().backend)) {
      impl.diagnostics = "the context group belongs to a different backend";
      return nullptr;
    }
  }
  // A demo can try HDR without a new API knob; the configuration field remains the product-facing one.
  WindowSurface::Configuration effective = configuration;
  if (std::getenv("CORAL_HDR_OUTPUT") != nullptr) effective.hdrOutput = true;
  if (effective.hdrOutput && effective.outputColorSpace == backend::SwapchainColorSpace::Srgb) {
    effective.outputColorSpace = backend::SwapchainColorSpace::Hdr10Pq;
  }
  if (!surface->initialize(device, true, effective)) {
    return nullptr;
  }
  return surface;
}

std::unique_ptr<WindowSurface> WindowSurface::makeOn(backend::Device* device, const Configuration& configuration) {
  if (device == nullptr || configuration.nativeWindow == nullptr || configuration.width == 0 ||
      configuration.height == 0) {
    return nullptr;
  }
  WindowSurface::Configuration effective = configuration;
  if (effective.hdrOutput && effective.outputColorSpace == backend::SwapchainColorSpace::Srgb) {
    effective.outputColorSpace = backend::SwapchainColorSpace::Hdr10Pq;
  }
  auto surface = std::unique_ptr<WindowSurface>(new WindowSurface());
  if (!surface->initialize(device, false, effective)) {
    return nullptr;
  }
  return surface;
}

bool WindowSurface::initialize(backend::Device* device, const bool ownsDevice, const Configuration& configuration) {
  Impl& impl = *mImpl;
  impl.device = device;
  impl.ownsDevice = ownsDevice;
  // The surface names the window once; everything after it reuses the RHI's swapchain.
  backend::SurfaceDescriptor descriptor;
  descriptor.nativeWindow = configuration.nativeWindow;
  descriptor.nativeDisplay = configuration.nativeDisplay;
  descriptor.nativeWindowExtra = configuration.nativeWindowExtra;
  descriptor.nativeWindowSystem = configuration.nativeWindowSystem;
  descriptor.cglContext = configuration.cglContext;
  descriptor.width = configuration.width;
  descriptor.height = configuration.height;
  descriptor.presentMode = configuration.presentMode;
  impl.context = std::make_shared<ContextImpl>();
  const backend::Result initialized = impl.context->initialize(device, configuration.preferred,
          configuration.jobSystem,
          configuration.diagnosticsCallback, configuration.diagnosticsUserData);
  if (!backend::isSuccess(initialized)) {
    impl.diagnostics = impl.context->diagnostics();
    return false;
  }

  if (!backend::isSuccess(device->createSemaphore(backend::SemaphoreDescriptor{}, &impl.acquireSemaphore)) ||
          !backend::isSuccess(device->createSemaphore(backend::SemaphoreDescriptor{},
                  &impl.renderFinishedSemaphore))) {
    impl.diagnostics = "the window surface could not create its acquire/present semaphores";
    return false;
  }

  // The RHI's own surface record, then the swapchain that presents through it. The facade never talks to the platform
  // layer directly; it names the window once and the backend keeps the record.
  if (!backend::isSuccess(device->createSurface(descriptor, &impl.surface))) {
    impl.diagnostics = "the backend refused the window surface";
    return false;
  }

  backend::SwapchainDescriptor swapchainDescriptor;
  swapchainDescriptor.surface = impl.surface;
  swapchainDescriptor.width = configuration.width;
  swapchainDescriptor.height = configuration.height;
  // The requested format is only a preference. The swapchain may grant B8G8R8A8 instead, and a
  // dynamic-rendering pipeline's attachment format has to equal the swapchain image's format, so the
  // target is created from `SwapchainInfo::format` rather than from the request.
  //
  // ## The output colour space selects the swapchain format
  //
  // The surface only *asks*; the backend grants or falls back, and `SwapchainInfo::colorSpace` below is
  // what the presentation pass reads. A 16F swapchain is the EDR path (values above 1.0 have headroom);
  // PQ needs a 10-bit UNORM target; P3 SDR is an ordinary 8-bit target with a wide-gamut colour space.
  impl.requestedColorSpace = configuration.outputColorSpace;
  impl.sdrWhiteNits = configuration.sdrWhiteNits > 0.0f ? configuration.sdrWhiteNits : 203.0f;
  swapchainDescriptor.presentMode = configuration.presentMode;
  switch (configuration.outputColorSpace) {
    case backend::SwapchainColorSpace::Srgb:
      swapchainDescriptor.format = backend::PixelFormat::R8G8B8A8_UNORM;
      swapchainDescriptor.colorSpace = backend::SwapchainColorSpace::Srgb;
      break;
    case backend::SwapchainColorSpace::DisplayP3:
      swapchainDescriptor.format = backend::PixelFormat::R8G8B8A8_UNORM;
      swapchainDescriptor.colorSpace = backend::SwapchainColorSpace::DisplayP3;
      break;
    case backend::SwapchainColorSpace::ExtendedLinearP3:
      swapchainDescriptor.format = backend::PixelFormat::R16G16B16A16_SFLOAT;
      swapchainDescriptor.colorSpace = backend::SwapchainColorSpace::ExtendedLinearP3;
      break;
    case backend::SwapchainColorSpace::Hdr10Pq:
      swapchainDescriptor.format = backend::PixelFormat::R10G10B10A2_UNORM;
      swapchainDescriptor.colorSpace = backend::SwapchainColorSpace::Hdr10Pq;
      break;
  }
  if (!backend::isSuccess(device->createSwapchain(swapchainDescriptor, &impl.swapchain))) {
    impl.diagnostics = "the backend refused the swapchain for this window";
    return false;
  }

  backend::SwapchainInfo granted;
  const bool hasGrantedInfo = backend::isSuccess(device->getSwapchainInfo(impl.swapchain, &granted));
  const backend::PixelFormat targetFormat =
          hasGrantedInfo && granted.format != backend::PixelFormat::Undefined ? granted.format
                                                                              : swapchainDescriptor.format;
  const uint32_t slotCount = std::max<uint32_t>(1u, impl.context->frameSlotCount());
  impl.targets.resize(slotCount);
  for (uint32_t slot = 0; slot < slotCount; ++slot) {
    impl.context->selectFrameSlot(slot);
    if (!backend::isSuccess(impl.context->createWindowTarget(configuration.width, configuration.height, targetFormat,
            configuration.sampleCount, &impl.targets[slot]))) {
      impl.diagnostics = "the window render target could not be created";
    return false;
    }
  }
  impl.context->selectFrameSlot(0);
  impl.target = impl.targets[0];

  impl.width = configuration.width;
  impl.height = configuration.height;
  impl.sampleCount = impl.target.sampleCount;
  // What the backend granted, not what was asked for: a driver without swap-interval control keeps Fifo, and a caller that
  // requested Immediate has to be able to see that it did not get it.
  impl.presentMode = hasGrantedInfo ? granted.presentMode : configuration.presentMode;
  impl.colorSpace = hasGrantedInfo ? granted.colorSpace : backend::SwapchainColorSpace::Srgb;
  // ## The canvas target is never PQ now
  //
  // The old HDR path made `image.frag` encode PQ straight into the window's 8-bit canvas. That is the
  // SDR-in-an-HDR-container transform and it stays available through `SurfaceConfiguration::pqOutput`;
  // the window path instead keeps the canvas in sRGB and runs the linear 16F presentation pass, which
  // is the only way an EDR value above diffuse white survives to the swapchain.
  impl.pqOutput = false;
  for (RenderTarget& slot: impl.targets) slot.pqOutput = false;
  impl.target.pqOutput = false;
  if (impl.colorSpace == backend::SwapchainColorSpace::Hdr10Pq) {
    // HDR10 static metadata: the PQ numbers alone do not tell the compositor what they mean. The values
    // describe SDR content in an HDR container (BT.2408 white at 203 cd/m^2), which is what the output
    // stage encodes; a backend without VK_EXT_hdr_metadata reports Unsupported, which is accepted here.
    backend::HdrMetadata hdr;
    hdr.maxContentLightLevel = impl.sdrWhiteNits;
    hdr.maxFrameAverageLightLevel = impl.sdrWhiteNits;
    (void)device->setHdrMetadata(impl.swapchain, hdr);
  }
  if (configuration.outputColorSpace != backend::SwapchainColorSpace::Srgb &&
      impl.colorSpace != configuration.outputColorSpace) {
    impl.diagnostics = "the output colour space was requested but this surface granted " +
            std::string(impl.colorSpace == backend::SwapchainColorSpace::Srgb ? "Srgb" : "another space") +
            "; presenting in the granted space";
  }
  return true;
}

uint32_t WindowSurface::width() const noexcept { return mImpl != nullptr ? mImpl->width : 0; }

uint32_t WindowSurface::height() const noexcept { return mImpl != nullptr ? mImpl->height : 0; }

backend::PixelFormat WindowSurface::format() const noexcept {
  return mImpl != nullptr ? mImpl->target.format : backend::PixelFormat::Undefined;
}

uint32_t WindowSurface::sampleCount() const noexcept {
  return mImpl != nullptr ? mImpl->sampleCount : 1;
}

std::string WindowSurface::describeDevice() const {
  if (mImpl == nullptr || mImpl->context == nullptr) return "no device";
  return mImpl->context->describeDevice();
}

std::string WindowSurface::diagnostics() const { return mImpl != nullptr ? mImpl->diagnostics : std::string(); }

backend::Device* WindowSurface::device() const noexcept { return mImpl != nullptr ? mImpl->device : nullptr; }

backend::Device* WindowSurface::releaseDevice() {
  if (mImpl == nullptr || !mImpl->ownsDevice || mImpl->device == nullptr) return nullptr;
  mImpl->ownsDevice = false;
  return mImpl->device;
}

bool WindowSurface::sharedWithGroup() const noexcept { return mImpl != nullptr && mImpl->sharedWithGroup; }

FrameStages WindowSurface::lastFrameStages() const {
  if (mImpl == nullptr || mImpl->context == nullptr) return FrameStages {};
  return mImpl->context->lastFrameStages();
}

double WindowSurface::lastFrameGpuMilliseconds() const {
  if (mImpl == nullptr || mImpl->context == nullptr) return 0.0;
  return mImpl->context->lastFrameGpuMilliseconds();
}

uint64_t WindowSurface::residentBytes() const {
  if (mImpl == nullptr || mImpl->context == nullptr) return 0;
  return mImpl->context->residentBytes();
}

backend::PresentMode WindowSurface::presentMode() const noexcept {
  return mImpl != nullptr ? mImpl->presentMode : backend::PresentMode::Fifo;
}

backend::SwapchainColorSpace WindowSurface::displayColorSpace() const noexcept {
  return mImpl != nullptr ? mImpl->colorSpace : backend::SwapchainColorSpace::Srgb;
}

WindowSurface::RenderTargetView WindowSurface::renderTarget() const noexcept {
  RenderTargetView view;
  if (mImpl == nullptr) return view;
  view.width = mImpl->target.width;
  view.height = mImpl->target.height;
  view.format = mImpl->target.format;
  view.pass = mImpl->target.pass;
  view.colorView = mImpl->target.colorView;
  return view;
}

graphics::Canvas* WindowSurface::beginFrame() {
  return beginFrameImpl(nullptr);
}

graphics::Canvas* WindowSurface::beginFrame(void* nativeDrawable) {
  return beginFrameImpl(nativeDrawable);
}

graphics::Canvas* WindowSurface::beginFrameImpl(void* nativeDrawable) {
  if (mImpl == nullptr || mImpl->context == nullptr) return nullptr;
  if (mImpl->frameOpen) {
    MINILOG_WARN("[graphics/gpu/window-surface] beginFrame called twice without a present in between");
    return nullptr;
  }
  mImpl->retainedValid = false;
  mImpl->retainedImages.clear();

  // ## Activate this device before touching the frame
  //
  // A context is current per thread and only one at a time, so a caller that owns several devices (a pool, or a group where
  // each surface has its own) must say which one it is about to use. Doing it here means the facade is always usable after a
  // `beginFrame` without the caller having to know about activation at all.
  if (!backend::isSuccess(mImpl->device->makeCurrent())) {
    mImpl->diagnostics = "this device's context could not be made current on this thread";
    return nullptr;
  }

  // Releases another thread handed to the group run here, with this device's context current -- which is the only place a GL
  // delete is legal. It is deliberately before `beginFrame` on the swapchain: the tasks are about the previous frame's
  // resources, and running them first keeps this frame's acquisition after them.
  if (mImpl->group != nullptr) {
    mImpl->group->drainDeferredReleases(mImpl->device);
  }

  backend::SwapchainInfo info;
  backend::Result acquired = backend::Result::Success;
  if (nativeDrawable != nullptr) {
    // A display link already owns the drawable, so there is nothing to acquire and no acquire
    // semaphore to signal. The overload is requested explicitly: a backend without it reports
    // Unsupported rather than silently acquiring a second drawable.
    mImpl->displayLinkFrame = true;
    acquired = mImpl->device->beginFrameWithDrawable(mImpl->swapchain, nativeDrawable, &info);
    if (acquired == backend::Result::OutOfDate) {
      // A drawable from the display link cannot be re-acquired; the link itself is stale. Resizing the
      // swapchain would not replace the drawable, so this is reported rather than retried.
      mImpl->diagnostics = "the display link's drawable is out of date";
      mImpl->displayLinkFrame = false;
      return nullptr;
    }
  } else {
    mImpl->displayLinkFrame = false;
    acquired = mImpl->device->beginFrame(mImpl->swapchain, &info, mImpl->acquireSemaphore);
    if (acquired == backend::Result::OutOfDate) {
      // The swapchain is stale (window resize, mode change, surface loss). Rebuild it at the size the
      // surface already knows, then acquire once more; this is the one place the caller should not have
      // to know about OutOfDate.
      if (!resize(mImpl->width, mImpl->height)) return nullptr;
      acquired = mImpl->device->beginFrame(mImpl->swapchain, &info, mImpl->acquireSemaphore);
    }
  }
  if (!backend::isSuccess(acquired)) {
    mImpl->diagnostics = "the swapchain could not hand over a frame (result " +
      std::to_string(static_cast<int>(acquired)) + ")";
    return nullptr;
  }

  // The platform is the authority on the size: a window that was resized between frames reports it here, and the target
  // has to follow before anything is drawn into it.
  // A begin may report a different granted mode (after a resize, or a driver that changed its mind): keep it current.
  mImpl->presentMode = info.presentMode;

  if (info.width != 0 && info.height != 0 && (info.width != mImpl->width || info.height != mImpl->height)) {
    if (!resize(info.width, info.height)) {
      return nullptr;
    }
  }

  if (mImpl->targets.empty()) return nullptr;
  mImpl->activeTarget = mImpl->hasFrame
          ? (mImpl->activeTarget + 1u) % static_cast<uint32_t>(mImpl->targets.size())
          : 0u;
  mImpl->hasFrame = true;
  mImpl->context->selectFrameSlot(mImpl->activeTarget);
  mImpl->swapchainTarget = mImpl->targets[mImpl->activeTarget];

  // UI frames are rendered into a persistent sampled target so a damage rectangle can update only
  // part of it; the swapchain image is refreshed from it on present. A swapchain image itself is not
  // guaranteed to keep its contents between frames.
  const backend::PixelFormat targetFormat = backend::PixelFormat::R8G8B8A8_UNORM;
  if (!mImpl->uiTarget.isValid() || mImpl->uiTarget.width != mImpl->width ||
      mImpl->uiTarget.height != mImpl->height || mImpl->uiTarget.format != targetFormat) {
    if (mImpl->uiTarget.isValid()) mImpl->context->destroyRenderTarget(mImpl->uiTarget);
    RenderTargetDescription description;
    description.width = mImpl->width;
    description.height = mImpl->height;
    description.format = targetFormat;
    description.needsStencil = true;
    description.sampleCount = 1;
    description.sampled = true;
    // The UI target is what actually holds the page: `readback` samples it instead of re-rendering the last
    // recording, which for a damage-only frame would draw the damage rect and nothing else.
    description.readable = true;
    description.label = "window-ui";
    if (!backend::isSuccess(mImpl->context->createRenderTarget(description, &mImpl->uiTarget))) {
      mImpl->diagnostics = "the window UI target could not be created";
      return nullptr;
    }
  }
  mImpl->target = mImpl->uiTarget;

  mImpl->recording = std::make_shared<DrawList>();
  mImpl->recording->label = "window-frame";
  mImpl->canvas = graphics::Canvas::createRecordingBackend(static_cast<int>(mImpl->width),
          static_cast<int>(mImpl->height), mImpl->context, mImpl->recording);
  if (mImpl->canvas == nullptr) {
    mImpl->diagnostics = "no recording backend could be created";
    return nullptr;
  }
  mImpl->frameOpen = true;
  return mImpl->canvas.get();
}

bool WindowSurface::drawExternalImage(const DrawList::ExternalImage& image, const Rect& deviceBounds,
    const Paint& paint) {
  if (mImpl == nullptr || mImpl->recording == nullptr || !mImpl->frameOpen) {
    return false;
  }
  if (!image.isValid() || deviceBounds.getWidth() <= 0.0f || deviceBounds.getHeight() <= 0.0f) {
    return false;
  }
  DrawList& list = *mImpl->recording;
  const Rect targetExtent = Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(mImpl->target.width),
    static_cast<Scalar>(mImpl->target.height));
  gpu::appendExternalImageCommand(list, image, deviceBounds, targetExtent, DrawList::kNoClipMask, 0,
    paint);
  list.bounds = Rect::MakeLTRB(std::min(list.bounds.getLeft(), deviceBounds.getLeft()),
    std::min(list.bounds.getTop(), deviceBounds.getTop()),
    std::max(list.bounds.getRight(), deviceBounds.getRight()),
    std::max(list.bounds.getBottom(), deviceBounds.getBottom()));
  return true;
}

bool WindowSurface::blitToSwapchain(bool waitAcquire) {
  if (mImpl == nullptr || !mImpl->uiTarget.isValid() || !mImpl->swapchainTarget.isValid()) {
    if (mImpl != nullptr) mImpl->diagnostics = "the window blit has no UI or swapchain target";
    return false;
  }

  // ## The SDR path is still the direct blit
  //
  // No colour conversion is needed and an extra 16F target would be a copy for nothing. `image.frag`
  // carries the source through unchanged, and the swapchain's own sRGB/present handling is the SDR
  // reference this path has always produced.
  if (mImpl->colorSpace == backend::SwapchainColorSpace::Srgb) {
    DrawList::ExternalImage image;
    image.texture = mImpl->uiTarget.color;
    image.view = mImpl->uiTarget.colorView;
    image.width = mImpl->uiTarget.width;
    image.height = mImpl->uiTarget.height;
    image.format = mImpl->uiTarget.format;
    image.premultiplied = true;

    gpu::Recorder recorder(static_cast<int>(mImpl->width), static_cast<int>(mImpl->height),
      mImpl->context.get());
    graphics::Canvas& canvas = recorder.canvas();
    const Rect targetRect = Rect::MakeLTRB(0.0f, 0.0f, static_cast<Scalar>(mImpl->width),
      static_cast<Scalar>(mImpl->height));
    Paint paint;
    paint.color = graphics::Color::White();
    paint.style = Paint::Style::kFill;
    paint.blendMode = BlendMode::kSrc;
    canvas.drawExternalImage(image, targetRect, paint);
    DrawList blit = recorder.finish();

    const backend::Result submitted = mImpl->context->submit(blit,
      mImpl->targets[mImpl->activeTarget], {}, backend::LoadOp::Clear,
      waitAcquire ? mImpl->acquireWait() : backend::SemaphoreHandle::invalid(),
      mImpl->renderFinishedSemaphore);
    if (!backend::isSuccess(submitted)) {
      mImpl->diagnostics = "the window blit could not be submitted";
      return false;
    }
    if (!backend::isSuccess(mImpl->context->waitFrameSlot(mImpl->activeTarget))) {
      mImpl->diagnostics = "the window blit did not finish";
      return false;
    }
    return true;
  }

  // ## The colour-managed path: canvas (sRGB) -> linear P3 16F -> display encoding
  //
  // The first pass is the linear 16F presentation pass. It decodes the canvas, converts Rec.709 to
  // Display P3 and writes float values, so the second pass has a target that can hold highlight
  // headroom and does not band. The second pass writes the encoding the granted swapchain needs: PQ,
  // extended linear (EDR) or gamma-encoded P3.
  constexpr backend::PixelFormat kLinearFormat = backend::PixelFormat::R16G16B16A16_SFLOAT;
  if (!mImpl->linearTarget.isValid() || mImpl->linearTarget.width != mImpl->width ||
      mImpl->linearTarget.height != mImpl->height || mImpl->linearTarget.format != kLinearFormat) {
    if (mImpl->linearTarget.isValid()) mImpl->context->destroyRenderTarget(mImpl->linearTarget);
    RenderTargetDescription description;
    description.width = mImpl->width;
    description.height = mImpl->height;
    description.format = kLinearFormat;
    description.needsStencil = false;
    description.sampleCount = 1;
    description.sampled = true;
    description.readable = false;
    description.label = "window-present-linear";
    if (!backend::isSuccess(mImpl->context->createRenderTarget(description, &mImpl->linearTarget))) {
      mImpl->diagnostics = "the linear presentation target could not be created";
      return false;
    }
  }

  const PresentationParameters linearParameters {
    .sourceIsLinear = false,
    .outputColorSpace = backend::SwapchainColorSpace::ExtendedLinearP3,
    .sdrWhiteNits = mImpl->sdrWhiteNits,
  };
  backend::Result result = mImpl->context->present(mImpl->uiTarget, mImpl->linearTarget, linearParameters);
  if (!backend::isSuccess(result)) {
    mImpl->diagnostics = "the linear 16F presentation pass could not be rendered";
    return false;
  }

  const PresentationParameters displayParameters {
    .sourceIsLinear = true,
    .outputColorSpace = mImpl->colorSpace,
    .sdrWhiteNits = mImpl->sdrWhiteNits,
  };
  result = mImpl->context->present(mImpl->linearTarget, mImpl->targets[mImpl->activeTarget],
          displayParameters,
          waitAcquire ? mImpl->acquireWait() : backend::SemaphoreHandle::invalid(),
          mImpl->renderFinishedSemaphore);
  if (!backend::isSuccess(result)) {
    mImpl->diagnostics = "the display-encoding presentation pass could not be rendered";
    return false;
  }
  if (!backend::isSuccess(mImpl->context->waitFrameSlot(mImpl->activeTarget))) {
    mImpl->diagnostics = "the presentation passes did not finish";
    return false;
  }
  return true;
}

bool WindowSurface::present() {
  if (mImpl == nullptr || mImpl->context == nullptr) return false;
  if (!mImpl->frameOpen) {
    mImpl->diagnostics = "present without a frame: call beginFrame first";
    return false;
  }

  // ## Two ways to fill a frame, and only one of them has a recording to submit
  //
  // A canvas frame records its commands and this submits them. A chain that ends at this window renders **directly** into
  // the swapchain image -- `coral::RenderTarget::adoptWindow` -- and there is nothing to submit: the pass already ran, so
  // submitting an empty recording here would clear the picture the chain just drew. Hence the emptiness test rather than a
  // flag: an empty recording means "somebody else drew this frame".
  if (mImpl->recording != nullptr && !mImpl->recording->isEmpty()) {
    mImpl->context->selectFrameSlot(mImpl->activeTarget);
    const std::vector<Bitmap> images = recordedImages(mImpl->recording.get());
    // The UI pass renders into the persistent target and waits for the acquire; the blit that follows
    // is ordered after it on the queue and signals present.
    const backend::Result submitted = mImpl->context->submit(*mImpl->recording, mImpl->uiTarget,
            images, backend::LoadOp::Clear, mImpl->acquireWait(),
            backend::SemaphoreHandle::invalid());
    if (!backend::isSuccess(submitted)) {
      mImpl->diagnostics = "the frame could not be rendered into the window";
      return false;
    }
    mImpl->retainedImages = images;
    mImpl->retainedValid = true;
    if (!blitToSwapchain(false)) return false;
  } else {
    if (!blitToSwapchain(true)) return false;
  }
  mImpl->canvas.reset();
  mImpl->frameOpen = false;

  // A canvas frame ordered its own submission after the acquire and signalled `renderFinishedSemaphore`.
  // A direct chain rendered into the window target without a submission here, so the acquire semaphore
  // is still the thing present has to wait for.
  const backend::Result presented = mImpl->device->endFrame(mImpl->swapchain,
          mImpl->renderFinishedSemaphore);
  if (!backend::isSuccess(presented)) {
    mImpl->diagnostics = "the swapchain could not present: " + std::to_string(static_cast<int>(presented));
    return false;
  }
  return true;
}

bool WindowSurface::presentRetained() {
  if (mImpl == nullptr || mImpl->context == nullptr || !mImpl->retainedValid ||
      mImpl->recording == nullptr || mImpl->frameOpen) {
    return false;
  }
  if (!backend::isSuccess(mImpl->device->makeCurrent())) {
    mImpl->diagnostics = "this device's context could not be made current on this thread";
    return false;
  }
  if (mImpl->group != nullptr) mImpl->group->drainDeferredReleases(mImpl->device);

  backend::SwapchainInfo info;
  mImpl->displayLinkFrame = false;
  backend::Result acquired = mImpl->device->beginFrame(mImpl->swapchain, &info, mImpl->acquireSemaphore);
  if (acquired == backend::Result::OutOfDate) {
    if (!resize(mImpl->width, mImpl->height)) return false;
    acquired = mImpl->device->beginFrame(mImpl->swapchain, &info, mImpl->acquireSemaphore);
  }
  if (!backend::isSuccess(acquired)) {
    mImpl->diagnostics = "the swapchain could not hand over a frame (result " +
      std::to_string(static_cast<int>(acquired)) + ")";
    return false;
  }
  mImpl->presentMode = info.presentMode;
  if (info.width != 0 && info.height != 0 && (info.width != mImpl->width || info.height != mImpl->height)) {
    if (!resize(info.width, info.height)) return false;
  }
  if (mImpl->targets.empty()) return false;

  mImpl->activeTarget = mImpl->hasFrame
    ? (mImpl->activeTarget + 1u) % static_cast<uint32_t>(mImpl->targets.size())
    : 0u;
  mImpl->hasFrame = true;
  mImpl->context->selectFrameSlot(mImpl->activeTarget);
  mImpl->swapchainTarget = mImpl->targets[mImpl->activeTarget];
  mImpl->target = mImpl->uiTarget;

  // The UI target already holds the picture; only the swapchain image has to be refreshed.
  if (!blitToSwapchain(true)) return false;
  const backend::Result presented = mImpl->device->endFrame(mImpl->swapchain,
    mImpl->renderFinishedSemaphore);
  if (!backend::isSuccess(presented)) {
    mImpl->diagnostics = "the swapchain could not present: " + std::to_string(static_cast<int>(presented));
    return false;
  }
  return true;
}
bool WindowSurface::presentRecording(const std::shared_ptr<const DrawList>& list) {
  if (mImpl == nullptr || list == nullptr) return false;
  mImpl->lastExternalRecording = list;
  return presentRecordingsImpl({ list.get() }, nullptr);
}

bool WindowSurface::presentRecording(const std::shared_ptr<const DrawList>& list, const Rect& damage) {
  if (mImpl == nullptr || list == nullptr) return false;
  mImpl->lastExternalRecording = list;
  const bool partial = damage.getWidth() > 0.0f && damage.getHeight() > 0.0f;
  return presentRecordingsImpl({ list.get() }, partial ? &damage : nullptr);
}

bool WindowSurface::presentRecording(const DrawList& list) {
  if (mImpl == nullptr) return false;
  // The caller promised to keep the list alive across the call; `readback` after the call is only
  // safe while that remains true.
  mImpl->lastExternalRecording = std::shared_ptr<const DrawList>(&list, [](const DrawList*) {});
  return presentRecordingsImpl({ &list }, nullptr);
}

std::shared_ptr<const DrawList> WindowSurface::currentRecording() const {
  if (mImpl == nullptr) return nullptr;
  return mImpl->recording;
}

uint32_t WindowSurface::lastFrameSubmitCount() const noexcept {
  return mImpl != nullptr ? mImpl->lastSubmitCount : 0;
}

uint32_t WindowSurface::lastFrameDrawCallCount() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().drawCallCount : 0;
}

uint32_t WindowSurface::lastFrameBatchCount() const noexcept {
  return mImpl != nullptr && mImpl->context != nullptr ? mImpl->context->lastFrameStats().batchCount : 0;
}

bool WindowSurface::presentListItems(const std::vector<ListItem>& items) {
  if (mImpl == nullptr || mImpl->context == nullptr) return false;
  std::vector<const DrawList*> lists;
  std::vector<ListItem> states;
  // The content recorded through `beginFrame` is the frame's first list, exactly as it is for
  // `presentRecordings`; the plan items come after it in paint order.
  if (mImpl->recording != nullptr && !mImpl->recording->isEmpty()) {
    lists.push_back(mImpl->recording.get());
    ListItem content;
    content.list = mImpl->recording;
    states.push_back(std::move(content));
  }
  for (const ListItem& item: items) {
    if (item.list == nullptr || item.list->isEmpty()) continue;
    // A per-list opacity would need an offscreen composite, which this path does not express; refusing
    // is what makes the caller fold the lists instead of drawing them wrong.
    if (item.opacity != 1.0f) return false;
    std::shared_ptr<const DrawList> list = item.list;
    if (!item.clip.isEmpty() || item.translation.x != 0.0f || item.translation.y != 0.0f) {
      list = bakeListState(*list, item.clip, item.translation);
      if (list == nullptr) return false;
    }
    ListItem state;
    state.list = std::move(list);
    lists.push_back(state.list.get());
    states.push_back(std::move(state));
  }
  if (lists.empty()) return false;
  mImpl->lastExternalRecording = nullptr;
  mImpl->retainedImages.clear();
  mImpl->retainedValid = false;
  // `beginFrame` has already acquired a swapchain image and recorded the content list. Reusing that
  // frame is what makes the plan path one acquire/present: re-beginning here would either deadlock on
  // the already-acquired drawable or present twice and flash the boundary-less content.
  if (mImpl->frameOpen) {
    return presentRecordingsImpl(lists, nullptr, &states, true);
  }
  mImpl->canvas.reset();
  mImpl->frameOpen = false;
  mImpl->recording.reset();
  return presentRecordingsImpl(lists, nullptr, &states);
}

bool WindowSurface::presentRecordings(const std::vector<std::shared_ptr<const DrawList>>& lists) {
  std::vector<const DrawList*> pending;
  for (const std::shared_ptr<const DrawList>& list: lists) {
    if (list != nullptr && !list->isEmpty()) pending.push_back(list.get());
  }
  if (pending.empty()) return false;
  mImpl->lastExternalRecording = nullptr;
  // The lists are owned by the caller (the UI's frame plan keeps them alive through the call), which is
  // the same promise `presentRecording(const DrawList&)` relies on.
  mImpl->retainedImages.clear();
  mImpl->retainedValid = false;
  bool ok = true;
  mImpl->canvas.reset();
  mImpl->frameOpen = false;
  mImpl->recording.reset();
  ok = presentRecordingsImpl(pending, nullptr);
  return ok;
}

bool WindowSurface::presentRecordingsImpl(const std::vector<const DrawList*>& lists, const Rect* damage,
    const std::vector<ListItem>* states, const bool reuseOpenFrame) {
  if (lists.empty()) return false;
  if (mImpl == nullptr || mImpl->context == nullptr) return false;
  if (!backend::isSuccess(mImpl->device->makeCurrent())) {
    mImpl->diagnostics = "this device's context could not be made current on this thread";
    return false;
  }
  if (mImpl->group != nullptr) mImpl->group->drainDeferredReleases(mImpl->device);

  if (!reuseOpenFrame) {
    // A frame recorded through `beginFrame` is not the input here; abandon it rather than mixing two
    // recordings into one frame.
    mImpl->canvas.reset();
    mImpl->frameOpen = false;
    mImpl->retainedValid = false;
    mImpl->recording.reset();

    backend::SwapchainInfo info;
    mImpl->displayLinkFrame = false;
    backend::Result acquired = mImpl->device->beginFrame(mImpl->swapchain, &info, mImpl->acquireSemaphore);
    if (acquired == backend::Result::OutOfDate) {
      if (!resize(mImpl->width, mImpl->height)) return false;
      acquired = mImpl->device->beginFrame(mImpl->swapchain, &info, mImpl->acquireSemaphore);
    }
    if (!backend::isSuccess(acquired)) {
      mImpl->diagnostics = "the swapchain could not hand over a frame (result " +
        std::to_string(static_cast<int>(acquired)) + ")";
      return false;
    }
    mImpl->presentMode = info.presentMode;
    if (info.width != 0 && info.height != 0 && (info.width != mImpl->width || info.height != mImpl->height)) {
      if (!resize(info.width, info.height)) return false;
    }
    if (mImpl->targets.empty()) return false;

    mImpl->activeTarget = mImpl->hasFrame
      ? (mImpl->activeTarget + 1u) % static_cast<uint32_t>(mImpl->targets.size())
      : 0u;
    mImpl->hasFrame = true;
    mImpl->context->selectFrameSlot(mImpl->activeTarget);
    mImpl->swapchainTarget = mImpl->targets[mImpl->activeTarget];
  } else if (!mImpl->frameOpen || !mImpl->uiTarget.isValid()) {
    mImpl->diagnostics = "a plan frame had no open frame to complete";
    return false;
  }

  // The persistent UI target is the surface that receives a damage rectangle on later frames; it is
  // recreated here for the same reason `beginFrame` does it.
  const backend::PixelFormat targetFormat = backend::PixelFormat::R8G8B8A8_UNORM;
  if (!mImpl->uiTarget.isValid() || mImpl->uiTarget.width != mImpl->width ||
      mImpl->uiTarget.height != mImpl->height || mImpl->uiTarget.format != targetFormat) {
    if (mImpl->uiTarget.isValid()) mImpl->context->destroyRenderTarget(mImpl->uiTarget);
    RenderTargetDescription description;
    description.width = mImpl->width;
    description.height = mImpl->height;
    description.format = targetFormat;
    description.needsStencil = true;
    description.sampleCount = 1;
    description.sampled = true;
    // The UI target is what actually holds the page: `readback` samples it instead of re-rendering the last
    // recording, which for a damage-only frame would draw the damage rect and nothing else.
    description.readable = true;
    description.label = "window-ui";
    if (!backend::isSuccess(mImpl->context->createRenderTarget(description, &mImpl->uiTarget))) {
      mImpl->diagnostics = "the window UI target could not be created";
      return false;
    }
  }
  mImpl->target = mImpl->uiTarget;

  // A damage-only packet keeps the pixels outside its rectangle: the UI target is loaded instead of
  // cleared and the renderer scissors every command to the damaged area. With several lists the first
  // one performs the clear and the rest load what it wrote.
  const backend::LoadOp rootLoad = damage != nullptr ? backend::LoadOp::Load : backend::LoadOp::Clear;
  const Rect damageRect = damage != nullptr ? *damage : Rect{};
  bool first = true;
  uint32_t submits = 0;
  for (size_t index = 0; index < lists.size(); ++index) {
    const DrawList* entry = lists[index];
    if (entry == nullptr || entry->isEmpty()) continue;
    // A plan-style frame carries a clip and a translation per list; they ride the submission, and the
    // lists share the frame's command buffer (continueFrame) exactly as Surface::submitLists does.
    const Rect clip = states != nullptr && index < states->size() ? (*states)[index].clip : damageRect;
    const Point translation = states != nullptr && index < states->size()
      ? (*states)[index].translation : Point{};
    const bool continueFrame = states != nullptr && index + 1 < lists.size();
    const std::vector<Bitmap> images = recordedImages(entry);
    const backend::Result submitted = mImpl->context->submit(*entry, mImpl->target, images,
      first ? rootLoad : backend::LoadOp::Load,
      first ? mImpl->acquireWait() : backend::SemaphoreHandle::invalid(),
      backend::SemaphoreHandle::invalid(), clip, translation, continueFrame);
    if (!backend::isSuccess(submitted)) {
      mImpl->diagnostics = "the recorded UI frame could not be rendered";
      return false;
    }
    ++submits;
    first = false;
  }
  // A plan-style frame is one command buffer however many lists it lent; a plain multi-list frame is
  // one per list, which is what the counter has always meant for this path.
  mImpl->lastSubmitCount = states != nullptr ? 1u : submits;
  // The external list is owned by the caller/UI thread; `presentRetained` cannot replay it safely.
  mImpl->retainedImages.clear();
  mImpl->retainedValid = false;
  if (!blitToSwapchain(false)) return false;
  const backend::Result presented = mImpl->device->endFrame(mImpl->swapchain,
    mImpl->renderFinishedSemaphore);
  if (!backend::isSuccess(presented)) {
    mImpl->diagnostics = "the swapchain could not present: " +
      std::to_string(static_cast<int>(presented));
    return false;
  }
  if (reuseOpenFrame) {
    mImpl->canvas.reset();
    mImpl->frameOpen = false;
    mImpl->recording.reset();
  }
  return true;
}

bool WindowSurface::presentDamage(const Rect& damage) {
  if (mImpl == nullptr || mImpl->context == nullptr || mImpl->recording == nullptr ||
      !mImpl->frameOpen || damage.getWidth() <= 0.0f || damage.getHeight() <= 0.0f) {
    return false;
  }
  mImpl->context->selectFrameSlot(mImpl->activeTarget);
  const std::vector<Bitmap> images = recordedImages(mImpl->recording.get());
  const backend::Result submitted = mImpl->context->submit(*mImpl->recording, mImpl->uiTarget,
    images, backend::LoadOp::Load, mImpl->acquireWait(), backend::SemaphoreHandle::invalid(),
    damage);
  if (!backend::isSuccess(submitted)) {
    mImpl->diagnostics = "the damaged frame could not be rendered";
    return false;
  }
  mImpl->retainedValid = false;
  mImpl->canvas.reset();
  mImpl->frameOpen = false;
  if (!blitToSwapchain(false)) return false;
  const backend::Result presented = mImpl->device->endFrame(mImpl->swapchain,
    mImpl->renderFinishedSemaphore);
  if (!backend::isSuccess(presented)) {
    mImpl->diagnostics = "the swapchain could not present: " + std::to_string(static_cast<int>(presented));
    return false;
  }
  return true;
}

bool WindowSurface::resize(const uint32_t width, const uint32_t height) {
  if (mImpl == nullptr || mImpl->context == nullptr || width == 0 || height == 0) return false;
  // A frame in flight owns a target this is about to destroy, so it is ended first. Refusing instead would leave the
  // caller with a window it cannot resize until it presents, which is the wrong thing to make it think about.
  mImpl->canvas.reset();
  mImpl->frameOpen = false;
  mImpl->retainedValid = false;
  mImpl->retainedImages.clear();

  if (mImpl->scratch.isValid()) {
    mImpl->context->destroyRenderTarget(mImpl->scratch);
    mImpl->scratch = RenderTarget {};
  }
  if (mImpl->linearTarget.isValid()) {
    mImpl->context->destroyRenderTarget(mImpl->linearTarget);
    mImpl->linearTarget = RenderTarget {};
  }
  for (RenderTarget& slot: mImpl->targets) {
    if (slot.isValid()) mImpl->context->destroyRenderTarget(slot);
  }
  mImpl->targets.clear();
  mImpl->target = RenderTarget {};
  if (mImpl->uiTarget.isValid()) mImpl->context->destroyRenderTarget(mImpl->uiTarget);
  mImpl->uiTarget = RenderTarget {};

  // Recreating targets allocates GL objects; after a resize the caller may not have made the device
  // current on this thread (the render-thread path in particular), which turned every buffer
  // allocation into GL_INVALID_OPERATION.
  if (!backend::isSuccess(mImpl->device->makeCurrent())) {
    mImpl->diagnostics = "the device's context could not be made current for resize";
    return false;
  }
  if (!backend::isSuccess(mImpl->device->resizeSwapchain(mImpl->swapchain, width, height))) {
    mImpl->diagnostics = "the swapchain refused the new size";
    return false;
  }
  backend::SwapchainInfo granted;
  const bool hasGrantedInfo = backend::isSuccess(mImpl->device->getSwapchainInfo(mImpl->swapchain, &granted));
  const backend::PixelFormat targetFormat =
          hasGrantedInfo && granted.format != backend::PixelFormat::Undefined ? granted.format
                                                                              : backend::PixelFormat::R8G8B8A8_UNORM;
  const uint32_t slotCount = std::max<uint32_t>(1u, mImpl->context->frameSlotCount());
  mImpl->targets.resize(slotCount);
  for (uint32_t slot = 0; slot < slotCount; ++slot) {
    mImpl->context->selectFrameSlot(slot);
    if (!backend::isSuccess(mImpl->context->createWindowTarget(width, height, targetFormat, mImpl->sampleCount,
            &mImpl->targets[slot]))) {
      mImpl->diagnostics = "the window render target could not be recreated";
      return false;
    }
    mImpl->targets[slot].pqOutput = mImpl->pqOutput;
  }
  mImpl->context->selectFrameSlot(mImpl->activeTarget);
  mImpl->target = mImpl->targets[mImpl->activeTarget];
  mImpl->target.pqOutput = mImpl->pqOutput;
  mImpl->width = width;
  mImpl->height = height;
  mImpl->sampleCount = mImpl->target.sampleCount;
  return true;
}

bool WindowSurface::readback(Bitmap* out) {
  if (mImpl == nullptr || mImpl->context == nullptr || out == nullptr) return false;
  // A caller may snapshot after a resize that destroyed the UI target, and the context is not
  // guaranteed to still be current on this thread; creating the readable target without it made
  // every GL buffer allocation fail with GL_INVALID_OPERATION.
  if (!backend::isSuccess(mImpl->device->makeCurrent())) {
    mImpl->diagnostics = "the device's context could not be made current for readback";
    return false;
  }
  mImpl->context->selectFrameSlot(mImpl->activeTarget);

  // The persistent UI target holds the presented page. Reading it is exact for a full frame and for a
  // damage-only frame alike; re-rendering the last recording cannot see the pixels a damage frame kept.
  if (mImpl->uiTarget.isValid()) {
    const backend::Result read = mImpl->context->readback(mImpl->uiTarget, out);
    if (backend::isSuccess(read)) {
      return true;
    }
  }

  const DrawList* recording = mImpl->recording != nullptr
    ? mImpl->recording.get() : mImpl->lastExternalRecording.get();
  if (recording == nullptr) return false;

  if (!mImpl->scratch.isValid()) {
    RenderTargetDescription description;
    description.width = mImpl->width;
    description.height = mImpl->height;
    description.format = backend::PixelFormat::R8G8B8A8_UNORM;
    description.needsStencil = false;
    description.sampled = false;
    description.readable = true;
    if (!backend::isSuccess(mImpl->context->createRenderTarget(description, &mImpl->scratch))) {
      mImpl->diagnostics = "no readable target could be created for the readback";
      return false;
    }
  }

  // The same recording, rendered again into a target that *can* be read: a swapchain image cannot, and its contents after
  // a swap are undefined on every backend.
  const backend::Result submitted = mImpl->context->submit(*recording, mImpl->scratch,
          recordedImages(recording), backend::LoadOp::Clear);
  if (!backend::isSuccess(submitted)) {
    mImpl->diagnostics = "the frame could not be re-rendered for the readback";
    return false;
  }
  const backend::Result read = mImpl->context->readback(mImpl->scratch, out);
  if (!backend::isSuccess(read)) {
    mImpl->diagnostics = "the readable target refused the readback";
    return false;
  }
  return true;
}

} // namespace graphics::gpu



