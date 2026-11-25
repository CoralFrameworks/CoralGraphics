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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file SvgDom.cpp
 *
 * Building the document tree from parsed XML, and drawing it.
 *
 * ## Two passes, and why
 *
 * The tree is built first and indexed second, rather than resolving references as elements are read. `use` may appear
 * before the element it names -- and in the corpus it does, since `defs` usually comes first but nothing requires it --
 * so a reference resolved during the build would be a dangling pointer half the time and a silent one the rest.
 *
 * ## Geometry is built in user space
 *
 * Every shape is turned into a `graphics::Path` in the coordinate system its element is written in, and the accumulated
 * transform goes to the canvas. Nothing here multiplies a point by a matrix except where SVG's own rules require it --
 * `use`'s translation, and the viewBox fit.
 */

#include "svg/SvgDocument.h"

#include "xml/Xml.h"

#include <graphics/Bitmap.h>
#include <graphics/FontManager.h>
#include <graphics/Surface.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace graphics::svg {
namespace {

/**
 * @return the typeface to draw @p family with.
 *
 * The system's font database is the only source: an SVG `@font-face` would need a font file fetched over the network,
 * which this renderer has no business doing. A family that is not installed falls back to the default face rather than
 * drawing nothing, because a picture with the wrong typeface is more useful than an empty one.
 */
std::shared_ptr<Typeface> resolveTypeface(const std::string& family, const svg::FontStyle style,
                                         const svg::FontWeight weight) {
  FontManager& manager = FontManager::shared();

  // The CSS weight scale is the OpenType one, so the keyword values are the numbers the font database compares against.
  // Qualified as `graphics::FontWeight` because `svg::FontWeight` -- the keyword enum this switch is over -- is the
  // nearer name and would otherwise shadow it.
  int wantedWeight = graphics::FontWeight::kNormal;
  switch (weight) {
    case svg::FontWeight::Bold: wantedWeight = graphics::FontWeight::kBold; break;
    case svg::FontWeight::Bolder: wantedWeight = graphics::FontWeight::kExtraBold; break;
    case svg::FontWeight::Lighter: wantedWeight = 200; break;
    case svg::FontWeight::W100: wantedWeight = 100; break;
    case svg::FontWeight::W200: wantedWeight = 200; break;
    case svg::FontWeight::W300: wantedWeight = 300; break;
    case svg::FontWeight::W400: wantedWeight = 400; break;
    case svg::FontWeight::W500: wantedWeight = 500; break;
    case svg::FontWeight::W600: wantedWeight = graphics::FontWeight::kSemiBold; break;
    case svg::FontWeight::W700: wantedWeight = graphics::FontWeight::kBold; break;
    case svg::FontWeight::W800: wantedWeight = graphics::FontWeight::kExtraBold; break;
    case svg::FontWeight::W900: wantedWeight = 900; break;
    case svg::FontWeight::Normal: break;
  }

  // `italic` and `oblique` are kept apart, because a family that ships no italic is a different request from one asked
  // to synthesise a slant -- and the font database decides which it can satisfy.
  const graphics::FontStyle::Slant slant = style == svg::FontStyle::Italic  ? graphics::FontStyle::Slant::Italic
                                           : style == svg::FontStyle::Oblique ? graphics::FontStyle::Slant::Oblique
                                                                              : graphics::FontStyle::Slant::Upright;
  const graphics::FontStyle wanted {wantedWeight, slant};

  if (!family.empty()) {
    if (const auto matched = manager.matchFamilyStyle(family, wanted)) return matched;
  }
  return manager.defaultTypeface();
}

// ==================================================================================================
// State merging
// ==================================================================================================

/**
 * Applies a `Paint` over the inherited colour and presence.
 *
 * `none` clears the paint rather than leaving it inherited -- that is the whole reason SVG has the keyword -- and
 * `currentColor` reads the `color` property, which is itself inherited and may have been set by an ancestor.
 */
void applyPaint(const std::optional<Paint>& paint, const Color& currentColor, const bool inheritedPresent,
                Color& color, bool& present, std::string& iri) {
  if (!paint) {
    present = inheritedPresent;
    iri.clear();
    return;
  }
  switch (paint->type) {
    case Paint::Type::None:
      present = false;
      iri.clear();
      break;
    case Paint::Type::Color:
      color = paint->color;
      present = true;
      iri.clear();
      break;
    case Paint::Type::CurrentColor:
      color = currentColor;
      present = true;
      iri.clear();
      break;
    case Paint::Type::IRI:
      // The reference is carried rather than resolved: a gradient written in the bounding box of whatever it fills
      // cannot become a shader until there is a shape and a box, which is at draw time and not here.
      //
      // The colour beside the reference is SVG's own fallback for one that cannot be resolved, and it becomes the
      // paint's colour -- so a reference that turns out to be unusable, or to name a pattern this renderer does not
      // implement, leaves the shape in the fallback colour rather than unfilled.
      iri = paint->iri;
      if (paint->fallback) {
        color = *paint->fallback;
        present = true;
      } else {
        // ## No fallback: the paint server *is* the colour, and the colour must still be reset
        //
        // `present` false says "there is no colour to paint with", which is the right answer and what makes an
        // unresolvable reference leave the shape unpainted. But `color` is not a comment -- it is the paint's colour,
        // and for a shader-masked fill the paint's colour is exactly what carries the alpha into the shader
        // (`Paint::shader`: "`color` is used only for its alpha"). Leaving the inherited value there meant a
        // translucent parent faded every gradient inside it, which the document never asked for: `fill` is inherited
        // as a whole *value*, so `fill="url(#g)"` does not inherit the parent's colour at all.
        //
        // Reset to an **opaque** colour rather than to transparent. Transparent is the tempting reading of "no colour",
        // and it is a colour with alpha zero by this library's own definition (`parseColor` says so where it handles
        // the `transparent` keyword) -- which would make every gradient invisible. The RGB is the identity here because
        // a shader supplies the colour; white is the value that says so.
        color = Color::White();
        present = false;
      }
      break;
  }
}

/** Merges a node's presentation over the inherited state. */
RenderState mergeState(const RenderState& parent, const Presentation& presentation) {
  RenderState state = parent;

  if (presentation.color) state.currentColor = *presentation.color;
  if (presentation.transform) state.transform = parent.transform * *presentation.transform;
  if (presentation.visibility) state.visibility = *presentation.visibility;
  if (presentation.fontSize) state.fontSize = *presentation.fontSize;
  if (presentation.fontFamily) state.fontFamily = *presentation.fontFamily;
  if (presentation.fontStyle) state.fontStyle = *presentation.fontStyle;
  if (presentation.fontWeight) state.fontWeight = *presentation.fontWeight;
  if (presentation.textAnchor) state.textAnchor = *presentation.textAnchor;
  if (presentation.fillRule) state.fillRule = *presentation.fillRule;

  if (presentation.opacity) state.opacity = parent.opacity * *presentation.opacity;
  if (presentation.clipPath) state.clipPathIri = *presentation.clipPath;
  if (presentation.mask) state.maskIri = *presentation.mask;

  // Carried rather than resolved, exactly as the mask is: a filter's region may be measured against the element's own
  // box, which the element does not know until it is drawn.
  //
  // A filter is a reference until the element is drawn, exactly as a mask is: its region may be measured against the
  // element's own box, which is not known until then.
  //
  // ## What stood between this line and a working wire
  //
  // Three bugs. Each was invisible to the tests that existed when it was introduced, and each was found only by
  // measuring -- two of them after I had spent a round reasoning about the wrong thing.
  //
  //   1. `scalarList` used `parseNumber` on a list, and `parseNumber` requires its whole input to be one number. So
  //      `feColorMatrix`'s twenty values came out empty, every matrix stayed zero, and every filtered element rendered
  //      as nothing. It looked correct in isolation because a direct call to `applyFilter` bypassed the parser.
  //
  //   2. The reference travelled down the render state into the children, so every descendant of a filtered group
  //      re-evaluated the whole filter for itself. One render of `complex7.svg` called `applyFilter` **664 times** --
  //      some 1400 seconds at its native 1090x2026. Clearing it once, in `GroupNode::render`, took that to **20**, the
  //      corpus test from over 600 seconds to 47.8, and the whole suite to under 90. Two rounds had been spent
  //      optimising *how* a filter is evaluated when the cost was *how often*: the byte-addressing path and the region
  //      confinement both stay, and both are verified correct, but neither was the fix.
  //
  //   3. An `objectBoundingBox` region was resolved against an empty box. A group has no geometry, so
  //      `GroupNode::render` passes an empty rectangle and the region came out `(0, 0, 0, 0)`, drawing nothing -- and
  //      seven of this corpus's twelve filter references are on a group. The comment beside that call already said the
  //      region "falls back to the viewport"; the code did not.
  //
  // Plus one that is a space rather than a bug: the object bounds arrive in the coordinates the element's geometry was
  // written in, and are mapped into device space in `drawFiltered`, where the transform is known.
  if (presentation.filter) state.filterIri = *presentation.filter;

  applyPaint(presentation.fill, state.currentColor, state.hasFill, state.fillColor, state.hasFill, state.fillIri);
  if (presentation.fillOpacity) state.fillOpacity = parent.fillOpacity * *presentation.fillOpacity;

  applyPaint(presentation.stroke, state.currentColor, state.hasStroke, state.strokeColor, state.hasStroke,
             state.strokeIri);
  if (presentation.strokeOpacity) state.strokeOpacity = parent.strokeOpacity * *presentation.strokeOpacity;
  if (presentation.strokeWidth) state.strokeWidth = *presentation.strokeWidth;
  if (presentation.strokeCap) state.strokeCap = *presentation.strokeCap;
  if (presentation.strokeJoin) state.strokeJoin = *presentation.strokeJoin;
  if (presentation.strokeMiterLimit) state.strokeMiterLimit = *presentation.strokeMiterLimit;
  if (presentation.strokeDashArray) state.strokeDashArray = *presentation.strokeDashArray;

  return state;
}

/**
 * A layer rectangle large enough to cover anything the caller can draw.
 *
 * A layer's bounds are only a clip: the canvas intersects them with the clip already in force, so this does not have to
 * be accurate -- it has to be *large*. Working out the real extent would mean un-transforming the device clip back into
 * caller space, which is a matrix inverse spent on a value that is then intersected away.
 */
const Rect kUnboundedLayer = Rect::MakeLTRB(-1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f);

/** A paint carrying only an alpha, which is all a layer reads from it. */
graphics::Paint layerPaint(const Scalar alpha) {
  const auto byte = static_cast<uint8_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
  graphics::Paint paint;
  paint.color = Color::MakeRGBA8(255, 255, 255, byte);
  return paint;
}

/**
 * @return the clip path an id refers to, or null when there is none.
 *
 * The lookup asks the node what it is rather than consulting a table, so a `clip-path` naming the wrong kind of element
 * is a clip that does not happen rather than a crash.
 */
const ClipPathNode* resolveClip(const std::string& iri, const RenderState& state) {
  if (iri.empty() || state.ids == nullptr) return nullptr;
  return dynamic_cast<const ClipPathNode*>(state.ids->find(iri));
}

/**
 * Intersects @p canvas's clip with the `clip-path` @p state refers to, if any.
 *
 * @param objectBounds the box an `objectBoundingBox` clip is measured against, or nothing when there is no such box --
 *        a group has no geometry of its own, and the union of its children's bounds is not tracked here. An
 *        `objectBoundingBox` clip on a group is therefore *skipped* rather than applied against a box that would be
 *        wrong; the default for `clipPathUnits` is `userSpaceOnUse`, so this is the uncommon case.
 *
 * The reference is cleared once applied, because the clip belongs to the element that named it: a descendant inheriting
 * the id would otherwise clip itself a second time, against a different box, narrowing the region for no reason the
 * document asked for.
 */
void applyClip(Canvas& canvas, RenderState& state, const std::optional<Rect>& objectBounds) {
  const ClipPathNode* const clip = resolveClip(state.clipPathIri, state);
  if (clip == nullptr || clip->path().isEmpty()) {
    state.clipPathIri.clear();
    return;
  }

  Path path = clip->path();
  if (clip->usesObjectBoundingBox()) {
    if (!objectBounds) {
      state.clipPathIri.clear();
      return;
    }
    path.transform(Matrix::MakeTranslation(Vector3 {objectBounds->getLeft(), objectBounds->getTop(), 0}) *
                   Matrix::MakeScale(Vector3 {objectBounds->getWidth(), objectBounds->getHeight(), 1}));
  }
  canvas.clipPath(path);
  state.clipPathIri.clear();
}

/**
 * Renders @p pattern's content into a tile and wraps it in a repeating shader.
 *
 * Three coordinate systems meet here, and keeping them straight is the whole of it:
 *
 *   * the *tile rectangle*, in the drawing's coordinates once the pattern's unit system has scaled it;
 *   * the *content*, in a system that may be the drawing's or the element's box;
 *   * the *tile bitmap*, whose pixels are one per drawing unit of the tile rectangle.
 *
 * The content is rendered with a transform that carries it into the tile's own pixels, and the shader is given the
 * inverse arrangement -- where the tile sits in the drawing -- so that sampling a point in the drawing lands on the right
 * pixel of the right repetition.
 */
std::shared_ptr<Shader> buildPatternShader(const PatternNode& pattern, const Rect& objectBounds,
                                           const RenderState& state) {
  const PatternNode::Tile& tile = pattern.tile();

  // The tile rectangle. Under `objectBoundingBox` its numbers are fractions of the element's box, which is the same
  // distinction masks have: a *number* there is a fraction, where in user space it is a unit.
  Scalar tileX = 0;
  Scalar tileY = 0;
  Scalar tileWidth = 0;
  Scalar tileHeight = 0;
  if (tile.objectBoundingBox) {
    LengthContext unit;
    unit.viewportWidth = 1;
    unit.viewportHeight = 1;
    const auto resolve = [&unit](const std::optional<Length>& length, const Scalar fallback) {
      return length ? unit.resolve(*length, LengthUnit::Px).value_or(fallback) : fallback;
    };
    const Scalar declaredWidth = resolve(tile.width, 0.0f);
    const Scalar declaredHeight = resolve(tile.height, 0.0f);
    tileX = objectBounds.getLeft() + resolve(tile.x, 0.0f) * objectBounds.getWidth();
    tileY = objectBounds.getTop() + resolve(tile.y, 0.0f) * objectBounds.getHeight();
    tileWidth = declaredWidth * objectBounds.getWidth();
    tileHeight = declaredHeight * objectBounds.getHeight();
  } else {
    const auto resolve = [&state](const std::optional<Length>& length, const Scalar fallback) {
      return length ? state.lengthContext.resolve(*length, LengthUnit::Px).value_or(fallback) : fallback;
    };
    tileX = resolve(tile.x, 0.0f);
    tileY = resolve(tile.y, 0.0f);
    tileWidth = resolve(tile.width, 0.0f);
    tileHeight = resolve(tile.height, 0.0f);
  }

  // A pattern with a zero or negative extent draws nothing. Refused rather than clamped to a pixel: a one-pixel tile
  // repeated across a shape is a picture, and not the one the document asked for.
  if (!(tileWidth > 0) || !(tileHeight > 0)) return nullptr;

  // One bitmap pixel per drawing unit of the tile, rounded up so a fractional tile is not clipped. A ceiling rather than
  // a round, because a tile of 2.4 units needs three pixels to hold all of it.
  const int pixelWidth = static_cast<int>(std::ceil(tileWidth));
  const int pixelHeight = static_cast<int>(std::ceil(tileHeight));
  if (pixelWidth <= 0 || pixelHeight <= 0) return nullptr;

  // A tile larger than this is a document mistake -- an `objectBoundingBox` tile whose numbers were written in user
  // units, which is exactly the inconsistency the corpus has. Allocating it would be gigabytes, so it is refused and the
  // fill falls back to its flat colour.
  constexpr int kMaxTilePixels = 4096;
  if (pixelWidth > kMaxTilePixels || pixelHeight > kMaxTilePixels) return nullptr;

  auto surface = Surface::makeRaster(pixelWidth, pixelHeight);
  if (surface == nullptr) return nullptr;
  const auto tileCanvas = surface->getCanvas();
  if (tileCanvas == nullptr) return nullptr;

  // ## The content transform
  //
  // From the content's coordinates into the tile's pixels: the content is expressed in the drawing's coordinates (or in
  // the element's box, which maps to them), and the tile's pixel (0,0) is the tile rectangle's top-left.
  // ## The content's units, taken at their word
  //
  // An earlier version second-guessed this: it measured the extent the content declared and, when that was far larger
  // than the tile, treated the declaration as contradicted and used the format's default instead. The measurement behind
  // it was wrong -- it read `jpg.svg`'s `<image width="200">` without the `transform="scale(0.005)"` on the `<use>` that
  // carries it, so it concluded the document was inconsistent when 200 * 0.005 is exactly the tile. The rule fired on a
  // document that was right, which is the one failure mode a compatibility heuristic must not have, so it is gone.
  //
  // The declaration is honoured. A document that writes `objectBoundingBox` content means it, and scaling content into
  // the element's box is what makes a pattern's contents grow with the shape it fills.
  Matrix contentToDrawing;
  if (tile.contentObjectBoundingBox) {
    contentToDrawing = Matrix::MakeTranslation(Vector3 {objectBounds.getLeft(), objectBounds.getTop(), 0}) *
                       Matrix::MakeScale(Vector3 {objectBounds.getWidth(), objectBounds.getHeight(), 1});
  }
  const Matrix contentToTile = Matrix::MakeTranslation(Vector3 {-tileX, -tileY, 0}) * contentToDrawing;
  tileCanvas->setTransform(contentToTile);
  tileCanvas->clipRect(Rect::MakeLTRB(0, 0, tileWidth, tileHeight));

  RenderState contentState = state;
  contentState.transform = contentToTile;
  // Cleared inside, so a pattern whose content refers to a pattern terminates rather than recursing.
  // The paint and clip references are cleared -- a pattern's content inherits no fill of its own -- but `activePatterns`
  // is deliberately *kept*, because that is what catches a cycle through an outer pattern rather than this one.
  contentState.fillIri.clear();
  contentState.strokeIri.clear();
  contentState.maskIri.clear();
  contentState.clipPathIri.clear();

  for (const NodePtr& child : pattern.children()) {
    child->render(*tileCanvas, contentState);
  }

  // ## Where the tile sits
  //
  // The shader samples in the tile bitmap's own pixels, so it needs the matrix that carries those pixels into the
  // drawing: the tile's origin, and then whatever `patternTransform` says about the finished tile.
  const Matrix localMatrix = tile.transform * Matrix::MakeTranslation(Vector3 {tileX, tileY, 0});
  return Shader::ImageShader(surface->detachBitmap(), TileMode::kRepeat, localMatrix);
}

/**
 * @return the shader an id refers to, sized for @p objectBounds, or null when there is none to build.
 *
 * The lookup goes through the document's id index and asks the node what it is, rather than keeping a separate table of
 * gradients: a reference may name anything, and a `url(#id)` that points at a rectangle is a document error that should
 * leave the shape with the fallback colour rather than crash looking for a gradient that was never there.
 */
std::shared_ptr<Shader> resolveShader(const std::string& iri, RenderState& state, const Rect& objectBounds) {
  if (iri.empty() || state.ids == nullptr) return nullptr;

  const Node* const node = state.ids->find(iri);
  if (node == nullptr) return nullptr;

  // A `url(#id)` may name either kind of paint definition, so both are asked for by type rather than a table being kept
  // alongside the index. A reference to anything else -- a clip path, a mask, an element that is not a definition at all
  // -- is a document error that leaves the shape in its fallback colour, which is what the format asks for.
  if (const auto* const gradient = dynamic_cast<const GradientNode*>(node)) {
    return buildShader(gradient->gradient(), objectBounds, state.lengthContext);
  }
  if (const auto* const pattern = dynamic_cast<const PatternNode*>(node)) {
    // A pattern whose tile is already being built is a cycle in the document. Refusing it leaves *that* shape in its
    // fallback colour rather than recursing until the stack runs out, and the outer pattern still finishes.
    const auto already = std::find(state.activePatterns.begin(), state.activePatterns.end(), pattern);
    if (already != state.activePatterns.end()) return nullptr;

    state.activePatterns.push_back(pattern);
    std::shared_ptr<Shader> shader = buildPatternShader(*pattern, objectBounds, state);
    state.activePatterns.pop_back();
    return shader;
  }
  return nullptr;
}

/** The colour a shape is painted with, with the opacity folded in. */
Color withOpacity(const Color color, const Scalar opacity) {
  Color out = color;
  out.alpha = color.alpha * std::clamp(opacity, 0.0f, 1.0f);
  return out;
}

/**
 * The stroke cap and join the canvas knows, from the SVG keywords.
 *
 * The two enumerations exist separately because they mean slightly different things: SVG's `butt`, `round` and `square`
 * are a closed set fixed by the format, while the canvas's are its own and could gain a value the format has no spelling
 * for. The mapping is spelled out rather than cast, so adding a value to either is a compile error here instead of a
 * silent reinterpretation.
 */
graphics::Paint::Cap toPaintCap(const LineCap cap) {
  switch (cap) {
    case LineCap::Round: return graphics::Paint::Cap::kRound;
    case LineCap::Square: return graphics::Paint::Cap::kSquare;
    case LineCap::Butt: break;
  }
  return graphics::Paint::Cap::kButt;
}

graphics::Paint::Join toPaintJoin(const LineJoin join) {
  switch (join) {
    case LineJoin::Round: return graphics::Paint::Join::kRound;
    case LineJoin::Bevel: return graphics::Paint::Join::kBevel;
    case LineJoin::Miter: break;
  }
  return graphics::Paint::Join::kMiter;
}

/** A `Paint` configured for filling or stroking with @p color. */
graphics::Paint makePaint(const Color color, const bool stroking, const RenderState& state) {
  graphics::Paint paint;
  paint.color = color;
  paint.style = stroking ? graphics::Paint::Style::kStroke : graphics::Paint::Style::kFill;
  if (stroking) {
    paint.strokeWidth = state.strokeWidth;
    paint.strokeCap = toPaintCap(state.strokeCap);
    paint.strokeJoin = toPaintJoin(state.strokeJoin);
    paint.strokeMiter = state.strokeMiterLimit;
  }
  return paint;
}

} // namespace anonymous

