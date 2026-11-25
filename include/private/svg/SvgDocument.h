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

#ifndef CORAL_GRAPHICS_SVG_SVG_DOCUMENT_H
#define CORAL_GRAPHICS_SVG_SVG_DOCUMENT_H

#include "svg/SvgGradient.h"
#include "svg/SvgFilter.h"
#include "svg/SvgPresentation.h"

#include <graphics/Bitmap.h>
#include <graphics/Canvas.h>
#include <graphics/Font.h>
#include <graphics/Paint.h>
#include <graphics/Path.h>
#include <graphics/Typeface.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/**
 * @file SvgDocument.h
 *
 * The SVG document tree and the pass that draws it.
 *
 * ## Nodes carry what they say; the state carries what was resolved
 *
 * A node holds a `Presentation` -- only what the element itself specifies -- and rendering threads a `RenderState` down
 * the tree, merging each node's presentation over it. That split is what makes inheritance work without copying a
 * resolved property onto every descendant, and it is why a node can be rendered under any inherited state rather than
 * only under the one it was built with.
 *
 * ## What is drawn, and what the canvas is asked to do
 *
 * Geometry stays in user space throughout: the accumulated transform goes to the canvas and the path is handed over
 * untransformed. Transforming the geometry here instead would bake the transform in, and a stroke width would then need
 * scaling by hand -- which is exactly the bug the canvas already documents avoiding by transforming before flattening.
 *
 * ## Two things this does not do yet, and says so
 *
 * **Group opacity is folded into the descendants' alpha.** That is exact for a single shape and an approximation for a
 * group, because children that overlap composite twice at partial alpha instead of once. The correct answer needs an
 * offscreen layer, and until the canvas has one this is the honest approximation rather than a silent one.
 *
 * **Gradients, clips and masks are not implemented.** A `url(#id)` fill currently falls back to its fallback colour, or
 * to nothing -- the `Paint` types already carry the reference, so the day those land they need no reparsing.
 */

// Declared rather than included: the XML tree is a means to this document, and a header that pulled in the whole reader
// would put it on every includer. Only a reference to it crosses this boundary.
namespace graphics::xml {
class Node;
} // namespace graphics::xml

namespace graphics::svg {

class Node;

/**
 * The id index, declared before the render state because the state carries a pointer to it: a shape resolves its own
 * `url(#id)` references at draw time, and the alternative -- threading the index through every render signature -- would
 * put an argument on every node that most of them ignore.
 */
class IdIndex;

/** Declared before the render state, which carries the patterns currently being built as a cycle guard. */
class PatternNode;

/**
 * The rendering state at a point in the tree, with every inherited property resolved.
 *
 * The initial values here are SVG's own, applied once at the root rather than written into every node.
 */
struct RenderState {
  Color fillColor = Color::Black();
  bool hasFill = true;
  Scalar fillOpacity = 1;
  FillRule fillRule = FillRule::NonZero;
  /** The id a `url(#id)` fill refers to, or empty. Resolved at draw time against the shape's own box. */
  std::string fillIri;

  Color strokeColor = Color::Black();
  bool hasStroke = false;
  Scalar strokeOpacity = 1;
  Scalar strokeWidth = 1;
  LineCap strokeCap = LineCap::Butt;
  LineJoin strokeJoin = LineJoin::Miter;
  Scalar strokeMiterLimit = 4;
  DashArray strokeDashArray;
  /** The id a `url(#id)` stroke refers to, or empty. */
  std::string strokeIri;

  /** Group opacity, multiplied down the tree, and applied through a layer at the node that sets it. */
  Scalar opacity = 1;

  /**
   * The id a `clip-path="url(#id)"` refers to, or empty.
   *
   * Resolved at draw time for the same reason a gradient is: the clip may be written in the shape's bounding box, which
   * is not known until the shape is drawn.
   */
  std::string clipPathIri;

