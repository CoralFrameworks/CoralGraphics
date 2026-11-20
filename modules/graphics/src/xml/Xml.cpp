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
 * @file Xml.cpp
 *
 * The XML reader declared in Xml.h.
 *
 * ## The structure of the parse
 *
 * One pass, one cursor, no lookahead beyond what a token needs. At each point the reader is either inside an element
 * (consuming children until the matching end tag) or between them, and the two states are separate functions rather than
 * a flag, so there is no shared mutable state to get wrong.
 *
 * Everything that can fail reports through a `ParseError` carrying the line and column, and every failure stops the
 * parse: half a tree is not a thing a caller can use, and returning one would put the "is this complete" question into
 * every consumer.
 *
 * ## Two details that are easy to get wrong
 *
 * **Text is decoded, and so are attribute values.** `&amp;` in a `<text>` element means an ampersand, and a document
 * whose entity references were left as written renders as visible `&amp;`. Both paths go through the same decoder.
 *
 * **A `<script>` or `<style>` element is not special here.** XML has no such notion; CDATA is how a document says "do
 * not parse this", and it is handled wherever it appears.
 */

#include "xml/Xml.h"

#include <graphics/DecodeLimits.h>

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace graphics::xml {
namespace {

/** True for the characters XML calls whitespace, which is four of them and not the locale's idea of space. */
bool isSpace(const char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isNameStart(const char c) noexcept {
  const auto uc = static_cast<unsigned char>(c);
  // A permissive name start: ASCII letters, underscore and colon. XML also allows a wide range of Unicode, and SVG in
  // these resources does not use it; widening this later costs one predicate and no structure.
  return std::isalpha(uc) != 0 || c == '_' || c == ':';
}

bool isNameChar(const char c) noexcept {
  const auto uc = static_cast<unsigned char>(c);
  return std::isalnum(uc) != 0 || c == '_' || c == ':' || c == '-' || c == '.';
}

/** The five entities XML defines, plus the numeric forms. */
std::optional<std::string> decodeEntity(std::string_view name) {
  if (name == "amp") return std::string {"&"};
  if (name == "lt") return std::string {"<"};
  if (name == "gt") return std::string {">"};
  if (name == "quot") return std::string {"\""};
  if (name == "apos") return std::string {"'"};

  if (name.size() >= 2 && name.front() == '#') {
    const bool hex = name[1] == 'x' || name[1] == 'X';
    const std::string_view digits = name.substr(hex ? 2 : 1);
    if (digits.empty()) return std::nullopt;

    unsigned long value = 0;
    for (const char digit : digits) {
      const auto uc = static_cast<unsigned char>(digit);
      unsigned int part = 0;
      if (digit >= '0' && digit <= '9') {
        part = static_cast<unsigned int>(digit - '0');
      } else if (hex && std::isxdigit(uc) != 0) {
        part = static_cast<unsigned int>(std::tolower(uc) - 'a') + 10U;
      } else {
        return std::nullopt;
      }
      value = value * (hex ? 16UL : 10UL) + part;
      if (value > 0x10FFFFUL) return std::nullopt;
    }

    // Encoded as UTF-8, because that is what the rest of the library carries. A code point in the surrogate range is
    // rejected rather than passed through: it cannot be encoded, and silently replacing it would hide a bad document.
    if (value >= 0xD800UL && value <= 0xDFFFUL) return std::nullopt;
    std::string out;
    if (value < 0x80UL) {
      out.push_back(static_cast<char>(value));
    } else if (value < 0x800UL) {
      out.push_back(static_cast<char>(0xC0U | (value >> 6U)));
      out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value < 0x10000UL) {
      out.push_back(static_cast<char>(0xE0U | (value >> 12U)));
      out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xF0U | (value >> 18U)));
      out.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    }
    return out;
  }
  return std::nullopt;
}

/**
 * A cursor over the document, which is where the line and column reporting comes from.
 *
 * The document is held as a view rather than copied: it belongs to the caller for the whole parse, and a copy of a
 * megabyte of SVG per element would be the kind of cost that shows up as a slow load rather than as a bug.
 */
class Reader {
  public:
  explicit Reader(const std::string_view document) : mDocument(document) {}

  [[nodiscard]] bool atEnd() const noexcept { return mPosition >= mDocument.size(); }
  [[nodiscard]] size_t position() const noexcept { return mPosition; }
  [[nodiscard]] size_t line() const noexcept { return mLine; }
  [[nodiscard]] size_t column() const noexcept { return mColumn; }

  [[nodiscard]] char peek(const size_t offset = 0) const noexcept {
    const size_t index = mPosition + offset;
    return index < mDocument.size() ? mDocument[index] : '\0';
  }

  [[nodiscard]] bool startsWith(const std::string_view text) const noexcept {
    return mDocument.compare(mPosition, text.size(), text) == 0;
  }

  char advance() {
    if (atEnd()) return '\0';
    const char c = mDocument[mPosition++];
    if (c == '\n') {
      ++mLine;
      mColumn = 1;
    } else {
      ++mColumn;
    }
    return c;
  }

  void advanceBy(const size_t count) {
    for (size_t i = 0; i < count && !atEnd(); ++i) {
      advance();
    }
  }

  /** Consumes @p text when it is next, and reports whether it was. */
  bool consume(const std::string_view text) {
    if (!startsWith(text)) return false;
    advanceBy(text.size());
    return true;
  }

  void skipSpace() {
    while (!atEnd() && isSpace(peek())) {
      advance();
    }
  }

  /** @return the characters up to but not including the first one @p stop matches. */
  [[nodiscard]] std::string_view takeUntil(const char stop) {
    const size_t begin = mPosition;
    while (!atEnd() && peek() != stop) {
      advance();
    }
    return mDocument.substr(begin, mPosition - begin);
  }

  /** @return a name, or empty when the cursor is not on one. */
  [[nodiscard]] std::string_view takeName() {
    if (atEnd() || !isNameStart(peek())) return {};
    const size_t begin = mPosition;
    advance();
    while (!atEnd() && isNameChar(peek())) {
      advance();
    }
    return mDocument.substr(begin, mPosition - begin);
  }

  /**
   * @return the characters up to @p terminator, with entity references resolved.
   *
   * Raw text -- the body of a CDATA section, or an attribute value -- goes through the same decoder as everything else,
   * except that CDATA passes @p decode as false because the whole point of a CDATA section is that its contents are
   * literal.
   */
  [[nodiscard]] std::string takeText(const char terminator, const bool decode) {
    std::string out;
    while (!atEnd() && peek() != terminator) {
      if (decode && peek() == '&') {
        out += decodeReference();
        continue;
      }
      out.push_back(advance());
    }
    return out;
  }

  /** Consumes one `&...;` reference and returns what it stands for. */
  std::string decodeReference() {
    const size_t begin = mPosition;
    advance(); // the '&'
    const size_t nameBegin = mPosition;
    while (!atEnd() && peek() != ';' && peek() != '&' && !isSpace(peek())) {
      advance();
    }
    const std::string_view name = mDocument.substr(nameBegin, mPosition - nameBegin);
    if (atEnd() || peek() != ';') {
      // Not a reference after all: put the characters back and hand them through, so a stray ampersand in text renders
      // as one rather than failing the whole document.
      mPosition = begin;
      return std::string {advance()};
    }
    advance(); // the ';'

    if (const auto decoded = decodeEntity(name)) {
      return *decoded;
    }
    // A named entity XML does not define. It is kept as written rather than rejected: a document that uses one is
    // usually relying on a DTD this reader does not process, and rendering `&nbsp;` literally is easier to notice than
    // a load that fails with no picture.
    return "&" + std::string {name} + ";";
  }

  private:
  std::string_view mDocument;
  size_t mPosition = 0;
  size_t mLine = 1;
  size_t mColumn = 1;
};

/** The parser: one reader, and an error reported at most once. */
class Parser {
  public:
  explicit Parser(const std::string_view document) : mReader(document), mLimits(decodeLimits()) {}

  std::optional<Node> run(ParseError* error) {
    skipProlog();

    if (mReader.atEnd() || mReader.peek() != '<') {
      return fail(error, "expected a root element");
    }
    if (!parseElement(mRoot)) {
      report(error);
      return std::nullopt;
    }

    // Trailing content is allowed to be whitespace, comments or the closing of the prolog, and nothing else: a second
    // root element is not a well-formed document.
    skipProlog();
    if (!mReader.atEnd()) {
      return fail(error, "content after the root element");
    }
    return std::move(mRoot);
  }

  private:
  void report(ParseError* error) const {
    if (error == nullptr || !mHasError) return;
    error->line = mErrorLine;
    error->column = mErrorColumn;
    error->message = mErrorMessage;
  }

  std::optional<Node> fail(ParseError* error, const std::string& message) {
    setError(message);
    report(error);
    return std::nullopt;
  }

  void setError(const std::string& message) {
    if (mHasError) return;
    mHasError = true;
    mErrorMessage = message;
    mErrorLine = mReader.line();
    mErrorColumn = mReader.column();
  }

  /** Consumes the XML declaration, comments, DOCTYPE, and whitespace, repeatedly. */
  void skipProlog() {
    for (;;) {
      mReader.skipSpace();
      if (mReader.consume("<?")) {
        skipProcessingInstruction();
      } else if (mReader.consume("<!--")) {
        skipComment();
      } else if (mReader.consume("<!")) {
        skipDeclaration();
      } else {
        return;
      }
    }
  }

  void skipProcessingInstruction() {
    // Everything up to `?>` is the instruction. The XML declaration is one of these and carries the encoding, which is
    // deliberately ignored: a document is read as the bytes it is, and the caller decides how to decode them.
    while (!mReader.atEnd() && !mReader.consume("?>")) {
      mReader.advance();
    }
  }

  void skipComment() {
    while (!mReader.atEnd() && !mReader.consume("-->")) {
      mReader.advance();
    }
  }

  /** Consumes a `<!...>` declaration, including a DOCTYPE with an internal subset. */
  void skipDeclaration() {
    if (mReader.consume("--")) {
      skipComment();
      return;
    }
    if (mReader.consume("[CDATA[")) {
      // Not valid here -- CDATA belongs in content -- but consuming it keeps the reader in step rather than looping.
      while (!mReader.atEnd() && !mReader.consume("]]>")) {
        mReader.advance();
      }
      return;
    }

    // A DOCTYPE may carry an internal subset in brackets, and an element declaration inside that subset may itself
    // contain brackets. Counting them is what keeps `<!DOCTYPE svg [...]>` from ending at the first `>`.
    int depth = 0;
    while (!mReader.atEnd()) {
      const char c = mReader.peek();
      if (c == '[') {
        ++depth;
      } else if (c == ']') {
        --depth;
      } else if (c == '>' && depth <= 0) {
        mReader.advance();
        return;
      }
      mReader.advance();
    }
  }

  /** Parses one element, with its attributes, children and end tag, into @p out. */
  bool parseElement(Node& out) {
    // Depth and node count are checked before the element is built: a hostile document fails on its
    // shape, not after the tree has already been allocated, and the guard keeps the counter balanced on
    // every return path below.
    ++mDepth;
    struct DepthGuard {
      size_t& depth;
      ~DepthGuard() { --depth; }
    } const guard{ mDepth };
    if (mLimits.maxDepth != 0 && mDepth > mLimits.maxDepth) {
      setError("nesting is deeper than the decode limit");
      return false;
    }
    if (!mReader.consume("<")) {
      setError("expected '<'");
      return false;
    }
    const std::string_view name = mReader.takeName();
    if (name.empty()) {
      setError("expected an element name");
      return false;
    }
    if (mLimits.maxNodes != 0 && ++mNodes > mLimits.maxNodes) {
      setError("the document has more elements than the decode limit");
      return false;
    }
    out = Node::makeElement(std::string {name});

    if (!parseAttributes(out)) return false;

    mReader.skipSpace();
    if (mReader.consume("/>")) return true; // self-closing: no children, no end tag
    if (!mReader.consume(">")) {
      setError("expected '>' or '/>'");
      return false;
    }

    return parseChildren(out, name);
  }

  bool parseAttributes(Node& element) {
    for (;;) {
      mReader.skipSpace();
      const char next = mReader.peek();
      if (next == '>' || next == '/' || next == '\0') return true;

      const std::string_view name = mReader.takeName();
      if (name.empty()) {
        setError("expected an attribute name");
        return false;
      }
      mReader.skipSpace();
      if (!mReader.consume("=")) {
        setError("expected '=' after the attribute name");
        return false;
      }
      mReader.skipSpace();

      const char quote = mReader.peek();
      if (quote != '"' && quote != '\'') {
        setError("expected a quoted attribute value");
        return false;
      }
      mReader.advance();
      std::string value = mReader.takeText(quote, true);
      if (!mReader.consume(std::string_view {&quote, 1})) {
        setError("unterminated attribute value");
        return false;
      }
      element.setAttribute(std::string {name}, std::move(value));
    }
  }

  /** Consumes content until the end tag matching @p name. */
  bool parseChildren(Node& element, const std::string_view name) {
    std::string text;
    const auto flushText = [&element, &text] {
      if (!text.empty()) {
        element.addChild(Node::makeText(std::move(text)));
        text.clear();
      }
    };

    for (;;) {
      if (mReader.atEnd()) {
        setError("unclosed element <" + std::string {name} + ">");
        return false;
      }

      if (mReader.peek() == '<') {
        if (mReader.startsWith("</")) {
          flushText();
          mReader.advanceBy(2);
          const std::string_view closing = mReader.takeName();
          mReader.skipSpace();
          if (!mReader.consume(">")) {
            setError("expected '>' after the end tag name");
            return false;
          }
          // The names must match: an end tag that closes a different element is the mistake that produces a tree
          // nobody expects, and it is cheap to catch here.
          if (closing != name) {
            setError("</" + std::string {closing} + "> does not close <" + std::string {name} + ">");
            return false;
          }
          return true;
        }
        if (mReader.startsWith("<!--")) {
          mReader.advanceBy(4);
          skipComment();
          continue;
        }
        if (mReader.startsWith("<![CDATA[")) {
          mReader.advanceBy(9);
          // Literal: that is what CDATA means, so the ampersands stay ampersands.
          text += mReader.takeText(']', false);
          if (!mReader.consume("]]>")) {
            setError("unterminated CDATA section");
            return false;
          }
          continue;
        }
        if (mReader.startsWith("<?")) {
          mReader.advanceBy(2);
          skipProcessingInstruction();
          continue;
        }
        if (mReader.startsWith("<!")) {
          mReader.advanceBy(2);
          skipDeclaration();
          continue;
        }

        flushText();
        Node child;
        if (!parseElement(child)) return false;
        element.addChild(std::move(child));
        continue;
      }

      if (mReader.peek() == '&') {
        text += mReader.decodeReference();
        continue;
      }
      text.push_back(mReader.advance());
    }
  }

  Reader mReader;
  /** The bounds read once at construction: a document is parsed against one set of limits. */
  const DecodeLimits mLimits;
  /** Current element nesting, and elements seen so far; both feed the decode limits. */
  size_t mDepth = 0;
  size_t mNodes = 0;
  Node mRoot;
  bool mHasError = false;
  std::string mErrorMessage;
  size_t mErrorLine = 1;
  size_t mErrorColumn = 1;
};

} // namespace anonymous