// ==================================================================================================
// The id index
// ==================================================================================================

void IdIndex::add(const std::string_view id, const Node* node) {
  if (id.empty()) return;
  // First one wins, which is what a document with duplicate ids gets from a browser: the first element with that id.
  mNodes.try_emplace(std::string {id}, node);
}

const Node* IdIndex::find(const std::string_view id) const {
  const auto it = mNodes.find(std::string {id});
  return it == mNodes.end() ? nullptr : it->second;
}

/**
 * @return the mask an id refers to, or null when there is none.
 *
 * Asks the node what it is rather than consulting a table, so a `mask` naming the wrong kind of element is a mask that
 * does not happen rather than a crash -- the same arrangement clips and gradients use.
 */
const MaskNode* resolveMask(const std::string& iri, const RenderState& state) {
  if (iri.empty() || state.ids == nullptr) return nullptr;
  return dynamic_cast<const MaskNode*>(state.ids->find(iri));
}

/**
 * The region a mask applies over, in the element's own coordinates.
 *
 * The format's defaults are -10%, -10%, 120%, 120%: a rectangle a tenth larger than the element on every side, which is
 * the format's way of saying "the whole thing and a margin" without having to know how big it is.
 */
Rect maskRegion(const MaskNode::Region& region, const Rect& objectBounds, const LengthContext& viewport) {
  // ## The two unit systems differ in what a *number* means
  //
  // `LengthContext::resolve` returns a `Number` unchanged and scales a `Percent` by the context's viewport. That is
  // exactly right for `userSpaceOnUse` -- `x="8"` there is eight user units and `x="50%"` is half the viewport.
  //
  // It is wrong for `objectBoundingBox`, where *both* spellings are fractions of the element's box: `x="0.5"` and
  // `x="50%"` name the same place, and a region written `width="0.5"` has to be half the element rather than half a unit.
  // So that case is resolved against a unit context, which turns both spellings into fractions, and the box then scales
  // them. Sharing one resolution between the two is precisely the mistake, because this is the difference between them.
  const auto resolve = [](const LengthContext& context, const std::optional<Length>& length, const Scalar fallback) {
    if (!length) return fallback;
    return context.resolve(*length, LengthUnit::Px).value_or(fallback);
  };

  if (region.objectBoundingBox) {
    LengthContext unit;
    unit.viewportWidth = 1;
    unit.viewportHeight = 1;

    // The defaults are -10%, -10%, 120%, 120% of the box, which in a unit context is -0.1 and 1.2.
    const Scalar fx = resolve(unit, region.x, -0.1f);
    const Scalar fy = resolve(unit, region.y, -0.1f);
    const Scalar fw = resolve(unit, region.width, 1.2f);
    const Scalar fh = resolve(unit, region.height, 1.2f);

    const Scalar left = objectBounds.getLeft();
    const Scalar top = objectBounds.getTop();
    const Scalar width = objectBounds.getWidth();
    const Scalar height = objectBounds.getHeight();
    return Rect::MakeLTRB(left + fx * width, top + fy * height, left + (fx + fw) * width, top + (fy + fh) * height);
  }

  // `userSpaceOnUse`: numbers are user units and percentages are fractions of the viewport, which is what the resolver
  // already distinguishes -- so the defaults have to be written out as the viewport fractions they are.
  const Scalar x = resolve(viewport, region.x, -0.1f * viewport.viewportWidth);
  const Scalar y = resolve(viewport, region.y, -0.1f * viewport.viewportHeight);
  const Scalar w = resolve(viewport, region.width, 1.2f * viewport.viewportWidth);
  const Scalar h = resolve(viewport, region.height, 1.2f * viewport.viewportHeight);
  return Rect::MakeLTRB(x, y, x + w, y + h);
}

