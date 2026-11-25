/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on
 * an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */

/**
 * @file demo_main.cpp
 *
 * The plumbing: find the resources, draw the scene, write the picture.
 *
 * ## Why this writes a file rather than opening a window
 *
 * A window would need a platform toolkit, and this is a demonstration of a *drawing* library, not of one. Writing a
 * PNG means the demo runs anywhere -- a build machine, a container, a laptop with no display -- and the result is a
 * file that can be looked at, committed or diffed. The drawing is the same either way: a surface is a surface.
 *
 * ## The arguments
 *
 * ```
 *   graphics_demo [--resources DIR] [--output FILE] [--font FILE] [--emoji-font FILE] [--image FILE]
 * ```
 *
 * Everything has a default and every resource is optional. Run it with no arguments from the repository root and it
 * finds what it needs; run it with `--resources` when it does not.
 */

#include "demo_scene.h"

#include <graphics/Bitmap.h>
#include <graphics/Data.h>
#include <graphics/Image.h>
#include <graphics/Surface.h>
#include <gpu/Surface.h>

#include <backend/Types.h>

#include <chrono>
#include <cmath>
#include <utils/JobSystem.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

/**
 * Which renderer draws the scene.
 *
 * The scene is the same either way -- `demo::drawScene` takes a `graphics::Canvas&` and neither it nor anything it
 * calls knows which implementation is behind it. That is the property this option exists to demonstrate: choosing a
 * GPU backend changes *where the pixels come from* and nothing about which calls a caller makes.
 */
enum class Renderer {
  /** The software rasterizer. Always available, needs no driver, runs on a build machine. */
  Raster,
  /** The hardware path: record into a `DrawList`, then submit. Needs a driver and an offscreen target. */
  Gpu,
};

/** The parsed command line. */
struct Options {
  std::string resources = "resources";
  std::string output = "coral_graphics_demo.png";
  std::string font;
  std::string image;
  std::string emojiFont;
  Renderer renderer = Renderer::Raster;
  /**
   * The GPU backend to ask for, when @c renderer is `Gpu`.
   *
   * `Null` means "whatever this platform has", which is what a caller who does not care should say. Naming one
   * anyway is the whole reason `SurfaceConfiguration::preferred` is a field rather than a second factory, and it is
   * what lets the same demo be pointed at a second backend when there is one.
   */
  backend::Backend backend = backend::Backend::Null;
  /** Draw the scene twice and report whether the two renderers agree, pixel for pixel. */
  bool compare = false;
  /**
   * How many frames to draw through the GPU surface.
   *
   * One by default, which is what a still comparison wants. More than one draws the scene again that many times and
   * reports the *cumulative* texture uploads -- the measurement that shows what a content-keyed image cache does for a
   * UI redrawing an unchanged scene, which a single frame cannot show at all.
   */
  int frames = 1;
  /**
   * The worker count for the frame's job pool, or a negative number for **no pool at all**.
   *
   * No pool is the default because of the measurement recorded in `renderGpu`: on this scene the parallel precompile was
   * about three times slower, and the note there says whoever makes it faster should turn it on and read the figure the
   * demo prints. This option is what makes that a command rather than an edit -- `--jobs 0` asks for one worker per
   * hardware thread, `--jobs 4` for four besides the calling thread, and leaving it out runs everything on the caller.
   */
  int jobs = -1;
  /**
   * Move the whole scene by a fraction of a pixel on every frame, so that no frame's coverage fields are the previous
   * frame's.
   *
   * The measurement the cross-frame field cache made necessary: `--frames N` draws an *identical* scene N times, which is
   * the case the cache covers completely (every field reused, `4.1.35`), and a UI that animates is the case it does not.
   * A sub-pixel translation is the smallest change that reaches every field's device-space geometry -- so every frame is a
   * miss, and the stage table that follows is what an animated frame costs. It is the same kind of flag as `--frames` and
   * `--jobs`: a command rather than an edit, for a question that can only be answered by running the real scene.
   */
  bool jitter = false;
  /**
   * Record the scene **once** into a list the demo owns, and submit a copy of it on every frame.
   *
   * The measurement flag for the retained form of the API: 4.1.38 and 4.1.39 measured the recording stage at ~100 ms a
   * frame against `submit`'s ~13 ms, so a caller that keeps its recording pays a copy instead. This flag is what makes
   * that a command, and its report says what the copy costs -- which is the whole question, and it is *not* the same as
   * "submit twice" (the copy is what a caller without a display list would not pay).
   *
   * It also exercises the retained path end to end for the first time: the picture a copied list produces has to be the
   * picture the same scene produces when recorded directly, and the PNG comparison is what says so.
   */
  bool retained = false;
  /**
   * Advance the Lottie animation by a tenth of a second per frame, so part of the scene moves and the rest does not.
   *
   * The animation case the field cache cannot serve at the *frame* level: --jitter moves everything, which no cache
   * of any shape could reuse, while this moves one panel -- which is the workload a bounded content-keyed field table is
   * supposed to pay for. CORAL_TRACE_FIELD_REUSE=<depth> is what measures whether it would (4.1.43).
   */
  bool animate = false;
  bool help = false;
};

