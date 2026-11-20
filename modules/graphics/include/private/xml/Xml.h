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

#ifndef CORAL_GRAPHICS_XML_XML_H
#define CORAL_GRAPHICS_XML_XML_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file Xml.h
 *
 * A small XML reader, enough for SVG.
 *
 * ## Why this exists rather than a dependency
 *
 * Skia's SVG module parses through Expat, and Expat is not vendored here. Adding it would mean a new third-party library
 * for one file format, and the part of XML an SVG document actually uses is narrow: elements, attributes, text, comments,
 * CDATA and the five predefined entities. That is a few hundred lines of ordinary parsing, and writing them keeps the
 * dependency list as it is -- which is a requirement of this library rather than a preference.
 *
 * What is deliberately *not* supported, because no SVG in the resources uses it and guessing would be worse than
 * refusing: external entity resolution (which is also the classic XML attack surface), DTD validation, and namespace
 * resolution. Names are carried through verbatim, colons and all, and the SVG layer decides what a prefix means.
 *
 * ## The shape of the result
 *
 * A document becomes a tree of `Node`s: an element carries a name, its attributes and its children; a text node carries a
 * run of characters. Whitespace is preserved rather than trimmed, because in SVG it is significant inside `text` and
 * insignificant between elements, and only the caller knows which it is looking at.
 *
 * Ownership is a `std::vector<std::unique_ptr<Node>>` per element, so a tree frees itself and cannot be copied into a
 * second owner. The whole structure is movable, which is what lets `parse` return it.
 */

namespace graphics::xml {

/** One attribute of an element, in document order. */
struct Attribute {
  std::string name;
  std::string value;
};

class Node;
using NodePtr = std::unique_ptr<Node>;

/**
 * One node of the document: either an element with children, or a run of text.
 *
 * A node is an element or it is text, never both, and the accessors that belong to the other kind return empty rather
 * than asserting: a caller walking a tree asks `name()` of children it has already filtered, and making that a
 * precondition would put a check on the hot path of every render for no gain.
 */
class Node {
  public:
  enum class Type : uint8_t { Element, Text };

  Node() = default;
  Node(Node&&) noexcept = default;
  Node& operator=(Node&&) noexcept = default;
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;
  ~Node();

  /** @return an element node with @p name and no children. */
  static Node makeElement(std::string name);
  /** @return a text node holding @p text. */
  static Node makeText(std::string text);

  /** @return a child appended at the end, owned by this node. */
  Node& addChild(Node child);
  /** Appends @p attribute, or replaces the value of an attribute of the same name. */
  void setAttribute(std::string name, std::string value);

  [[nodiscard]] Type type() const noexcept { return mType; }
  [[nodiscard]] bool isElement() const noexcept { return mType == Type::Element; }
  [[nodiscard]] bool isText() const noexcept { return mType == Type::Text; }

  /** @return the element's name, or empty for a text node. */
  [[nodiscard]] const std::string& name() const noexcept { return mName; }
  [[nodiscard]] const std::vector<Attribute>& attributes() const noexcept { return mAttributes; }
  [[nodiscard]] const std::vector<NodePtr>& children() const noexcept { return mChildren; }
  /** @return the text node's characters, or empty for an element. */
  [[nodiscard]] const std::string& text() const noexcept { return mText; }

  [[nodiscard]] bool hasAttribute(std::string_view name) const noexcept;
  /** @return the value of @p name, or nothing when the element does not carry it. */
  [[nodiscard]] std::optional<std::string_view> attribute(std::string_view name) const noexcept;

  /** @return the first child element, optionally the first one named @p name. */
  [[nodiscard]] const Node* firstChild(std::string_view name = {}) const noexcept;

  /**
   * @return every character in this node's subtree, in document order.
   *
   * SVG puts a `<text>` element's content in a mix of child text nodes and `<tspan>` elements, so asking the element
   * directly is what a caller wants far more often than walking the children itself.
   */
  [[nodiscard]] std::string textContent() const;

  /** @return true when this node is text and holds nothing but whitespace. */
  [[nodiscard]] bool isWhitespace() const noexcept;

  private:
  Type mType = Type::Element;
  std::string mName;
  std::string mText;
  std::vector<Attribute> mAttributes;
  std::vector<NodePtr> mChildren;
};

/** Where a parse failed, in terms a person can act on. */
struct ParseError {
  size_t line = 1;
  size_t column = 1;
  std::string message;

  /** @return "line N, column M: message". */
  [[nodiscard]] std::string describe() const;
};

/**
 * Parses @p document and returns its root element.
 *
 * @param error filled in on failure; may be null when the caller does not want the reason.
 * @return the root element, or nothing when the document is not well-formed. A document with no root element -- empty,
 *         or nothing but comments -- is a failure, not an empty tree.
 */
[[nodiscard]] std::optional<Node> parse(std::string_view document, ParseError* error = nullptr);

} // namespace graphics::xml

#endif // CORAL_GRAPHICS_XML_XML_H