/**
 * Renders @p mask's children into a coverage bitmap of the canvas's own size.
 *
 * @return the coverage, or nothing when it could not be allocated. The caller treats a missing coverage as "no mask"
 *         rather than as "mask everything away", because a mask that cannot be built is a failure to apply an effect and
 *         not an instruction to hide the element.
 */
std::optional<Bitmap> renderMaskCoverage(const Canvas& canvas, const MaskNode& mask, const RenderState& state,
                                         const Rect& objectBounds) {
  // The mask is rendered by the *CPU* rasterizer whatever backend the target is, so it needs a size and a pixel layout
  // and not the target's pixels -- which the GPU backend does not have. Reading them from `canvas.target()` made this
  // return `nullopt` on every GPU frame, so `maskLayer` was never called and the layer was composited unmasked: the same
  // defect as `drawFiltered`'s, one function over. See that function for the longer note.
  const Bitmap& target = canvas.target();
  const Rect bounds = canvas.deviceBounds();
  const ImageInfo info = target.isEmpty()
                           ? ImageInfo { static_cast<int>(bounds.getWidth()), static_cast<int>(bounds.getHeight()),
                                 ColorType::kRGBA_8888, AlphaType::kPremultiplied }
                           : target.info();
  if (!info.isValid()) return std::nullopt;

  auto surface = Surface::makeRaster(info);
  if (surface == nullptr) return std::nullopt;

  const auto maskCanvas = surface->getCanvas();
  if (maskCanvas == nullptr) return std::nullopt;

  // The element's own transform, which carries caller space to device space. Everything the mask does is relative to it.
  const Matrix toDevice = canvas.transform();

  // The region is in the element's coordinates, so it is clipped with the element's transform in force. Outside it a mask
  // contributes nothing -- which is what makes the default margin a margin rather than a no-op.
  maskCanvas->setTransform(toDevice);
  maskCanvas->clipRect(maskRegion(mask.region(), objectBounds, state.lengthContext));

  // The content's own transform, which may be a different space from the region's. This is the part the format makes
  // awkward and the corpus exercises: the region is usually in the element's box and the content usually is not.
  Matrix content;
  if (mask.region().contentObjectBoundingBox) {
    content = Matrix::MakeTranslation(Vector3 {objectBounds.getLeft(), objectBounds.getTop(), 0}) *
              Matrix::MakeScale(Vector3 {objectBounds.getWidth(), objectBounds.getHeight(), 1});
  }
  const Matrix contentToDevice = toDevice * content;
  maskCanvas->setTransform(contentToDevice);

  RenderState contentState = state;
  contentState.transform = contentToDevice;
  // The reference is cleared inside the mask: a mask whose content refers to the same mask is a cycle, and the bounds
  // would be the mask's own rather than the element's.
  contentState.maskIri.clear();
  contentState.clipPathIri.clear();

  for (const NodePtr& child : mask.children()) {
    child->render(*maskCanvas, contentState);
  }

  return surface->detachBitmap();
}