void printUsage() {
  std::cout << "Coral graphics Canvas demo\n\n"
               "  graphics_demo [options]\n\n"
               "Options:\n"
               "  --resources DIR   where the resource directories live (default: resources)\n"
               "  --output FILE     the PNG to write (default: coral_graphics_demo.png)\n"
               "  --backend NAME    raster (default) or gpu -- which renderer draws the scene\n"
               "  --frames N        draw the scene N times through the GPU and report per-frame uploads (default: 1)\n"
               "  --jobs N          hand the frame a JobSystem with N worker threads besides this one (0: one per\n"
               "                    hardware thread). Absent means no pool, which is the default -- see the note in\n"
               "                    renderGpu for the measurement that decided it\n"
               "  --jitter          move the scene by a fraction of a pixel on every frame, so that no frame's coverage\n"
               "                    fields are the previous frame's -- the animated case, which the field cache cannot\n"
               "                    serve, against the identical-scene case it serves completely\n"
               "  --retained        record the scene once and submit a copy of the list on every frame -- the display-list\n"
               "                    form of the API, and the flag that measures what a frame costs without re-recording\n"
               "  --animate         advance the Lottie panel by 0.1s per frame: one part of the scene moves and the rest does\n"
               "                    not -- the animation a bounded per-field cache would be for (CORAL_TRACE_FIELD_REUSE)\n"
               "  --gpu-api NAME    the GPU API to ask for: null, gl, vulkan, metal (default: null, meaning\n"
               "                    whatever the platform has); only meaningful with --backend gpu\n"
               "  --compare         draw the scene on both renderers and report whether they agree\n"
               "  --font FILE       a specific font file to use\n"
               "  --image FILE      a specific image file to draw\n"
               "  --emoji-font FILE a colour-emoji font (default: resources/fonts/NotoColorEmoji.ttf)\n"
               "  --svg DIR         a directory of SVG documents to draw\n"
               "  --lottie FILE     a Lottie animation to draw\n"
               "  --help            this text\n\n"
               "Every resource is optional: a missing one is reported and the demo draws without it.\n"
               "The scene is byte-for-byte the same on either backend because both implement one `Canvas`.\n";
}

/** @return the backend named by @p name, or `Undefined` when the name is not one. */
backend::Backend parseBackend(const std::string& name, bool* recognised) {
  *recognised = true;
  if (name == "null") return backend::Backend::Null;
  if (name == "gl" || name == "opengl" || name == "gles") return backend::Backend::OpenGL;
  if (name == "vulkan") return backend::Backend::Vulkan;
  if (name == "metal") return backend::Backend::Metal;
  *recognised = false;
  // `Null` doubles as "unrecognised" and as "let the platform decide", which is why the caller gets `recognised`
  // separately: the two cases need different handling and one value cannot express both.
  return backend::Backend::Null;
}

/** @return the options, or `help` set when the arguments asked for the usage text. */
Options parseArguments(const int argc, char** argv) {
  Options options;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << name << " needs a value\n";
        return {};
      }
      return argv[++i];
    };

    if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else if (argument == "--resources") {
      options.resources = next("--resources");
    } else if (argument == "--output") {
      options.output = next("--output");
    } else if (argument == "--backend") {
      const std::string name = next("--backend");
      if (name == "raster" || name == "cpu" || name == "software") {
        options.renderer = Renderer::Raster;
      } else if (name == "gpu" || name == "hardware") {
        options.renderer = Renderer::Gpu;
      } else {
        std::cerr << "unrecognised backend: " << name << " (expected raster or gpu)\n";
        options.help = true;
      }
    } else if (argument == "--frames") {
    options.frames = std::max(1, std::stoi(next("--frames")));
  } else if (argument == "--jobs") {
    // The *value* is the worker count, so 0 keeps `JobSystem`'s own meaning ("one per hardware thread") and the flag's
    // presence is what asks for a pool. Nothing here clamps it: a caller asking for 64 workers on a 4-core machine is
    // measuring the pool's contention on purpose.
    options.jobs = std::max(0, std::stoi(next("--jobs")));
  } else if (argument == "--jitter") {
    options.jitter = true;
  } else if (argument == "--retained") {
    options.retained = true;
  } else if (argument == "--animate") {
    options.animate = true;
  } else if (argument == "--gpu-api") {
      bool recognised = false;
      const backend::Backend parsed = parseBackend(next("--gpu-api"), &recognised);
      if (!recognised) {
        std::cerr << "unrecognised GPU API (expected null, gl, vulkan or metal)\n";
        options.help = true;
      }
      options.backend = parsed;
    } else if (argument == "--compare") {
      options.compare = true;
    } else if (argument == "--font") {
      options.font = next("--font");
    } else if (argument == "--image") {
      options.image = next("--image");
    } else if (argument == "--emoji-font") {
      options.emojiFont = next("--emoji-font");
    } else if (argument == "--svg") {
      // Accepted and ignored rather than rejected: the scene already finds every SVG directory it knows about, and
      // failing on an argument the usage text used to advertise would be a worse surprise than accepting it.
      (void) next("--svg");
    } else if (argument == "--lottie") {
      (void) next("--lottie");
    } else {
      std::cerr << "unrecognised argument: " << argument << "\n";
      options.help = true;
    }
  }

  return options;
}

/** @return true when @p path exists and is a regular file. */bool isFile(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error);
}

/**
 * @return the first existing file among the candidates.
 *
 * The demo is run from several places -- a build directory, the repository root, an IDE -- so a path relative to the
 * current directory is often wrong. Walking upwards to find the repository root is what makes one command work from
 * any of them, and it is what the library's own tests do for the same reason.
 */
std::filesystem::path findFile(const std::vector<std::filesystem::path>& candidates) {
  for (const std::filesystem::path& candidate: candidates) {
    if (isFile(candidate)) return candidate;
  }
  return {};
}

/**
 * @return a font: the one asked for, or the first of the bundled ones that exists.
 *
 * The resources carry a Latin-capable Roboto in the vendored harfbuzz tree, the Noto fonts in the resources
 * directory, and a CJK-capable Noto as a fallback. The order is by likelihood of being present.
 */
std::filesystem::path resolveFont(const Options& options) {
  if (!options.font.empty()) {
    const std::filesystem::path wanted = options.font;
    return isFile(wanted) ? wanted : std::filesystem::path{};
  }

  std::vector<std::filesystem::path> candidates;
  std::filesystem::path directory = std::filesystem::current_path();
  for (int depth = 0; depth < 8 && !directory.empty(); ++depth) {
    candidates.push_back(directory / "third_party" / "harfbuzz" / "perf" / "fonts" / "Roboto-Regular.ttf");
    candidates.push_back(directory / options.resources / "fonts" / "NotoSansSC-Regular.otf");
    candidates.push_back(directory / "assets" / "fonts" / "Roboto-Medium.ttf");
    directory = directory.parent_path();
  }

  return findFile(candidates);
}