  /**
   * The id a `mask="url(#id)"` refers to, or empty.
   *
   * Carried rather than resolved for the same reason the clip is, and for one more: a mask may be written in the
   * element's bounding box, which the element does not know until it is drawn. See @c MaskNode for what is done with it.
   */
  std::string maskIri;

  /** The id a `filter="url(#id)"` refers to, or empty. Carried for the same reason the mask is. */
  std::string filterIri;

  /**
   * The patterns whose tiles are being built, outermost first.
   *
   * A cycle guard, and a necessary one: a pattern's content may reference a pattern, and a child that writes
   * `fill="url(#p)"` overrides the inherited fill -- so clearing the reference in the content state does not stop it.
   * Without this the tile build re-enters itself until the stack runs out, which is a crash rather than a wrong picture.
   */
  std::vector<const PatternNode*> activePatterns;

  Color currentColor = Color::Black();
  Matrix transform;
  LengthContext lengthContext;

  /**
   * The document's id index, so a shape can resolve its own `url(#id)` references.
   *
   * A pointer rather than a reference so the state stays copy-assignable, and raw because the index outlives every
   * render: it belongs to the document, which the whole pass is reading from.
   */
  const IdIndex* ids = nullptr;

  std::string fontFamily;
  Scalar fontSize = 16;
  FontStyle fontStyle = FontStyle::Normal;
  FontWeight fontWeight = FontWeight::Normal;
  TextAnchor textAnchor = TextAnchor::Start;

  Visibility visibility = Visibility::Visible;
};

/** An element's name-to-node index, so `use` and `url(#id)` can find what they refer to. */
class IdIndex {
  public:
  void add(std::string_view id, const Node* node);
  [[nodiscard]] const Node* find(std::string_view id) const;

  private:
  std::unordered_map<std::string, const Node*> mNodes;
};

/** One element of the document. */
class Node {
  public:
  Node() = default;
  virtual ~Node() = default;
  Node(Node&&) = delete;
  Node& operator=(Node&&) = delete;
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  /** Draws this node and its descendants under @p state. */
  virtual void render(Canvas& canvas, const RenderState& state) const = 0;

  /** Adds this node and its descendants to @p index. Called once, after the tree is built. */
  virtual void indexIds(IdIndex& index) const;

  /**
   * Resolves the references this node and its descendants hold. Called once, after the index is built.
   *
   * Virtual for the same reason @c indexIds is: a walk that knew the container types by name knew only some of them, and
   * a `<use>` inside a `<pattern>` was never resolved because of it. A container added later overrides this, and cannot
   * be forgotten by a walk that does not enumerate containers at all.
   */
  virtual void resolveReferences(const IdIndex& index);

  [[nodiscard]] const Presentation& presentation() const noexcept { return mPresentation; }
  void setPresentation(Presentation presentation) { mPresentation = std::move(presentation); }

  /**
   * The element's presentation, mutable.
   *
   * The builder uses this for the one case where a document's meaning cannot be read off the element alone: a `<line>`
   * takes no fill, so the builder turns the fill off after the presentation has been parsed -- and only when the element
   * did not set one itself.
   */
  [[nodiscard]] Presentation& presentation() noexcept { return mPresentation; }

  [[nodiscard]] const std::string& id() const noexcept { return mId; }
  void setId(std::string id) { mId = std::move(id); }

  /** True when the element is not rendered: `display="none"` or a definition. */
  [[nodiscard]] bool isHidden() const noexcept { return mHidden; }
  void setHidden(const bool hidden) noexcept { mHidden = hidden; }

  protected:
  Presentation mPresentation;
  std::string mId;
  bool mHidden = false;
};

using NodePtr = std::unique_ptr<Node>;

/** A `<g>`, `<defs>`, `<svg>` nested, or any element whose only job is to hold children. */
class GroupNode final : public Node {
  public:
  void render(Canvas& canvas, const RenderState& state) const override;
  void indexIds(IdIndex& index) const override;
  void resolveReferences(const IdIndex& index) override;

  Node& addChild(NodePtr child);
  [[nodiscard]] const std::vector<NodePtr>& children() const noexcept { return mChildren; }