/**
 * Applies the mask @p state refers to, if any, to the layer @p canvas currently has open.
 *
 * The reference is cleared once applied, for the same reason a clip's is: the effect belongs to the element that named
 * it, and a descendant inheriting the id would apply it a second time.
 */
/**
 * @note An unsupported element inside a mask does not weaken the mask, it **empties** it -- and an empty mask hides the
 *       element rather than leaving it alone. That is the correct reading of a mask whose content drew nothing, and the
 *       wrong outcome when the reason is a feature this renderer does not have: mask_image.svg in the corpus draws
 *       nothing at all for exactly that reason, and its test asserts the fact so that it is noticed when <image> lands
 *       rather than rediscovered later.
 *
 *       It is also why the mask was worth doing before the features it composes with, rather than after.
 */
void applyMask(Canvas& canvas, RenderState& state, const Rect& objectBounds) {
  const MaskNode* const mask = resolveMask(state.maskIri, state);
  state.maskIri.clear();
  if (mask == nullptr || mask->children().empty()) return;

  if (auto coverage = renderMaskCoverage(canvas, *mask, state, objectBounds)) {
    // The mask's own mode, which is luminance unless the document said otherwise. The two disagree wherever the mask's
    // colour varies while its alpha does not, so honouring the declaration is not a detail.
    canvas.maskLayer(*coverage, mask->region().mode);
  }
}