/**
 * @return the colour-emoji font: the one asked for, or `resources/fonts/NotoColorEmoji.ttf` beside the
 *         resources directory.
 *
 * Kept separate from `resolveFont` because it is not a fallback for the Latin font: the text panel
 * draws ordinary text with one and a `CBDT` emoji row with the other, and using a Latin font for the
 * row would silently draw six missing-glyph boxes.
 */
std::filesystem::path resolveEmojiFont(const Options& options) {
  if (!options.emojiFont.empty()) {
    const std::filesystem::path wanted = options.emojiFont;
    return isFile(wanted) ? wanted : std::filesystem::path{};
  }

  std::vector<std::filesystem::path> candidates;
  std::filesystem::path directory = std::filesystem::current_path();
  for (int depth = 0; depth < 8 && !directory.empty(); ++depth) {
    candidates.push_back(directory / options.resources / "fonts" / "NotoColorEmoji.ttf");
    directory = directory.parent_path();
  }
  return findFile(candidates);
}

/** @return an image to draw: the one asked for, or the first bundled one that exists. */
std::filesystem::path resolveImage(const Options& options) {
  if (!options.image.empty()) {
    const std::filesystem::path wanted = options.image;
    return isFile(wanted) ? wanted : std::filesystem::path{};
  }

  std::vector<std::filesystem::path> candidates;
  std::filesystem::path directory = std::filesystem::current_path();
  for (int depth = 0; depth < 8 && !directory.empty(); ++depth) {
    // A photograph and a small illustration, so the panel shows the JPEG path and a picture with an alpha channel.
    candidates.push_back(directory / options.resources / "assets" / "bridge.jpg");
    candidates.push_back(directory / options.resources / "apitest" / "mandrill_128.png");
    candidates.push_back(directory / "assets" / "image" / "bridge.jpg");
    directory = directory.parent_path();
  }

  return findFile(candidates);
}

/** @return a second, smaller image, or an empty path when there is none to find. */
std::filesystem::path resolveSecondImage(const Options& options) {
  std::vector<std::filesystem::path> candidates;
  std::filesystem::path directory = std::filesystem::current_path();
  for (int depth = 0; depth < 8 && !directory.empty(); ++depth) {
    candidates.push_back(directory / options.resources / "assets" / "glyph1.png");
    candidates.push_back(directory / options.resources / "apitest" / "checker_128.png");
    directory = directory.parent_path();
  }
  return findFile(candidates);
}

/**
 * @return the directory of SVG documents, or an empty path.
 *
 * A directory rather than a file, because the panel is a montage of the whole corpus: showing one document would not show
 * that the parser handles the grammar the others use.
 */
std::filesystem::path resolveSvgDirectory(const Options& options) {
  std::vector<std::filesystem::path> candidates;
  std::filesystem::path directory = std::filesystem::current_path();
  for (int depth = 0; depth < 8 && !directory.empty(); ++depth) {
    candidates.push_back(directory / options.resources / "apitest" / "SVG");
    candidates.push_back(directory / "resources" / "apitest" / "SVG");
    directory = directory.parent_path();
  }

  for (const std::filesystem::path& candidate : candidates) {
    std::error_code failure;
    if (std::filesystem::is_directory(candidate, failure)) return candidate;
  }
  return {};
}

/** @return the Lottie animation, or an empty path when there is none to find. */
std::filesystem::path resolveLottie(const Options& options) {
  std::vector<std::filesystem::path> candidates;
  std::filesystem::path directory = std::filesystem::current_path();
  for (int depth = 0; depth < 8 && !directory.empty(); ++depth) {
    candidates.push_back(directory / options.resources / "apitest" / "lottie" / "gear.json");
    candidates.push_back(directory / "resources" / "apitest" / "lottie" / "gear.json");
    directory = directory.parent_path();
  }
  return findFile(candidates);
}

/**
 * @return true when the image was written.
 *
 * The picture goes through `Image::encode`, so the demo exercises the codec as well as the rasterizer: the bytes it
 * writes are produced by the same libpng path a client would use.
 */
bool writePng(const graphics::Image& image, const std::string& path) {
  std::string error;
  const graphics::Data encoded = image.encode(graphics::ImageFormat::PNG, 100, &error);
  if (encoded.size() == 0) {
    std::cerr << "could not encode the picture: " << (error.empty() ? "no reason given" : error) << "\n";
    return false;
  }

  std::ofstream file(path, std::ios::binary);
  if (!file.good()) {
    std::cerr << "could not open " << path << " for writing\n";
    return false;
  }

  file.write(reinterpret_cast<const char*>(encoded.bytes()), static_cast<std::streamsize>(encoded.size()));
  if (!file.good()) {
    std::cerr << "could not write " << path << "\n";
    return false;
  }

  std::cout << "wrote " << path << " (" << encoded.size() << " bytes, " << demo::kWidth << "x" << demo::kHeight
            << ")\n";
  return true;
}

/**
 * @return a copy of @p list, field by field.
 *
 * `DrawList`'s copy constructor is **deleted on purpose** (*"a copy would be a second owner of several vectors' worth of
 * geometry, and the hand-off is a move by construction"*), and `DrawList::clone()` is the named copy a caller asks for.
 * `--retained` uses it once, to *measure* the route a caller without `Surface::submitRecording` would have to take: the
 * clone is the middle option between re-recording the scene (~130 ms) and lending the list (nothing).
 */
graphics::gpu::DrawList cloneList(const graphics::gpu::DrawList& list) {
  return list.clone();
}