  private:
  std::vector<NodePtr> mChildren;
};

/** A shape: `path`, `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`. */
class ShapeNode final : public Node {
  public:
  explicit ShapeNode(Path path) : mPath(std::move(path)) {}

  void render(Canvas& canvas, const RenderState& state) const override;

  [[nodiscard]] const Path& path() const noexcept { return mPath; }

  private:
  Path mPath;
};

/**
 * A `linearGradient` or `radialGradient`: a definition, drawn nowhere and referred to by id.
 *
 * It is a node rather than a side table because it has to be reachable through the document's id index, and the index
 * maps ids to nodes. Rendering one draws nothing, which is what makes it safe to leave in the tree where it was
 * written -- including outside `<defs>`, which SVG allows.
 */
class GradientNode final : public Node {
  public:
  explicit GradientNode(Gradient gradient) : mGradient(std::move(gradient)) {}

  /** Drawing a gradient element paints nothing; it is a definition. */
  void render(Canvas&, const RenderState&) const override {}

  [[nodiscard]] const Gradient& gradient() const noexcept { return mGradient; }

  private:
  Gradient mGradient;
};

/** A `<use>`: renders the element it refers to, translated to this element's position. */
class UseNode final : public Node {
  public:
  void render(Canvas& canvas, const RenderState& state) const override;

  [[nodiscard]] const std::string& target() const noexcept { return mTarget; }
  void setTarget(std::string target) { mTarget = std::move(target); }

  void setPosition(const Point position) noexcept { mPosition = position; }
  [[nodiscard]] Point position() const noexcept { return mPosition; }

  /** @return what this refers to, or null when it could not be resolved. */
  [[nodiscard]] const Node* referenced() const noexcept { return mReferenced; }

  void resolveReferences(const IdIndex& index) override;

  private:
  std::string mTarget;
  Point mPosition {0, 0};
  const Node* mReferenced = nullptr;
};

/** A `<text>` element, drawn as a single run at its `x`/`y`. */
class TextNode final : public Node {
  public:
  void render(Canvas& canvas, const RenderState& state) const override;

  void setPosition(const Point position) noexcept { mPosition = position; }
  void setContent(std::string content) { mContent = std::move(content); }

  [[nodiscard]] Point position() const noexcept { return mPosition; }
  [[nodiscard]] const std::string& content() const noexcept { return mContent; }

  private:
  Point mPosition {0, 0};
  std::string mContent;
};

/**
 * A `clipPath`: a definition whose shape becomes a clip when something refers to it.
 *
 * The element's children are combined into one path at build time rather than kept as a tree, because a clip path has no
 * paint, no transforms of its own beyond its children's, and no nesting semantics that a single path cannot express. It
 * is a *union* of its children, which under the nonzero rule is what concatenating same-wound contours gives -- see the
 * note in the implementation about where that approximation ends.
 */
class ClipPathNode final : public Node {
  public:
  ClipPathNode(Path path, const bool objectBoundingBox)
      : mPath(std::move(path)), mObjectBoundingBox(objectBoundingBox) {}

  /** Drawing a clip path element paints nothing; it is a definition. */
  void render(Canvas&, const RenderState&) const override {}

  [[nodiscard]] const Path& path() const noexcept { return mPath; }
  /** True when `clipPathUnits="objectBoundingBox"`, so the geometry is in a unit box around whatever is clipped. */
  [[nodiscard]] bool usesObjectBoundingBox() const noexcept { return mObjectBoundingBox; }

  private:
  Path mPath;
  bool mObjectBoundingBox = false;
};

/**
 * A `<filter>`: a chain of primitives applied to an element's own rendering.
 *
 * A definition, like a mask or a pattern: drawing it in place paints nothing, and it is the element that refers to it
 * which is filtered. The primitives are the model's own -- see @c SvgFilter.h -- and this node exists so that the id
 * index can find one by name.
 */
class FilterNode final : public Node {
  public:
  void render(Canvas&, const RenderState&) const override {}
  void indexIds(IdIndex& index) const override;
  void resolveReferences(const IdIndex& index) override;