// ==================================================================================================
// Nodes
// ==================================================================================================

void Node::indexIds(IdIndex& index) const { index.add(mId, this); }

void Node::resolveReferences(const IdIndex&) {
  // A leaf holds no references. The containers override this to walk their children.
}

void GroupNode::indexIds(IdIndex& index) const {
  Node::indexIds(index);
  for (const NodePtr& child : mChildren) {
    child->indexIds(index);
  }
}

void GroupNode::resolveReferences(const IdIndex& index) {
  Node::resolveReferences(index);
  for (const NodePtr& child : mChildren) {
    child->resolveReferences(index);
  }
}

void UseNode::resolveReferences(const IdIndex& index) {
  Node::resolveReferences(index);
  // Resolved now rather than at render time, because a `use` may precede what it names and the index is only complete
  // once the whole tree exists.
  mReferenced = index.find(mTarget);
}

void PatternNode::resolveReferences(const IdIndex& index) {
  Node::resolveReferences(index);
  for (const NodePtr& child : mChildren) {
    child->resolveReferences(index);
  }
}

void MaskNode::resolveReferences(const IdIndex& index) {
  Node::resolveReferences(index);
  for (const NodePtr& child : mChildren) {
    child->resolveReferences(index);
  }
}

Node& GroupNode::addChild(NodePtr child) {
  mChildren.push_back(std::move(child));
  return *mChildren.back();
}

void FilterNode::indexIds(IdIndex& index) const {
  // Only itself: a filter's primitives are data rather than nodes, so there is nothing below to index. The override
  // exists because the base class's walk cannot know that.
  Node::indexIds(index);
}

void FilterNode::resolveReferences(const IdIndex& index) {
  // A primitive names its inputs by `result` string, resolved inside the chain rather than through the id index -- so
  // there is nothing here either, and the two overrides are here to say so rather than to do something.
  Node::resolveReferences(index);
}

void PatternNode::indexIds(IdIndex& index) const {
  // Its children are drawable nodes -- a `use`, an image, a gradient fill -- and all of them resolve through this index.
  // Indexing only the pattern itself would leave everything inside it unreachable.
  Node::indexIds(index);
  for (const NodePtr& child : mChildren) {
    child->indexIds(index);
  }
}

void MaskNode::indexIds(IdIndex& index) const {
  // A mask is a definition, but its children are not: they may carry ids a `use` refers to, or fill themselves with a
  // gradient, and both of those resolve through this index. Indexing only the mask itself would leave everything inside
  // it unreachable.
  Node::indexIds(index);
  for (const NodePtr& child : mChildren) {
    child->indexIds(index);
  }
}