/**
 * Draws the demo scene, optionally moved by a fraction of a pixel that depends on @p frameIndex.
 *
 * A sub-pixel translation is the smallest change that reaches every device-space coordinate a coverage field is built
 * from, which is what `--jitter` is for: it makes every frame a miss for the cross-frame field cache without changing
 * what the frame *is*. The offset grows with the frame index rather than alternating, so no two frames of a jittered run
 * share geometry -- and frame 0 is not moved at all, so a jittered run's first frame is the same picture an unjittered
 * run writes, which is what makes the two comparable.
 *
 * `--animate` is the other kind of change, and the one an animation actually makes: part of the scene moves while the
 * rest stays exactly where it is. The Lottie panel advances by a tenth of a second a frame, so the gears turn and the
 * 22 SVG documents, the text and the panels do not. The field cache is all-or-nothing over the whole frame, so the
 * *frame* misses -- which is the case a per-field table would answer, and the case `CORAL_TRACE_FIELD_REUSE` was added to
 * measure (4.1.43).
 */
void drawSceneForFrame(graphics::Canvas& canvas, const demo::LoadedResources& loaded, const int frameIndex,
        const bool jitter, const bool animate = false) {
  const auto animationSeconds = static_cast<graphics::Scalar>(frameIndex) * 0.1f;
  if (!jitter || frameIndex == 0) {
    demo::drawScene(canvas, loaded, animate ? animationSeconds : 0.0f);
    return;
  }
  const auto offset = static_cast<graphics::Scalar>(frameIndex);
  canvas.save();
  canvas.concat(graphics::Matrix::MakeTranslation(graphics::Vector3 { 0.37f * offset, 0.11f * offset, 0.0f }));
  demo::drawScene(canvas, loaded, animate ? animationSeconds : 0.0f);
  canvas.restore();
}

} // namespace anonymous

namespace {

/**
 * Draws the scene through a rasterized surface and returns the picture.
 *
 * @return an empty image when the surface or its canvas could not be created.
 */
graphics::Image renderRaster(const demo::LoadedResources& loaded) {
  auto surface = graphics::Surface::makeRaster(demo::kWidth, demo::kHeight);
  if (surface == nullptr) {
    std::cerr << "could not create a " << demo::kWidth << "x" << demo::kHeight << " raster surface\n";
    return {};
  }

  // `getCanvas` is refused twice without a `snapshot` between, so the canvas is taken once and held for the whole
  // scene -- which is how a caller drawing a static picture works.
  const auto canvas = surface->getCanvas();
  if (canvas == nullptr) {
    std::cerr << "the raster surface refused a canvas\n";
    return {};
  }

  canvas->clear(graphics::Color::MakeRGBA8(12, 14, 20, 255));
  demo::drawScene(*canvas, loaded);

  std::cout << "raster: save count " << canvas->saveCount() << " (0 means every save was matched)\n";
  return surface->snapshot();
}

/** What a GPU frame produced: the picture and the numbers a caller would put in an overlay. */
struct GpuFrame {
  /**
   * The readback.
   *
   * A `Bitmap` rather than a `graphics::Image`: the GPU surface's `snapshot` hands back pixels it already owns, and
   * encoding them is the caller's business. The raster surface's `snapshot` returns an `Image` instead, because it
   * has an encoder for them. The two are the same picture through two types, which is a wart worth naming rather
   * than hiding -- `Image::makeBitmap` converts one way and `Image(Bitmap)` the other.
   */
  graphics::Bitmap pixels;
  std::string device;
  uint32_t batches = 0;
  uint32_t draws = 0;
  /**
   * Where the draws went: the coverage field atlases (one per batch), the glyph and bitmap route (one per command), and
   * the stencil route (one per contour plus a cover). The three are parts of @c draws, not alternatives to it.
   *
   * Printed because a frame's cost is per-command and a single draw count cannot say which route to look at: measured on
   * this scene it is what showed the field route drawing 41000 commands one at a time, and afterwards that the remaining
   * calls are elsewhere.
   */
  uint32_t coverageDraws = 0;
  uint32_t imageDraws = 0;
  /** The image route's batches, which is what its draws would collapse to with an image atlas. */
  uint32_t imageBatches = 0;
  uint32_t pathDraws = 0;
  double precompileMs = 0.0;
  double renderMs = 0.0;