Node::~Node() = default;

Node Node::makeElement(std::string name) {
  Node node;
  node.mType = Type::Element;
  node.mName = std::move(name);
  return node;
}

Node Node::makeText(std::string text) {
  Node node;
  node.mType = Type::Text;
  node.mText = std::move(text);
  return node;
}

Node& Node::addChild(Node child) {
  mChildren.push_back(std::make_unique<Node>(std::move(child)));
  return *mChildren.back();
}

void Node::setAttribute(std::string name, std::string value) {
  // Replacing rather than appending a duplicate: XML forbids the same attribute twice, and keeping the last one is what
  // every renderer does with a document that breaks that rule.
  const auto existing = std::find_if(mAttributes.begin(), mAttributes.end(),
                                     [&name](const Attribute& attribute) { return attribute.name == name; });
  if (existing != mAttributes.end()) {
    existing->value = std::move(value);
    return;
  }
  mAttributes.push_back(Attribute {std::move(name), std::move(value)});
}

bool Node::hasAttribute(const std::string_view name) const noexcept {
  return attribute(name).has_value();
}

std::optional<std::string_view> Node::attribute(const std::string_view name) const noexcept {
  for (const Attribute& attribute : mAttributes) {
    if (attribute.name == name) return attribute.value;
  }
  return std::nullopt;
}

const Node* Node::firstChild(const std::string_view name) const noexcept {
  for (const NodePtr& child : mChildren) {
    if (!child->isElement()) continue;
    if (name.empty() || child->name() == name) return child.get();
  }
  return nullptr;
}

bool Node::isWhitespace() const noexcept {
  if (mType != Type::Text) return false;
  return std::all_of(mText.begin(), mText.end(), [](const char c) { return isSpace(c); });
}

std::string Node::textContent() const {
  if (mType == Type::Text) return mText;
  std::string out;
  for (const NodePtr& child : mChildren) {
    out += child->textContent();
  }
  return out;
}

std::string ParseError::describe() const {
  return "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message;
}

std::optional<Node> parse(const std::string_view document, ParseError* error) {
  if (error != nullptr) *error = ParseError {};
  Parser parser(document);
  return parser.run(error);
}

} // namespace graphics::xml