/** @return the filter an id refers to, or null when there is none to apply. */
const FilterNode* resolveFilter(const std::string& iri, const RenderState& state) {
  if (iri.empty() || state.ids == nullptr) return nullptr;

  const Node* const node = state.ids->find(iri);
  if (node == nullptr) return nullptr;

  // A `url(#id)` naming something that is not a filter is a document error. Reported as no filter rather than as a
  // failure, so the element draws as though the attribute were absent -- which is what SVG's error handling asks for.
  return dynamic_cast<const FilterNode*>(node);
}

/**
 * @return @p bounds mapped through @p matrix, as the box that contains the result.
 *
 * A rectangle is built and mapped rather than the two corners being transformed and re-paired, because a rotation maps
 * a box to a diamond whose containing box is not the box its corners span. `Matrix` offers no rectangle mapping, and
 * `Path::transformed` is the one that exists.
 */
Rect toDevice(const Rect& bounds, const Matrix& matrix) {
  if (!(bounds.getWidth() > 0) || !(bounds.getHeight() > 0)) return bounds;

  Path box;
  box.moveTo(Point::MakeXY(bounds.getLeft(), bounds.getTop()));
  box.lineTo(Point::MakeXY(bounds.getRight(), bounds.getTop()));
  box.lineTo(Point::MakeXY(bounds.getRight(), bounds.getBottom()));
  box.lineTo(Point::MakeXY(bounds.getLeft(), bounds.getBottom()));
  box.close();
  return box.transformed(matrix).bounds();
}

/**
 * Draws @p draw through @p filter.
 *
 * @param draw paints the element, in the canvas's current space.
 *
 * The element is painted into a device-sized surface with the canvas's transform in force, so the buffer holds exactly
 * what the canvas would have received. The filter then runs over that buffer, and the result is drawn back with the
 * transform reset -- because a filtered picture is in device space, and re-applying the element's transform to it would
 * move it a second time.
 *
 * A filter that cannot be built leaves the element as it was drawn, which is the format's own rule for an unusable
 * filter and the opposite of how a mask behaves: a missing mask hides an element, and a missing filter does not.
 */
void drawFiltered(Canvas& canvas, const Filter& filter, const Rect& objectBounds, const RenderState& state,
                  const std::function<void(Canvas&)>& draw) {
  // ## The buffer's size comes from the device bounds, not from the target's pixels
  //
  // This used to read `canvas.target()` for both, which is the caller's own bitmap on the rasterizer and *empty* on the
  // GPU backend -- that backend has no host pixels to hand back before submission, by design. So the whole filter path
  // bailed out at the check below and drew the element **unfiltered**: a `<use fill="#000" filter="url(#innerShadow)">`
  // painted its black fill raw instead of the faint shadow the filter asks for, which is what made the demo's
  // `complex2.svg` and `blur.svg` wrong on the GPU and right on the rasterizer.
  //
  // Only the *size* and the pixel layout are needed to allocate the buffer, and the device bounds give the size on both
  // backends. The pixels are still taken from the target when it has them, because its `ImageInfo` is the caller's own
  // format -- an unpremultiplied or narrower target would otherwise be filtered through a format it was not drawn in.
  const Bitmap& target = canvas.target();
  const Rect bounds = canvas.deviceBounds();
  const ImageInfo info = target.isEmpty()
                           ? ImageInfo { static_cast<int>(bounds.getWidth()), static_cast<int>(bounds.getHeight()),
                                 ColorType::kRGBA_8888, AlphaType::kPremultiplied }
                           : target.info();
  if (!info.isValid()) {
    draw(canvas);
    return;
  }

  auto surface = Surface::makeRaster(info);
  if (surface == nullptr) {
    draw(canvas);
    return;
  }

  const auto bufferCanvas = surface->getCanvas();
  if (bufferCanvas == nullptr) {
    draw(canvas);
    return;
  }

  // The element's own transform, so the buffer holds what the canvas would have received.
  bufferCanvas->setTransform(canvas.transform());
  draw(*bufferCanvas);

  const Bitmap source = surface->detachBitmap();
  // The object bounds arrive in the coordinates the element's own geometry was written in, and the region derived from
  // them is compared against device pixels -- so they are mapped here, through the same transform the buffer was drawn
  // with. At a transform of one this is the identity, which is why the bug it fixes was invisible to every earlier test.
  const auto filtered = applyFilter(filter, source, toDevice(objectBounds, canvas.transform()), state.lengthContext,
                                    canvas.transformScale(), canvas.deviceBounds());


  canvas.save();
  canvas.setTransform(Matrix {});
  graphics::Paint paint;
  if (filtered) {
    canvas.drawBitmap(*filtered, 0, 0, paint);
  } else {
    // The element as it was drawn, unfiltered. Losing a filter changes the picture; losing the element loses it.
    canvas.drawBitmap(source, 0, 0, paint);
  }
  canvas.restore();
}

void GroupNode::render(Canvas& canvas, const RenderState& state) const {
  if (mHidden) return;
  RenderState merged = mergeState(state, mPresentation);
  // `visibility="hidden"` hides the element and its descendants; unlike `display="none"` it does not remove them from
  // layout, but nothing here lays out, so the effect on the picture is the same.
  if (merged.visibility == Visibility::Hidden || merged.visibility == Visibility::Collapse) return;

  canvas.save();
  applyClip(canvas, merged, std::nullopt);

  // ## One layer, for two reasons
  //
  // A group with opacity is one object, not a set of children each drawn faintly: inside a layer the children composite
  // against each other at full strength and the finished group is faded once, so an overlap between two children is the
  // same shade as the rest of the group rather than twice as dark.
  //
  // A mask needs a layer for a different reason -- it is applied to a finished picture, not to geometry. But it is the
  // *same* layer: opening a second would composite the group twice and square its alpha, turning a half into a quarter.
  // So one `saveLayer` covers both, the opacity rides on its paint, and the mask is applied to what the group drew.
  const bool faded = merged.opacity < 1.0f;
  const bool masked = resolveMask(merged.maskIri, merged) != nullptr;
  // A filter occupies the same layer as the opacity and the mask, for the same reason: opening a second would composite
  // the group twice and square its alpha.
  const FilterNode* const filterNode = resolveFilter(merged.filterIri, merged);

  // ## A filter belongs to the element, not to its descendants
  //
  // This one line is worth more than the two rounds of optimisation before it. A filter is applied to the element's
  // finished rendering -- one picture -- but the reference travelled down the state, so every descendant re-evaluated
  // the whole filter for itself. `complex7.svg` nests its filtered groups, and one render called `applyFilter` **664
  // times**, each allocating a device-sized buffer per primitive: about 1400 seconds at 1090x2026, which is exactly the
  // cost that per-pixel work could not touch.
  //
  // Cleared here, once, for the state the children see. The filter is applied to what they draw, by the caller below,
  // which is what the format asks for.
  merged.filterIri.clear();

  if (faded || masked || filterNode != nullptr) {
    canvas.saveLayer(kUnboundedLayer, layerPaint(faded ? merged.opacity : 1.0f));
    // The opacity is reset inside, because the layer carries it now. Without that it would be applied twice, once by
    // the layer and once by each descendant folding it into its own alpha.
    merged.opacity = 1;
  }

  const auto drawChildren = [this, &merged](Canvas& target) {
    for (const NodePtr& child : mChildren) {
      child->render(target, merged);
    }
  };

  if (filterNode != nullptr) {
    // A group has no geometry of its own, so an `objectBoundingBox` filter region has no box to measure against and
    // falls back to the viewport -- the same limitation a mask has here, and for the same reason.
    drawFiltered(canvas, filterNode->filter(), Rect {}, merged, drawChildren);
  } else {
    drawChildren(canvas);
  }

  // A group has no geometry of its own, so an `objectBoundingBox` mask on one has no box to measure against and is
  // skipped -- the same limitation clip paths have here, and for the same reason. The default for `maskUnits` is
  // `objectBoundingBox`, so this is the case a corpus that omits the attribute would hit.
  if (masked) applyMask(canvas, merged, Rect {});

  if (faded || masked || filterNode != nullptr) canvas.restore();
  canvas.restore();
}