  /** @return the picture as the type the PNG writer takes. */
  [[nodiscard]] graphics::Image asImage() const { return graphics::Image::fromBitmap(pixels); }
};

/**
 * Draws the scene through a hardware surface and reads it back.
 *
 * The body is deliberately the same calls as `renderRaster` plus the two a GPU frame needs -- `beginFrame` instead
 * of `getCanvas`, and `submit` before the readback. Nothing else differs, and that is the claim this demo makes: the
 * hardware path is a second implementation of one interface, not a second drawing API.
 *
 * @return the frame, with an empty image when the device or its target could not be brought up.
 */
GpuFrame renderGpu(const demo::LoadedResources& loaded, const backend::Backend preferred, const int frameCount,
        const int jobs = -1, const bool jitter = false, const bool retained = false, const bool animate = false) {
  GpuFrame frame;

  std::cout << "gpu   : creating a " << demo::kWidth << "x" << demo::kHeight << " surface (preferred backend "
            << toString(preferred) << ")\n"
            << std::flush;

  graphics::gpu::SurfaceConfiguration configuration;
  configuration.width = static_cast<uint32_t>(demo::kWidth);
  configuration.height = static_cast<uint32_t>(demo::kHeight);
  configuration.preferred = preferred;
  // The stencil buffer is what lets a path be filled by winding accumulation rather than tessellation, and this scene
  // is full of paths: a triangle, a rounded rectangle, SVG outlines, Lottie shapes.
  configuration.stencil = true;
  configuration.label = "graphics_demo";

  // ## The job pool is opt-in, and the measurement that made it so
  //
  // `SurfaceConfiguration::jobSystem` makes the precompile and coverage-field stages parallel, and setting it looked like
  // the obvious way to show the multi-threaded path this framework is built around. Measured on this scene it was
  // **markedly slower**: precompile went from 62 ms to 2568 ms (that figure is summed over the pool's threads, so it is
  // roughly 171 ms of wall time on each of fifteen -- still about three times the single-threaded total), while the
  // frame's overall cost was unchanged because the precompile was never its bottleneck.
  //
  // That measurement was recorded without a build type, and a later one showed why that matters: the coverage-field
  // stage was measured **3.4x faster** with the pool in Release and **2.6x slower** in Debug, the same code and the same
  // pixels (`路径边缘抗锯齿设计与现状.md` 4.1.9). So the note above is not evidence about a release build, and the
  // question it settles is re-openable with one command rather than an edit:
  //
  //   out/graphics_demo/bin/Release/graphics_demo.exe --backend gpu --frames 3
  //   out/graphics_demo/bin/Release/graphics_demo.exe --backend gpu --frames 3 --jobs 0
  //
  // Whoever makes the parallel path faster than the serial one should turn this on by default and say which build the
  // two lines were measured in. Until then a caller that wants it passes a pool -- `--jobs` here, a `JobSystem` in a
  // test -- and the frame is serial by default.
  //
  // Note what the pool does *not* affect: the recording. A `DrawList` is filled on the calling thread either way, which
  // is the "N recorders + one submitter" model the framework documents rather than a threading mode of this program.
  std::optional<utils::JobSystem> pool;
  if (jobs >= 0) {
    pool.emplace(static_cast<uint32_t>(jobs));
    configuration.jobSystem = &*pool;
    std::cout << "gpu   : job pool with " << pool->workerCount() << " worker threads besides this one\n" << std::flush;
  }

  // ## The backend's own explanation, collected if the surface cannot be made
  //
  // A null return leaves no surface to ask, so the reason has to be captured *during* the attempt. See the failure branch
  // below for what this replaces.
  std::vector<std::string> collected;
  configuration.diagnosticsCallback = [](const backend::MessageSeverity, const char* message, void* userData) {
    auto* lines = static_cast<std::vector<std::string>*>(userData);
    if (message != nullptr && lines != nullptr) lines->emplace_back(message);
  };
  configuration.diagnosticsUserData = &collected;

  auto surface = graphics::gpu::Surface::make(configuration);
  if (surface == nullptr) {
    std::cerr << "no GPU surface could be brought up";
    if (preferred != backend::Backend::Null) {
      std::cerr << " for the requested backend (" << toString(preferred) << ")";
    }
    std::cerr << "\n";
    // ## The reason, which used to be thrown away
    //
    // `make` returning null leaves no surface to ask, so the only place the backend's own explanation can reach a caller
    // is a callback given *before* the attempt -- which is what `SurfaceConfiguration::diagnosticsCallback` is. Without
    // it this message said only "is there a driver?", which was actively misleading when the real cause was a build with
    // no embedded shaders: the driver was fine and the programs the renderer needed did not exist.
    for (const std::string& line: collected) {
      std::cerr << "  backend: " << line << "\n";
    }
    std::cerr << "  (try --backend raster for the software renderer)\n";
    return frame;
  }

  frame.device = surface->describeDevice();

  // ## A fallback is reported, because a silent one makes the option a lie
  //
  // `--gpu-api vulkan` used to print "preferred backend Vulkan", create an OpenGL device, and write a picture that was
  // byte-for-byte identical to `--gpu-api gl` -- four ways to ask for four backends, one backend, and nothing said. A
  // caller comparing the outputs of two APIs would have concluded they agreed.
  //
  // The preferred backend is a *preference*: the RHI picks the best available when the named one is not compiled in or
  // has no driver, which is the right behaviour for a demo. What is not right is doing it quietly. This says which
  // backend was actually created and, when it is not the one asked for, that it was substituted.
  {
    // `Null` is a real backend here (it selects a headless/no-device context), so it is not exempt: asking for it and
    // getting OpenGL is exactly the substitution this is meant to report.
    const bool substituted = frame.device.find(toString(preferred)) == std::string::npos;
    if (substituted) {
      // The device line is printed by the caller after this, so the note goes first and the two read as one report:
      // what was asked for, that it was not what came up, and what did.
      std::cout << "gpu   : note: " << toString(preferred)
                << " was not the backend created, so the pictures below are NOT that API's output\n";
    }
    std::cout << std::flush;
  }

  // ## The retained path: record the scene **once**, submit it every frame
  //
  // `--retained` records the scene into a list the *caller* owns and then hands the surface a copy of it each frame,
  // which is the form a UI with a display list uses -- and the one the measurement of 4.1.38/4.1.39 says is worth having:
  // re-recording the scene costs ~100 ms a frame against `submit`'s ~13 ms, so a caller that keeps its recording pays the
  // copy instead of the recording. That copy is what this flag measures, because it is the whole question: the ~22 MB a
  // `DrawList` holds (41849 commands, 41084 field requests, the flattened points) against 40000 path renders.
  //
  // A recording canvas takes a **null** device on purpose: the recording half never dereferences one, which is the same
  // property the whole record-then-submit design rests on, and it is what lets a caller record without a surface.
  std::shared_ptr<graphics::gpu::DrawList> recorded;
  double retainedCloneMs = 0.0;
  double recordedOnceMs = 0.0;
  if (retained) {
    recorded = std::make_shared<graphics::gpu::DrawList>();
    recorded->label = "graphics_demo/retained";
    std::unique_ptr<graphics::Canvas> recorder =
            graphics::Canvas::createRecordingBackend(demo::kWidth, demo::kHeight, nullptr, recorded);
    if (recorder == nullptr) {
      std::cerr << "the recording backend could not be created\n";
      return frame;
    }
    recorder->clear(graphics::Color::MakeRGBA8(12, 14, 20, 255));
    // Timed, because this one recording *is* what a caller without a display list pays every frame (4.1.39).
    const auto onceStart = std::chrono::steady_clock::now();
    drawSceneForFrame(*recorder, loaded, 0, jitter, animate);
    recordedOnceMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - onceStart).count();
    std::cout << "gpu   : retained: recorded the scene once in " << recordedOnceMs << " ms into a "
              << recorded->commands.size() << "-command list\n";
  }

  graphics::Canvas* canvas = surface->beginFrame();
  if (canvas == nullptr) {
    std::cerr << "the GPU surface refused a frame\n";
    return frame;
  }