  void setFilter(Filter filter) { mFilter = std::move(filter); }
  [[nodiscard]] const Filter& filter() const noexcept { return mFilter; }

  private:
  Filter mFilter;
};

/**
 * An `<image>`: a picture the document carries inside itself.
 *
 * The bytes are decoded when the tree is built, so a document whose picture will not load is a note at load time rather
 * than a failure on every frame -- and a player redraws many times a second.
 *
 * The node holds a @c Bitmap rather than an @c Image on purpose: `Image` keeps its pixels behind a handle and
 * `Image::makeBitmap` is a deep copy, so asking for one per frame would copy several hundred kilobytes every frame for
 * no reason. The bitmap is made once, here.
 *
 * Nothing here samples the picture: `Canvas::drawBitmap` does, through a transform that scales the image into its
 * rectangle. A second sampler in this layer would be a second place for the same bug.
 */
class ImageNode final : public Node {
  public:
  ImageNode(Bitmap bitmap, Rect frame) : mBitmap(std::move(bitmap)), mFrame(frame) {}

  void render(Canvas& canvas, const RenderState& state) const override;

  [[nodiscard]] const Bitmap& bitmap() const noexcept { return mBitmap; }
  /** The rectangle the picture is placed in, in the caller's coordinates. */
  [[nodiscard]] const Rect& frame() const noexcept { return mFrame; }

  void setPreserveAspectRatio(PreserveAspectRatio value) noexcept { mAspect = value; }
  [[nodiscard]] const PreserveAspectRatio& preserveAspectRatio() const noexcept { return mAspect; }

  private:
  Bitmap mBitmap;
  Rect mFrame;
  /** The format's default is `xMidYMid meet`, which is the fit -- not the stretch, and not the cover. */
  PreserveAspectRatio mAspect;
};

/**
 * A `<pattern>`: a tile of content repeated across whatever refers to it.
 *
 * The tile is rendered once, at draw time, into a bitmap the size of the tile rectangle, and handed to an image shader
 * with `kRepeat`. Rendering it at draw time rather than at parse time is the same constraint gradients have: the tile's
 * rectangle may be measured against the element the pattern fills, which the pattern element does not know.
 */
class PatternNode final : public Node {
  public:
  /** The tile rectangle, and the two independent unit systems it and the content are written in. */
  struct Tile {
    std::optional<Length> x;
    std::optional<Length> y;
    std::optional<Length> width;
    std::optional<Length> height;
    /** `patternUnits`: true when the tile's numbers are fractions of the element's box. The format's default. */
    bool objectBoundingBox = true;
    /** `patternContentUnits`: true when the content's numbers are fractions of the element's box. Not the default. */
    bool contentObjectBoundingBox = false;
    /** `patternTransform`, applied to the finished tile in the drawing's own coordinates. */
    Matrix transform;
  };

  /** Drawing a pattern element paints nothing in place; the tile it produces is used by whoever refers to it. */
  void render(Canvas&, const RenderState&) const override {}
  void indexIds(IdIndex& index) const override;
  void resolveReferences(const IdIndex& index) override;

  void addChild(NodePtr child) { mChildren.push_back(std::move(child)); }
  [[nodiscard]] const std::vector<NodePtr>& children() const noexcept { return mChildren; }
  void setTile(Tile tile) { mTile = std::move(tile); }
  [[nodiscard]] const Tile& tile() const noexcept { return mTile; }