void ShapeNode::render(Canvas& canvas, const RenderState& state) const {
  if (mHidden) return;
  RenderState merged = mergeState(state, mPresentation);
  if (merged.visibility == Visibility::Hidden || merged.visibility == Visibility::Collapse) return;
  if (mPath.isEmpty()) return;

  // The stroke uses the path's fill type as written; the fill uses the `fill-rule` property. They are separate
  // properties in SVG for exactly this reason -- a self-crossing outline can be stroked as drawn and filled by the
  // nonzero rule.
  canvas.save();
  canvas.setTransform(merged.transform);

  const Rect objectBounds = mPath.bounds();
  applyClip(canvas, merged, objectBounds);

  // A shape's own `opacity` gets a layer too, and for the same reason a group's does: a shape with both a fill and a
  // stroke draws its outline twice where they overlap, so folding the alpha into each paint would darken the join. A
  // shape with one paint would be exact either way, and using a layer uniformly is one rule instead of two.
  //
  // And as in the group, a mask shares that layer rather than nesting inside it. A shape has real geometry, so an
  // `objectBoundingBox` mask on one *can* be measured -- `objectBounds` is the path's own box.
  const bool faded = merged.opacity < 1.0f;
  const bool masked = resolveMask(merged.maskIri, merged) != nullptr;
  const FilterNode* const filterNode = resolveFilter(merged.filterIri, merged);
  if (faded || masked || filterNode != nullptr) {
    canvas.saveLayer(kUnboundedLayer, layerPaint(faded ? merged.opacity : 1.0f));
    merged.opacity = 1;
  }

  // ## A reference is not a fallback
  //
  // `hasFill` answers "is there a colour to paint with", and for `fill="url(#g)"` written without a fallback colour the
  // answer is no -- which is correct, because a reference that cannot be resolved must leave the shape unpainted.
  //
  // But it is not the same question as "is there anything to draw", and reading it as though it were means a shape
  // filled with a perfectly good gradient is never drawn at all: the reference resolves, and the check that gates the
  // draw is looking at the fallback that was never supplied. So the shader is resolved first, and either one is enough.
  // Mutable because building a pattern shader records the pattern as in progress, which is how a cycle is caught.
  const std::shared_ptr<Shader> fillShader = resolveShader(merged.fillIri, merged, objectBounds);
  const std::shared_ptr<Shader> strokeShader = resolveShader(merged.strokeIri, merged, objectBounds);

  const auto paintShape = [&](Canvas& target) {
    if (merged.hasFill || fillShader != nullptr) {
      Path filled = mPath;
      filled.setFillType(merged.fillRule == FillRule::EvenOdd ? FillType::EvenOdd : FillType::Winding);
      graphics::Paint paint =
              makePaint(withOpacity(merged.fillColor, merged.opacity * merged.fillOpacity), false, merged);
      paint.shader = fillShader;
      target.drawPath(filled, paint);
    }

    if ((merged.hasStroke || strokeShader != nullptr) && merged.strokeWidth > 0) {
      Path stroked = mPath;
      stroked.setFillType(FillType::Winding);
      graphics::Paint paint =
              makePaint(withOpacity(merged.strokeColor, merged.opacity * merged.strokeOpacity), true, merged);
      paint.shader = strokeShader;
      target.drawPath(stroked, paint);
    }
  };

  // A shape has real geometry, so an `objectBoundingBox` filter region *can* be measured -- `objectBounds` is the
  // path's own box, as it is for a mask.
  if (filterNode != nullptr) {
    drawFiltered(canvas, filterNode->filter(), objectBounds, merged, paintShape);
  } else {
    paintShape(canvas);
  }

  if (masked) applyMask(canvas, merged, objectBounds);

  if (faded || masked || filterNode != nullptr) canvas.restore();
  canvas.restore();
}

void UseNode::render(Canvas& canvas, const RenderState& state) const {
  if (mHidden || mReferenced == nullptr) return;
  const RenderState merged = mergeState(state, mPresentation);
  if (merged.visibility == Visibility::Hidden || merged.visibility == Visibility::Collapse) return;

  // The reference is drawn at this element's position, and under this element's inherited state rather than the state
  // where the target happens to live: a `use` is a copy, not a move.
  RenderState translated = merged;
  translated.transform = merged.transform * Matrix::MakeTranslation(Vector3 {mPosition.x, mPosition.y, 0});

  canvas.save();
  mReferenced->render(canvas, translated);
  canvas.restore();
}