  if (retained) {
    // The retained path: the surface **borrows** the list (`submitRecording`), so the frame pays neither a re-recording
    // nor a copy. The clone is measured once rather than every frame, because it is the price of the *other* way to keep
    // a recording -- the one a caller without `submitRecording` has to pay, which 4.1.40 measured per frame at 3.6-4.3 ms.
    const auto probeStart = std::chrono::steady_clock::now();
    graphics::gpu::DrawList probe = cloneList(*recorded);
    retainedCloneMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - probeStart).count();
    probe.clear();
    if (!surface->submitRecording(*recorded)) {
      std::cerr << "the surface refused the recorded list\n";
      return frame;
    }
  } else {
    canvas->clear(graphics::Color::MakeRGBA8(12, 14, 20, 255));
    // The scene, moved by frame index when `--jitter` asks for it -- see `Options::jitter`. The first frame moves by
    // nothing, so a jittered run's first frame is the same picture as an unjittered one and the two PNGs are comparable.
    drawSceneForFrame(*canvas, loaded, 0, jitter, animate);
  }

  if (!surface->submit()) {
    std::cerr << "the GPU device refused the frame:\n" << surface->diagnostics() << "\n";
    return frame;
  }

  frame.batches = surface->lastFrameBatchCount();
  frame.draws = surface->lastFrameDrawCallCount();
  frame.coverageDraws = surface->lastFrameCoverageDrawCallCount();
  frame.imageDraws = surface->lastFrameImageDrawCallCount();
  frame.imageBatches = surface->lastFrameImageBatchCount();
  frame.pathDraws = surface->lastFramePathDrawCallCount();
  frame.precompileMs = surface->lastFramePrecompileMilliseconds();
  frame.renderMs = surface->lastFrameRenderMilliseconds();
  frame.pixels = surface->snapshot();

  std::cout << "gpu   : " << frame.device << "\n"
            << "        batches " << frame.batches << ", draws " << frame.draws << " (coverage " << frame.coverageDraws
            << ", glyphs/images " << frame.imageDraws << " in " << frame.imageBatches << " batches, stencil paths "
            << frame.pathDraws << "), precompile "
            << frame.precompileMs << "ms (summed), frame " << frame.renderMs << "ms\n";
  // ## The frame's stages, which is what the total above has to be read against
  //
  // The single total was the defect this replaced: it was labelled "upload+record+execute" while it in fact measured the
  // whole of `submit`, field generation included, so a frame reported as 70 ms looked like a recording cost while 50 ms
  // of it was the rasteriser. Printing the stages in the order they run is what makes a number that moved attributable,
  // and the sum of the parts is expected to be *less* than the total -- the difference is the bookkeeping between them.
  const graphics::gpu::FrameStages stages = surface->lastFrameStages();
  std::cout << "        stages fields " << stages.fields << ", placeImages " << stages.placeImages << ", precompile "
            << stages.precompile << ", targets " << stages.targets << ", arena " << stages.arena << ", vertexUpload "
            << stages.vertexUpload << ", record " << stages.record << ", execute " << stages.execute << ", total "
            << stages.total << "ms\n";
  // ## The coverage-field stage, which is the one that can be parallel and the one a path-heavy scene pays for
  //
  // Printed because a frame's cost is otherwise one number with no way to attribute it: this scene is thousands of
  // paths, each of which is recorded as a field *request* and rasterised by the renderer before anything is drawn
  // (`GpuRenderer::resolveCoverageFields`). The count is what the time has to be read against, and the atlas count is
  // what says whether the frame's fields needed an upload at all -- a frame that changed uploads one atlas, and a frame
  // that did not uploads nothing; see the note on `GpuRenderer::FieldAtlas`.
  std::cout << "        coverage fields generated " << surface->lastFrameCoverageFieldCount() << " (reused "
            << surface->lastFrameCoverageFieldsReused() << ") in "
            << surface->lastFrameCoverageFieldMilliseconds() << "ms (rasterising, wall clock) + "
            << surface->lastFrameCoveragePackMilliseconds() << "ms (packing the atlas, serial), covering "
            << (surface->lastFrameCoverageFieldArea() / 1024u) << " KiB of texels, pooled field atlases "
            << surface->pooledFieldAtlasCount() << "\n";
  // The frame's bandwidth, which is what a texture-format change moves: every payload a frame sends is copied into the
  // frame's arena, so this is the number that says what the coverage fields cost in bytes rather than in milliseconds.
  std::cout << "        uploaded " << (surface->lastFrameUploadBytes() / 1024u) << " KiB of vertex, uniform and texture data\n";

  // ## The frames after the first, which is where a UI actually lives
  //
  // One frame cannot show what a cross-frame cache does, and it is also not what the surface is for: a UI draws the same
  // scene sixty times a second with a few things moving. This draws the scene `--frames` times, reports each frame, and
  // reports the *cumulative* texture uploads, which is the number the content-keyed image cache exists to keep flat.
  //
  // The uploads are the whole point of the phase: the first frame uploads every glyph and image it draws, and every
  // later frame of an unchanged scene uploads **nothing**, because the cache is keyed on the pixels rather than on an
  // image index that a fresh frame's list renumbers from zero.
  if (frameCount > 1) {
    const uint64_t afterFirst = surface->textureUploadCount();
    double totalRenderMs = 0.0;
    // ## And the stage that is *not* `submit`: recording the scene
    //
    // Everything above times `submit` -- the renderer's own work, from the field stage to the driver's execution. A frame
    // also has to be *recorded*, which is what `Canvas::drawScene` does: flattening paths, rasterising a coverage bitmap
    // per glyph and appending the commands. That work is the caller's, it runs on the same thread, and until this line
    // existed nothing in the report measured it -- which made the demo's per-frame cost read as 13.7 ms when the same
    // 1000x1160 scene cost ~150 ms per frame end to end. The two numbers belong side by side for the same reason the
    // field stage's two halves do: a change that moves one of them says nothing about the frame unless the other is read.
    double totalRecordMs = 0.0;
    // The steady-state stages, summed over the frames after the first and reported per frame. The first frame is not the
    // one a UI lives in: it creates the layer targets, uploads every glyph and image and fills the field atlas, so its
    // breakdown says what the *startup* costs. The average below is the number a change should be judged against.
    graphics::gpu::FrameStages stageSums;
    int measured = 0;
    uint64_t reusedFields = 0;
    uint64_t askedFields = 0;
    // The animation-reuse probe's totals, all zero unless `CORAL_TRACE_FIELD_REUSE=<depth>` is set -- see
    // `FieldReuseProbe`. `windowDepth` is the same on every frame, so the last frame's value is the run's.
    uint64_t windowHits = 0;
    uint64_t windowRequests = 0;
    uint32_t windowDepth = 0;
    for (int index = 1; index < frameCount; ++index) {
      const auto recordStart = std::chrono::steady_clock::now();
      graphics::Canvas* next = surface->beginFrame();
      if (next == nullptr) break;
      if (retained) {
        // The retained path's per-frame work: hand the surface the list it already has. Nothing is copied -- see
        // `Surface::submitRecording` -- so what is left is `beginFrame`'s empty list and the call itself.
        if (!surface->submitRecording(*recorded)) break;
      } else {
        next->clear(graphics::Color::MakeRGBA8(12, 14, 20, 255));
        drawSceneForFrame(*next, loaded, index, jitter, animate);
      }
      totalRecordMs +=
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - recordStart).count();
      if (!surface->submit()) break;
      totalRenderMs += surface->lastFrameRenderMilliseconds();
      const graphics::gpu::FrameStages stages = surface->lastFrameStages();
      stageSums.fields += stages.fields;
      stageSums.placeImages += stages.placeImages;
      stageSums.precompile += stages.precompile;
      stageSums.targets += stages.targets;
      stageSums.arena += stages.arena;
      stageSums.vertexUpload += stages.vertexUpload;
      stageSums.record += stages.record;
      stageSums.execute += stages.execute;
      stageSums.total += stages.total;
      reusedFields += surface->lastFrameCoverageFieldsReused();
      askedFields += surface->lastFrameCoverageFieldCount();
      windowHits += surface->lastFrameFieldReuseWindowHits();
      windowRequests += surface->lastFrameFieldReuseWindowRequests();
      windowDepth = surface->lastFrameFieldReuseWindowDepth();
      ++measured;
    }
    const uint64_t later = surface->textureUploadCount() - afterFirst;
    std::cout << "gpu   : " << frameCount << " frames: first frame uploaded " << afterFirst
              << " textures, the remaining " << (frameCount - 1) << " uploaded " << later
              << "; average frame " << (totalRenderMs / static_cast<double>(frameCount - 1)) << "ms\n";
    if (measured > 0) {
      const auto perFrame = [measured](const double sum) { return sum / static_cast<double>(measured); };
      std::cout << "        stages (average of the frames after the first): fields " << perFrame(stageSums.fields)
                << ", placeImages " << perFrame(stageSums.placeImages) << ", precompile "
                << perFrame(stageSums.precompile) << ", targets " << perFrame(stageSums.targets) << ", arena "
                << perFrame(stageSums.arena) << ", vertexUpload " << perFrame(stageSums.vertexUpload) << ", record "
                << perFrame(stageSums.record) << ", execute " << perFrame(stageSums.execute) << ", total "
                << perFrame(stageSums.total) << "ms\n";
      std::cout << "        coverage fields: " << reusedFields << " of " << askedFields
                << " reused across those frames\n";
      // The probe's answer, printed only when it was asked for -- and printed as the fraction that decides the question,
      // because "would a table keyed on each field's content pay" is a ratio and not a count.
      if (windowDepth > 0 && windowRequests > 0) {
        std::cout << "        field reuse in a window of " << windowDepth << " frames: " << windowHits << " of "
                  << windowRequests << " field requests are content-identical to one of the previous " << windowDepth
                  << " frames (" << 100.0 * static_cast<double>(windowHits) / static_cast<double>(windowRequests)
                  << "%)\n";
      }
      // Recording next to submitting, because the frame a caller pays for is their sum -- and on this scene the first is
      // ten times the second. See the note where the timer is taken.
      if (retained) {
        // The comparison is inside one run: the one-time recording above is exactly what a caller *without* a display list
        // pays every frame, and borrowing is what this caller pays instead. The clone's cost is reported beside them
        // because it is the middle option -- the one `addDrawList` needs, and the one `clone()` now offers explicitly.
        const double handoffMs = perFrame(totalRecordMs);
        const double submitMs = perFrame(stageSums.total);
        std::cout << "        retained: borrow " << handoffMs << "ms + submit " << submitMs << "ms = "
                  << handoffMs + submitMs << "ms per frame, against " << recordedOnceMs + submitMs
                  << "ms re-recording the scene each frame (" << (recordedOnceMs + submitMs) / (handoffMs + submitMs)
                  << "x); cloning the list instead would add " << retainedCloneMs << "ms\n";
      } else {
        std::cout << "        recording the scene " << perFrame(totalRecordMs) << "ms per frame, submitting "
                  << perFrame(stageSums.total) << "ms: the renderer is " << perFrame(stageSums.total) /
                          (perFrame(totalRecordMs) + perFrame(stageSums.total)) * 100.0 << "% of a frame\n";
      }
    }
  }
  return frame;
}

