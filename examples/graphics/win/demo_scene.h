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

#ifndef CORAL_GRAPHICS_DEMO_SCENE_H
#define CORAL_GRAPHICS_DEMO_SCENE_H

#include <graphics/Canvas.h>
#include <graphics/Font.h>
#include <graphics/Image.h>
#include <graphics/Surface.h>

#include <memory>
#include <lottie/Lottie.h>
#include <svg/Svg.h>

#include <string>
#include <vector>

namespace demo {

/** The size of the picture the demo draws. */
inline constexpr int kWidth = 1000;
inline constexpr int kHeight = 1160;

/**
 * The layout constants, in one place because they have to agree.
 *
 * `kTop` leaves room for the heading, the sub-line and the rule; `kFooterRoom` leaves room for the footer. The four
 * rows and the three gaps divide what is left, and each panel's body is its card less the title strip and the inset.
 * The first version of this file had the same structure with different numbers and a body 96 px tall, which was not
 * enough for the text panel and did not match the grid the transform panel drew.
 */
inline constexpr int kHeadingRoom = 150;
inline constexpr int kFooterRoom = 46;

/**
 * What the demo needs from disk.
 *
 * Passed in rather than hard-coded, because a demo that only runs from one directory is a demo nobody runs. The
 * caller resolves the paths and reports what it could not find, so a missing resource produces a sentence rather than
 * a blank rectangle.
 */
struct Resources {
  /** A sans-serif font. Any Latin-capable file works; the demo also tries it for CJK, which is optional. */
  std::string fontPath;

  /** A photograph or illustration. Uses the JPEG and PNG paths of the codec. */
  std::string imagePath;

  /** A second image, drawn smaller and blended. Optional; the demo skips the panel without it. */
  std::string secondImagePath;

  /** The directory of SVG documents, drawn by the SVG panel. Optional; the panel is skipped without it. */
  std::string svgDirectory;

  /** A Lottie animation, drawn by the Lottie panel. Optional; the panel is skipped without it. */
  std::string lottiePath;

  /** A colour-emoji font, used by the text panel. Optional; the emoji row is skipped without it. */
  std::string emojiFontPath;
};

/** What the demo managed to load, so the scene can adapt rather than fail. */
struct LoadedResources {
  std::shared_ptr<graphics::Font> titleFont;
  std::shared_ptr<graphics::Font> bodyFont;

  /** The colour-emoji font at emoji-row size, or null. */
  std::shared_ptr<graphics::Font> emojiFont;
  graphics::Image image;
  graphics::Image secondImage;

  /**
   * The SVG documents that were found, in the order they were read.
   *
   * The documents themselves rather than a directory path, because parsing is the thing that can fail -- a directory that
   * exists may still hold a file that does not parse -- and doing it once at load time means the panel draws rather than
   * reporting an error per frame.
   */
  std::vector<graphics::Svg> svgDocuments;
  std::vector<std::string> svgNames;

  /** The Lottie animation, or an empty one. */
  graphics::Lottie lottie;

  /** A sentence for each resource that could not be loaded. Empty when everything was found. */
  std::vector<std::string> notes;

  [[nodiscard]] bool hasSvg() const noexcept { return !svgDocuments.empty(); }
  [[nodiscard]] bool hasLottie() const noexcept { return !lottie.isEmpty(); }
  [[nodiscard]] bool hasImage() const noexcept { return !image.isEmpty(); }
  [[nodiscard]] bool hasSecondImage() const noexcept { return !secondImage.isEmpty(); }
  [[nodiscard]] bool hasFonts() const noexcept { return titleFont != nullptr && bodyFont != nullptr; }
  [[nodiscard]] bool hasEmoji() const noexcept { return emojiFont != nullptr; }
};

/**
 * @return the resources that could be loaded from @p wanted, with a note for each that could not.
 *
 * Every resource is optional. The demo is about what the library can draw, and a missing font should not stop a caller
 * from seeing that it can draw shapes -- so a failure is recorded and the scene adapts.
 */
[[nodiscard]] LoadedResources load(const Resources& wanted);

/**
 * Draws the whole demonstration onto @p canvas.
 *
 * The scene is organised as labelled panels, so a reader can find the part they care about and read just that
 * function. Each panel is a separate function below it.
 *
 * @param animationSeconds shifts the Lottie panel's four stills in time, which is what makes the scene *animate*: the
 *        gears turn while every other panel stays exactly where it is. At zero -- the default, and every run compared
 *        against a baseline -- the picture is the one the stills have always been.
 */
void drawScene(graphics::Canvas& canvas, const LoadedResources& resources, graphics::Scalar animationSeconds = 0.0f);

} // namespace demo

#endif //CORAL_GRAPHICS_DEMO_SCENE_H