void ImageNode::render(Canvas& canvas, const RenderState& state) const {
  if (mHidden) return;
  const RenderState merged = mergeState(state, mPresentation);
  if (merged.visibility == Visibility::Hidden || merged.visibility == Visibility::Collapse) return;
  if (mBitmap.isEmpty()) return;

  // A zero or negative width or height disables the element, as it does for a rectangle. Drawing it anyway would put the
  // picture at its own size, which is a different picture from the one the document asked for.
  if (!(mFrame.getWidth() > 0) || !(mFrame.getHeight() > 0)) return;

  const auto imageWidth = static_cast<Scalar>(mBitmap.width());
  const auto imageHeight = static_cast<Scalar>(mBitmap.height());
  if (!(imageWidth > 0) || !(imageHeight > 0)) return;

  // ## The aspect ratio
  //
  // The format's default is `xMidYMid meet`: the picture is scaled uniformly to *fit* inside the rectangle and centred,
  // so a square image in a wide box is letterboxed rather than stretched. `slice` is the cover, and is the only other
  // value that changes what is drawn.
  //
  // None of the corpus's nine images states a `preserveAspectRatio`, so `meet` is the branch that runs -- and the one
  // that has to be right, because stretching produces a picture that is obviously wrong only to somebody who knows what
  // it should look like.
  const Scalar scaleX = mFrame.getWidth() / imageWidth;
  const Scalar scaleY = mFrame.getHeight() / imageHeight;
  const Scalar scale = mAspect.slice ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);

  const Scalar drawnWidth = imageWidth * scale;
  const Scalar drawnHeight = imageHeight * scale;
  const Scalar offsetX = mFrame.getLeft() + (mFrame.getWidth() - drawnWidth) / 2;
  const Scalar offsetY = mFrame.getTop() + (mFrame.getHeight() - drawnHeight) / 2;

  canvas.save();
  canvas.setTransform(merged.transform);
  canvas.concat(Matrix::MakeTranslation(Vector3 {offsetX, offsetY, 0}) *
                Matrix::MakeScale(Vector3 {scale, scale, 1}));

  // The clip is what makes `slice` mean cover rather than overflow: the parts of the picture outside the rectangle are
  // cut off. For `meet` the scale already fits, so this cuts nothing. Applied after the transform, because `clipRect`
  // takes its rectangle in the canvas's current space.
  canvas.clipRect(Rect::MakeLTRB(0, 0, imageWidth, imageHeight));

  // The paint's colour is an *alpha* and nothing else. A picture carries its own colours, and the format's initial `fill`
  // is black -- so treating it as a colour would multiply every image to nothing, which is a blank panel that looks like
  // a decode failure.
  const auto alpha = static_cast<uint8_t>(std::lround(std::clamp(merged.opacity, 0.0f, 1.0f) * 255.0f));
  graphics::Paint paint;
  paint.color = Color::MakeRGBA8(255, 255, 255, alpha);
  canvas.drawBitmap(mBitmap, 0, 0, paint);

  canvas.restore();
}

void TextNode::render(Canvas& canvas, const RenderState& state) const {
  if (mHidden) return;
  const RenderState merged = mergeState(state, mPresentation);
  if (merged.visibility == Visibility::Hidden || merged.visibility == Visibility::Collapse) return;
  if (mContent.empty()) return;

  const std::shared_ptr<Typeface> typeface = resolveTypeface(merged.fontFamily, merged.fontStyle, merged.fontWeight);
  if (typeface == nullptr) return;
  const std::shared_ptr<Font> font = Font::create(typeface, merged.fontSize);
  if (font == nullptr) return;

  const TextBlob blob = font->shape(mContent);
  if (blob.isEmpty()) return;

  // `text-anchor` moves the run relative to the point it is drawn at: the start is the default, the middle centres it,
  // and the end finishes there. The width has to come from the same shaper that produced the glyphs.
  Scalar offset = 0;
  switch (merged.textAnchor) {
    case TextAnchor::Start: break;
    case TextAnchor::Middle: offset = -font->measureWidth(mContent) / 2; break;
    case TextAnchor::End: offset = -font->measureWidth(mContent); break;
  }

  canvas.save();
  canvas.setTransform(merged.transform);
  if (merged.hasFill) {
    canvas.drawTextBlob(blob, mPosition.x + offset, mPosition.y,
                        *font, makePaint(withOpacity(merged.fillColor, merged.opacity * merged.fillOpacity), false, merged));
  }
  canvas.restore();
}

// ==================================================================================================
// The document
// ==================================================================================================

Document::~Document() = default;

void Document::render(Canvas& canvas) const { render(canvas, Rect::MakeWH(mWidth, mHeight)); }

void Document::render(Canvas& canvas, const Rect& bounds) const {
  if (mRoot == nullptr) return;

  RenderState state;
  // The index is what a shape resolves `url(#id)` through, so it is threaded into the pass rather than consulted by the
  // shape directly -- the nodes do not know the document they belong to.
  state.ids = &mIndex;
  // The initial `color` is black, which is what `currentColor` means when nothing set it.
  state.currentColor = Color::Black();
  state.lengthContext.viewportWidth = mWidth;
  state.lengthContext.viewportHeight = mHeight;

  canvas.save();

  // ## The fit, which is not optional when there is no viewBox
  //
  // With a viewBox, the box is what is fitted into the destination. Without one, the document's own `width` and `height`
  // are -- which is the same operation against a different source of extent. Leaving the transform at the identity for
  // that case draws a 300x200 document at 300x200 whatever rectangle it was given, which is correct only when the two
  // happen to agree; the montage in `examples/graphics/win` drew at five times the size of its cell before this was fixed.
  const Matrix fit = mViewBox ? fitViewBox(*mViewBox, bounds.getWidth(), bounds.getHeight(), mAspect).matrix
                              : Matrix::MakeScale(Vector3 {
                                        mWidth > 0 ? bounds.getWidth() / mWidth : 1.0f,
                                        mHeight > 0 ? bounds.getHeight() / mHeight : 1.0f, 1.0f});
  const Matrix placement = Matrix::MakeTranslation(Vector3 {bounds.getLeft(), bounds.getTop(), 0});
  state.transform = placement * fit;

  // The percentages inside the document resolve against the *destination*, because that is the viewport the picture ends
  // up drawn in. A document that fills 100% of a cell fills the cell, not its own nominal size.
  state.lengthContext.viewportWidth = bounds.getWidth();
  state.lengthContext.viewportHeight = bounds.getHeight();

  // And the drawing is bounded by the rectangle it was asked to fill. SVG itself permits content to overflow a viewport
  // -- `overflow: visible` is a real feature and several of the corpus documents use it -- but a caller that hands over
  // a rectangle is drawing *into* that rectangle, and a montage cell that spills across its neighbours is not a montage.
  //
  // The clip goes on *after* the transform, because `clipRect` takes its rectangle in the canvas's current space.
  // Applying it first clips against (0,0)-(w,h) in *device* space, which is the destination only when the destination
  // happens to be at the origin -- so a cell at (20,20) would be clipped away entirely, and the mistake is invisible in
  // every test whose rectangle starts at zero.
  canvas.setTransform(state.transform);
  canvas.clipRect(Rect::MakeLTRB(0, 0, bounds.getWidth(), bounds.getHeight()));

  for (const NodePtr& child : mRoot->children()) {
    child->render(canvas, state);
  }

  canvas.restore();
}

} // namespace graphics::svg