/**
 * Reports how far apart two pictures are, and where.
 *
 * A count of differing pixels is not enough to act on: "one shape is missing" and "every edge is a step off" produce
 * similar counts and have nothing in common as defects. So the worst offender is named, and the budget is compared
 * against the picture's *ink* rather than its area -- an empty background cannot dilute the fraction and make a
 * systematic disagreement look small.
 */
bool comparePictures(const graphics::Bitmap& softwarePixels, const graphics::Bitmap& hardwarePixels) {
  if (softwarePixels.width() != hardwarePixels.width() || softwarePixels.height() != hardwarePixels.height()) {
    std::cout << "compare: different sizes (" << softwarePixels.width() << "x" << softwarePixels.height()
              << " against " << hardwarePixels.width() << "x" << hardwarePixels.height() << ")\n";
    return false;
  }

  if (softwarePixels.isEmpty() || hardwarePixels.isEmpty()) {
    std::cout << "compare: one of the two pictures is empty\n";
    return false;
  }

  const int width = static_cast<int>(softwarePixels.width());
  const int height = static_cast<int>(softwarePixels.height());
  const auto pixelAt = [](const graphics::Bitmap& bitmap, const int x, const int y) -> uint32_t {
    return bitmap.makePixmap().getPixelARGB32(x, y);
  };

  size_t differing = 0;
  size_t inked = 0;
  int worstDifference = 0;
  int worstX = 0;
  int worstY = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint32_t a = pixelAt(softwarePixels, x, y);
      const uint32_t b = pixelAt(hardwarePixels, x, y);
      // A pixel counts as inked when *either* side drew something, so a shape present on one renderer only counts
      // against the budget rather than being excused as background.
      if (((a | b) >> 24) != 0) ++inked;

      int worstChannel = 0;
      for (int shift = 0; shift < 32; shift += 8) {
        const int difference =
                std::abs(static_cast<int>((a >> shift) & 0xFFu) - static_cast<int>((b >> shift) & 0xFFu));
        if (difference > worstChannel) worstChannel = difference;
      }
      if (worstChannel > worstDifference) {
        worstDifference = worstChannel;
        worstX = x;
        worstY = y;
      }
      // A per-channel budget rather than an exact match: the two rasterizers compute edge coverage differently, so
      // requiring equality would report every antialiased edge and hide the real differences among them.
      if (worstChannel > 32) ++differing;
    }
  }

  const double fraction = inked == 0 ? 0.0 : static_cast<double>(differing) / static_cast<double>(inked);
  std::cout << "compare: " << inked << " inked pixels, " << differing << " differ by more than 32/255 ("
            << static_cast<int>(fraction * 100.0) << "%), worst " << worstDifference << "/255 at (" << worstX << ", "
            << worstY << ")\n";

  // A tenth is generous for edge antialiasing and still far below what a missing shape costs: one absent panel is a
  // fifth of this scene's ink, so a shape that did not draw cannot hide inside it.
  const bool agree = fraction < 0.10;
  std::cout << "compare: the two renderers " << (agree ? "agree within the edge budget" : "DISAGREE") << "\n";
  return agree;
}

} // namespace anonymous