  private:
  std::vector<NodePtr> mChildren;
  Tile mTile;
};

/**
 * A `<mask>`: a definition whose children become the coverage that hides part of whatever refers to it.
 *
 * ## Two coordinate systems, and they are independent
 *
 * `maskUnits` says what units the *region* (`x`, `y`, `width`, `height`) is written in, and `maskContentUnits` says what
 * units the *children* are. The format lets them differ, and documents use that: nine of this corpus's ten patterns and
 * all of its masks mix the defaults, which are `objectBoundingBox` for the region and `userSpaceOnUse` for the content.
 * So the region is mapped through one transform and the content through another, and the region additionally clips the
 * content -- a mask's coverage is zero outside its own region.
 *
 * ## Luminance, because the format says so
 *
 * SVG 1.1's default is `mask-type: luminance`, and nothing in this corpus overrides it. The corpus makes that matter
 * rather than pedantic: three of its masks are filled with a black-to-white gradient whose stops are all opaque, so an
 * alpha rule would keep the entire element and a luminance rule fades it. See @c MaskMode.
 */
class MaskNode final : public Node {
  public:
  /** The region the mask applies over, and the units both halves are written in. */
  struct Region {
    std::optional<Length> x;
    std::optional<Length> y;
    std::optional<Length> width;
    std::optional<Length> height;
    /** `maskUnits`: true when the region's numbers are fractions of the element's box. The format's default. */
    bool objectBoundingBox = true;
    /** `maskContentUnits`: true when the children's numbers are fractions of the element's box. Not the default. */
    bool contentObjectBoundingBox = false;

    /**
     * `mask-type`, which decides whether the coverage is the mask's luminance or its alpha.
     *
     * The format's default is luminance, and a document overrides it -- `mask_image.svg` in this corpus does, through
     * `style="mask-type:alpha"`. Reading only the attribute spelling misses that, which is how the first measurement of
     * this corpus came to report that nothing used it.
     */
    MaskMode mode = MaskMode::kLuminance;
  };

  /** Drawing a mask element paints nothing in place; the coverage it produces is used by whoever refers to it. */
  void render(Canvas&, const RenderState&) const override {}
  void indexIds(IdIndex& index) const override;
  void resolveReferences(const IdIndex& index) override;

  void addChild(NodePtr child) { mChildren.push_back(std::move(child)); }
  [[nodiscard]] const std::vector<NodePtr>& children() const noexcept { return mChildren; }
  void setRegion(Region region) { mRegion = std::move(region); }
  [[nodiscard]] const Region& region() const noexcept { return mRegion; }

  private:
  std::vector<NodePtr> mChildren;
  Region mRegion;
};

/** The `<svg>` root: the viewport, the optional viewBox, and the children. */
class Document {
  public:
  Document() = default;
  Document(Document&&) noexcept = default;
  Document& operator=(Document&&) noexcept = default;
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;
  ~Document();

  /** The size the document asks for, in user units. */
  [[nodiscard]] Scalar width() const noexcept { return mWidth; }
  [[nodiscard]] Scalar height() const noexcept { return mHeight; }
  [[nodiscard]] bool isEmpty() const noexcept { return mRoot == nullptr; }

  /** Fills the whole document into @p bounds of @p canvas, viewBox and all. */
  void render(Canvas& canvas, const Rect& bounds) const;

  /** Renders at the document's own size, at the origin. */
  void render(Canvas& canvas) const;

  /** The root group, or null. */
  [[nodiscard]] const GroupNode* root() const noexcept { return mRoot.get(); }

  private:
  // The builder fills these in one go, after the whole tree exists: an id index cannot be built while the tree is still
  // growing, and `use` may refer to an element that appears later in the document.
  friend std::optional<Document> buildDocument(const class xml::Node& root, std::string* error);

  Scalar mWidth = 0;
  Scalar mHeight = 0;
  std::optional<Rect> mViewBox;
  PreserveAspectRatio mAspect;
  std::unique_ptr<GroupNode> mRoot;
  IdIndex mIndex;
};

/**
 * Builds a document from a parsed XML tree.
 *
 * @param error filled in when the tree is not an SVG document.
 * @return the document, or nothing.
 */
[[nodiscard]] std::optional<Document> buildDocument(const class xml::Node& root, std::string* error = nullptr);

} // namespace graphics::svg

#endif // CORAL_GRAPHICS_SVG_SVG_DOCUMENT_H