int main(int argc, char** argv) {
  auto operateStarted = std::chrono::steady_clock::now();
  const Options options = parseArguments(argc, argv);
  if (options.help) {
    printUsage();
    return EXIT_SUCCESS;
  }

  // Resolve everything before drawing, so the notes about what was missing can be drawn into the picture.
  demo::Resources wanted;
  wanted.fontPath = resolveFont(options).string();
  wanted.emojiFontPath = resolveEmojiFont(options).string();
  wanted.imagePath = resolveImage(options).string();
  wanted.secondImagePath = resolveSecondImage(options).string();
  wanted.svgDirectory = resolveSvgDirectory(options).string();
  wanted.lottiePath = resolveLottie(options).string();

  std::cout << "resources:\n"
            << "  font  : " << (wanted.fontPath.empty() ? "(none found)" : wanted.fontPath) << "\n"
            << "  emoji : " << (wanted.emojiFontPath.empty() ? "(none found)" : wanted.emojiFontPath) << "\n"
            << "  image : " << (wanted.imagePath.empty() ? "(none found)" : wanted.imagePath) << "\n"
            << "  svg   : " << (wanted.svgDirectory.empty() ? "(none found)" : wanted.svgDirectory) << "\n"
            << "  lottie: " << (wanted.lottiePath.empty() ? "(none found)" : wanted.lottiePath) << "\n";

  const demo::LoadedResources loaded = demo::load(wanted);
  for (const std::string& note: loaded.notes) std::cout << "  note  : " << note << "`n";
  std::cout << std::flush;

  // `--compare` draws with both renderers and reports whether they agree, which is the one thing a renderer cannot
  // tell you about itself. Not the default, because it needs a driver and the demo has to run without one.
  if (options.compare) {
    const graphics::Image software = renderRaster(loaded);
    if (software.isEmpty()) {
      std::cerr << "the raster frame produced no image, so there is nothing to compare against\n";
      return EXIT_FAILURE;
    }
    const GpuFrame hardware = renderGpu(loaded, options.backend, 1, options.jobs);
    if (hardware.pixels.isEmpty()) {
      std::cerr << "the GPU frame produced no image\n";
      return EXIT_FAILURE;
    }
    // Both are written, because when the comparison fails the next question is always "which one, and how".
    if (!writePng(software, "coral_graphics_demo_raster.png")) return EXIT_FAILURE;
    // Image(Bitmap) is explicit, so the conversion is named rather than implicit -- the two types are one picture
    // through two APIs and it is worth being able to grep for where they meet.
    if (!writePng(hardware.asImage(), "coral_graphics_demo_gpu.png")) return EXIT_FAILURE;

    const bool agree = comparePictures(software.peekBitmap(), hardware.pixels);
    auto compared = std::chrono::steady_clock::now();
    std::cout << "cost time: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(compared - operateStarted).count() << "ms.\n";
    return agree ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  if (options.renderer == Renderer::Gpu) {
    const GpuFrame frame = renderGpu(loaded, options.backend, options.frames, options.jobs, options.jitter,
            options.retained, options.animate);
    if (frame.pixels.isEmpty()) return EXIT_FAILURE;
    if (!writePng(frame.asImage(), options.output)) return EXIT_FAILURE;
  } else {
    const graphics::Image picture = renderRaster(loaded);
    if (picture.isEmpty()) {
      std::cerr << "the snapshot produced no image\n";
      return EXIT_FAILURE;
    }
    if (!writePng(picture, options.output)) return EXIT_FAILURE;
  }

  auto now = std::chrono::steady_clock::now();
  std::cout << "canvas: " << demo::kWidth << "x" << demo::kHeight << ", cost time: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(now - operateStarted).count() << "ms.\n";

  return EXIT_SUCCESS;
}



