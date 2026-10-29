#include "PixelDSL.hpp"

#include <nlohmann/json.hpp>
#include <raylib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace PixelDSL {
namespace {

// ---------------------------------------------------------------------------
// Small value types
// ---------------------------------------------------------------------------

struct Rgba {
  unsigned char r = 0, g = 0, b = 0, a = 0;

  bool operator==(const Rgba &other) const {
    return r == other.r && g == other.g && b == other.b && a == other.a;
  }
  bool operator<(const Rgba &other) const {
    if (r != other.r) return r < other.r;
    if (g != other.g) return g < other.g;
    if (b != other.b) return b < other.b;
    return a < other.a;
  }
};

std::uint32_t PackRgb(const Rgba &c) {
  return (static_cast<std::uint32_t>(c.r) << 16) |
         (static_cast<std::uint32_t>(c.g) << 8) | c.b;
}

struct Ramp {
  std::string name;
  std::vector<Rgba> steps;
  Rgba base{};
  bool hasBase = false;
};

struct Ramps {
  bool loaded = false;
  std::string path;
  std::string error;
  std::map<std::string, Ramp> byName;
};

struct PaletteReport {
  bool loaded = false;
  std::string error;
  std::set<std::uint32_t> colours;
};

// ---------------------------------------------------------------------------
// Ramps and the pack palette
// ---------------------------------------------------------------------------

bool LoadRamps(const std::string &path, Ramps *out) {
  out->path = path;
  std::ifstream file(path);
  if (!file.is_open()) {
    out->error = "could not read " + path +
                 " (ramps are derived from the art pack: run "
                 "tools/derive_ramps.py)";
    return false;
  }
  nlohmann::json root;
  try {
    file >> root;
  } catch (const std::exception &e) {
    out->error = std::string("malformed ramps file ") + path + ": " + e.what();
    return false;
  }
  if (!root.contains("ramps") || !root["ramps"].is_object()) {
    out->error = "ramps file " + path + " has no \"ramps\" object";
    return false;
  }
  for (auto it = root["ramps"].begin(); it != root["ramps"].end(); ++it) {
    Ramp ramp;
    ramp.name = it.key();
    const nlohmann::json &entry = it.value();
    if (entry.contains("steps") && entry["steps"].is_array()) {
      for (const nlohmann::json &step : entry["steps"]) {
        if (!step.is_array() || step.size() < 3) continue;
        Rgba colour;
        colour.r = static_cast<unsigned char>(step[0].get<int>());
        colour.g = static_cast<unsigned char>(step[1].get<int>());
        colour.b = static_cast<unsigned char>(step[2].get<int>());
        colour.a = 255;
        ramp.steps.push_back(colour);
      }
    }
    if (entry.contains("base") && entry["base"].is_array() &&
        entry["base"].size() >= 3) {
      ramp.base.r = static_cast<unsigned char>(entry["base"][0].get<int>());
      ramp.base.g = static_cast<unsigned char>(entry["base"][1].get<int>());
      ramp.base.b = static_cast<unsigned char>(entry["base"][2].get<int>());
      ramp.base.a = 255;
      ramp.hasBase = true;
    }
    if (ramp.steps.empty()) continue;
    out->byName[ramp.name] = ramp;
  }
  out->loaded = !out->byName.empty();
  if (!out->loaded) out->error = "ramps file " + path + " defines no usable ramp";
  return out->loaded;
}

PaletteReport LoadPaletteReport(const std::string &path) {
  PaletteReport report;
  if (path.empty()) return report;
  std::ifstream file(path);
  if (!file.is_open()) {
    report.error = "could not read " + path;
    return report;
  }
  nlohmann::json root;
  try {
    file >> root;
  } catch (const std::exception &e) {
    report.error = std::string("malformed palette ") + path + ": " + e.what();
    return report;
  }
  if (!root.contains("colors") || !root["colors"].is_array()) {
    report.error = "palette " + path + " has no \"colors\" array";
    return report;
  }
  for (const nlohmann::json &entry : root["colors"]) {
    if (!entry.is_array() || entry.size() < 3) continue;
    Rgba colour;
    colour.r = static_cast<unsigned char>(entry[0].get<int>());
    colour.g = static_cast<unsigned char>(entry[1].get<int>());
    colour.b = static_cast<unsigned char>(entry[2].get<int>());
    report.colours.insert(PackRgb(colour));
  }
  report.loaded = !report.colours.empty();
  return report;
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

struct Token {
  enum class Type { Word, Number, Dimension, Hex, String, Punct, End } type =
      Type::End;
  std::string text;
  int line = 1;
  int column = 1;
};

bool IsWordStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHexDigit(char c) {
  return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::vector<Token> Lex(const std::string &text, std::vector<Diagnostic> *diags,
                       bool *ok) {
  std::vector<Token> tokens;
  int line = 1;
  int column = 1;
  size_t i = 0;
  auto advance = [&](size_t count) {
    for (size_t k = 0; k < count && i < text.size(); ++k) {
      if (text[i] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
      ++i;
    }
  };
  auto push = [&](Token::Type type, std::string value, int tokenLine,
                  int tokenColumn) {
    Token token;
    token.type = type;
    token.text = std::move(value);
    token.line = tokenLine;
    token.column = tokenColumn;
    tokens.push_back(std::move(token));
  };

  while (i < text.size()) {
    const char c = text[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance(1);
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') advance(1);
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      const int startLine = line;
      const int startColumn = column;
      advance(2);
      bool closed = false;
      while (i + 1 < text.size()) {
        if (text[i] == '*' && text[i + 1] == '/') {
          advance(2);
          closed = true;
          break;
        }
        advance(1);
      }
      if (!closed) {
        diags->push_back({Severity::Error, startLine, startColumn,
                          "unterminated /* comment"});
        *ok = false;
        return tokens;
      }
      continue;
    }
    const int tokenLine = line;
    const int tokenColumn = column;
    if (c == '#') {
      size_t j = i + 1;
      while (j < text.size() && IsHexDigit(text[j])) ++j;
      const size_t digits = j - (i + 1);
      if (digits != 6 && digits != 8) {
        diags->push_back({Severity::Error, tokenLine, tokenColumn,
                          "a colour literal is #rrggbb or #rrggbbaa, with 6 or 8 "
                          "hex digits"});
        *ok = false;
        return tokens;
      }
      push(Token::Type::Hex, text.substr(i, j - i), tokenLine, tokenColumn);
      advance(j - i);
      continue;
    }
    if (IsWordStart(c)) {
      size_t j = i;
      while (j < text.size()) {
        const char d = text[j];
        if (IsWordStart(d) || IsDigit(d) || d == '.') {
          ++j;
          continue;
        }
        break;
      }
      push(Token::Type::Word, text.substr(i, j - i), tokenLine, tokenColumn);
      advance(j - i);
      continue;
    }
    if (IsDigit(c) || (c == '-' && i + 1 < text.size() && IsDigit(text[i + 1]))) {
      // A leading '-' is part of the number: a wrapped shape's polygon routinely
      // starts left of x=0, and requiring the author to shift it is the kind of
      // papercut that makes a language annoying for no gain.
      size_t j = i;
      if (text[j] == '-') ++j;
      while (j < text.size() && IsDigit(text[j])) ++j;
      // A decimal is one token: `0.35` must not depend on the host locale, and
      // splitting it into Number/Punct/Number would make every caller reassemble
      // it.
      if (j + 1 < text.size() && text[j] == '.' && IsDigit(text[j + 1])) {
        ++j;
        while (j < text.size() && IsDigit(text[j])) ++j;
        push(Token::Type::Number, text.substr(i, j - i), tokenLine, tokenColumn);
        advance(j - i);
        continue;
      }
      // `32x32` is one token. The canvas size is the only place a bare `NxM`
      // appears, and lexing it as Number + Word("x32") would make the parser
      // carry a special case forever.
      if (j < text.size() && (text[j] == 'x' || text[j] == 'X') &&
          j + 1 < text.size() && IsDigit(text[j + 1])) {
        size_t k = j + 1;
        while (k < text.size() && IsDigit(text[k])) ++k;
        push(Token::Type::Dimension, text.substr(i, k - i), tokenLine, tokenColumn);
        advance(k - i);
        continue;
      }
      push(Token::Type::Number, text.substr(i, j - i), tokenLine, tokenColumn);
      advance(j - i);
      continue;
    }
    if (c == '"') {
      size_t j = i + 1;
      std::string value;
      while (j < text.size() && text[j] != '"') {
        if (text[j] == '\n') break;
        value += text[j];
        ++j;
      }
      if (j >= text.size() || text[j] != '"') {
        diags->push_back({Severity::Error, tokenLine, tokenColumn,
                          "unterminated string"});
        *ok = false;
        return tokens;
      }
      push(Token::Type::String, value, tokenLine, tokenColumn);
      advance(j - i + 1);
      continue;
    }
    if (c == '(' || c == ')' || c == ',' || c == '{' || c == '}' || c == '=') {
      push(Token::Type::Punct, std::string(1, c), tokenLine, tokenColumn);
      advance(1);
      continue;
    }
    diags->push_back({Severity::Error, tokenLine, tokenColumn,
                      std::string("unexpected character '") + c + "'"});
    *ok = false;
    return tokens;
  }
  Token end;
  end.type = Token::Type::End;
  end.line = line;
  end.column = column;
  tokens.push_back(end);
  return tokens;
}

// ---------------------------------------------------------------------------
// AST
// ---------------------------------------------------------------------------

struct Point {
  int x = 0;
  int y = 0;
};

struct Op {
  enum class Kind {
    Fill,
    Pixel,
    Rect,
    Line,
    Circle,
    Polygon,
    Area,
    Scatter,
  };
  Kind kind = Kind::Fill;
  bool erase = false;
  std::vector<Point> points;
  int radius = 0;
  std::string color;
  std::vector<std::string> colors;
  double density = 0.0;
  bool hasDensity = false;
  long long seed = 0;
  bool hasSeed = false;
  int cluster = 1;
  bool inside = false;
  int line = 0;
  int column = 0;
};

struct FrameAst {
  std::string name;
  int line = 0;
  std::vector<Op> ops;
};

struct Document {
  int version = 1;
  bool hasSprite = false;
  int width = 0;
  int height = 0;
  std::string kind = "tile";
  std::string material;
  int wrap = -1;  // -1 = per-kind default
  std::vector<std::string> paletteNames;
  std::map<std::string, std::string> paletteRaw;
  std::vector<std::string> paletteLines;  // parallel positions, for diagnostics
  std::vector<FrameAst> frames;
  std::vector<Op> ops;
  bool hasFrames = false;
  bool hasOps = false;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class Parser {
 public:
  Parser(std::vector<Token> tokens, std::vector<Diagnostic> *diags)
      : tokens_(std::move(tokens)), diags_(diags) {}

  bool Parse(Document *doc) {
    while (Peek().type != Token::Type::End) {
      const Token &token = Peek();
      if (token.type != Token::Type::Word) {
        Fail(token, "expected a keyword");
        return false;
      }
      const std::string &word = token.text;
      if (word == "version") {
        if (!ParseVersion(doc)) return false;
      } else if (word == "sprite") {
        if (!ParseSprite(doc)) return false;
      } else if (word == "kind") {
        Take();
        if (Peek().type != Token::Type::Word) {
          Fail(Peek(), "kind needs a value: tile or object");
          return false;
        }
        doc->kind = Take().text;
        if (doc->kind != "tile" && doc->kind != "object") {
          Fail(token, "kind must be tile or object, not '" + doc->kind + "'");
          return false;
        }
      } else if (word == "material") {
        Take();
        if (Peek().type != Token::Type::Word) {
          Fail(Peek(), "material needs a name from the art pack's ramps");
          return false;
        }
        doc->material = Take().text;
      } else if (word == "wrap") {
        Take();
        if (Peek().type != Token::Type::Word) {
          Fail(Peek(), "wrap needs on or off");
          return false;
        }
        const std::string value = Take().text;
        if (value != "on" && value != "off") {
          Fail(token, "wrap must be on or off, not '" + value + "'");
          return false;
        }
        doc->wrap = value == "on" ? 1 : 0;
      } else if (word == "palette") {
        if (!ParsePalette(doc)) return false;
      } else if (word == "frame") {
        if (!ParseFrame(doc)) return false;
      } else if (IsOpWord(word)) {
        if (doc->hasFrames) {
          Fail(token,
               "a document is either a list of operations or a list of frames, "
               "not both: move this operation inside a frame");
          return false;
        }
        Op op;
        if (!ParseOp(&op)) return false;
        doc->ops.push_back(std::move(op));
        doc->hasOps = true;
      } else {
        Fail(token, "unknown keyword '" + word + "'");
        return false;
      }
    }

    if (!doc->hasSprite) {
      Error(doc->frames.empty() ? 1 : 1, 1,
            "the document never declares a size: add `sprite 32x32`");
      return false;
    }
    if (!doc->hasOps && !doc->hasFrames) {
      Error(1, 1, "the document has no operations, so it renders nothing");
      return false;
    }
    if (doc->hasFrames) {
      std::set<std::string> names;
      for (const FrameAst &frame : doc->frames) {
        if (!names.insert(frame.name).second) {
          Error(frame.line, 1,
                "two frames are called \"" + frame.name + "\"; names are how a "
                "variant is referenced, so they must be unique");
          return false;
        }
      }
    }
    return true;
  }

 private:
  static bool IsOpWord(const std::string &word) {
    return word == "fill" || word == "pixel" || word == "rect" ||
           word == "line" || word == "circle" || word == "polygon" ||
           word == "fill_area" || word == "scatter" || word == "erase";
  }

  const Token &Peek(int ahead = 0) const {
    const size_t index = static_cast<size_t>(cursor_ + ahead);
    if (index >= tokens_.size()) return tokens_.back();
    return tokens_[index];
  }
  Token Take() {
    const Token token = Peek();
    if (cursor_ < static_cast<int>(tokens_.size()) - 1) ++cursor_;
    return token;
  }
  bool MatchPunct(const char *text) {
    if (Peek().type == Token::Type::Punct && Peek().text == text) {
      Take();
      return true;
    }
    return false;
  }
  void Error(int line, int column, const std::string &message) {
    diags_->push_back({Severity::Error, line, column, message});
  }
  void Fail(const Token &token, const std::string &message) {
    Error(token.line, token.column, message);
  }
  bool ExpectPunct(const char *text, const char *context) {
    if (MatchPunct(text)) return true;
    Fail(Peek(), std::string("expected '") + text + "' " + context);
    return false;
  }
  bool ExpectNumber(int *value, const std::string &context) {
    if (Peek().type != Token::Type::Number) {
      Fail(Peek(), "expected a whole number " + context);
      return false;
    }
    *value = std::atoi(Take().text.c_str());
    return true;
  }
  bool ExpectWord(std::string *value, const char *context) {
    if (Peek().type != Token::Type::Word) {
      Fail(Peek(), std::string("expected a word ") + context);
      return false;
    }
    *value = Take().text;
    return true;
  }
  bool ParseDecimal(double *value, const char *context) {
    // The lexer keeps the fractional part inside the token, so this is only about
    // refusing a word where a number belongs. std::strtod is deliberately not used:
    // the decimal separator must not depend on the host locale.
    if (Peek().type != Token::Type::Number) {
      Fail(Peek(), std::string("expected a number ") + context);
      return false;
    }
    *value = std::atof(Take().text.c_str());
    return true;
  }
  bool ParseVersion(Document *doc) {
    const Token keyword = Take();
    if (Peek().type != Token::Type::Number) {
      Fail(Peek(), "version needs a number");
      return false;
    }
    doc->version = std::atoi(Take().text.c_str());
    if (doc->version != 1) {
      Fail(keyword, "this compiler reads version 1 documents, not version " +
                        std::to_string(doc->version));
      return false;
    }
    return true;
  }
  bool ParseSprite(Document *doc) {
    const Token keyword = Take();
    if (doc->hasSprite) {
      Fail(keyword, "the size is declared twice");
      return false;
    }
    if (Peek().type != Token::Type::Dimension) {
      Fail(Peek(), "sprite needs a size like 32x32");
      return false;
    }
    const std::string text = Take().text;
    const size_t x = text.find_first_of("xX");
    doc->width = std::atoi(text.substr(0, x).c_str());
    doc->height = std::atoi(text.substr(x + 1).c_str());
    doc->hasSprite = true;
    if (doc->width != doc->height) {
      Fail(keyword, "a sprite is square; " + text + " is not");
      return false;
    }
    if (doc->width < 8 || doc->width > 128) {
      Fail(keyword, "a sprite is 8 to 128 pixels; " + text + " is outside that");
      return false;
    }
    return true;
  }
  bool ParsePalette(Document *doc) {
    Take();  // palette
    if (!ExpectPunct("{", "to open the palette block")) return false;
    while (!MatchPunct("}")) {
      if (Peek().type == Token::Type::End) {
        Fail(Peek(), "unterminated palette block");
        return false;
      }
      if (Peek().type != Token::Type::Word) {
        Fail(Peek(), "expected a colour name in the palette block");
        return false;
      }
      const Token name = Take();
      if (doc->paletteRaw.count(name.text)) {
        Fail(name, "the palette already defines '" + name.text + "'");
        return false;
      }
      if (!ExpectPunct("=", "after the colour name")) return false;
      if (Peek().type != Token::Type::Word && Peek().type != Token::Type::Hex) {
        Fail(Peek(), "expected a ramp reference (wood.2, wood.base) or a #hex literal");
        return false;
      }
      const Token value = Take();
      doc->paletteNames.push_back(name.text);
      doc->paletteRaw[name.text] = value.text;
      doc->paletteLines.push_back(std::to_string(name.line) + ":" +
                                  std::to_string(name.column));
    }
    return true;
  }
  bool ParseFrame(Document *doc) {
    const Token keyword = Take();  // frame
    if (doc->hasOps) {
      Fail(keyword,
           "a document is either a list of operations or a list of frames, not "
           "both: move the top-level operations into a frame");
      return false;
    }
    if (Peek().type != Token::Type::String) {
      Fail(Peek(), "a frame needs a quoted name, e.g. frame \"mature\" { ... }");
      return false;
    }
    FrameAst frame;
    frame.name = Take().text;
    frame.line = keyword.line;
    if (!ExpectPunct("{", "to open the frame")) return false;
    while (!MatchPunct("}")) {
      if (Peek().type == Token::Type::End) {
        Fail(Peek(), "unterminated frame \"" + frame.name + "\"");
        return false;
      }
      if (Peek().type != Token::Type::Word || !IsOpWord(Peek().text)) {
        Fail(Peek(), "a frame holds operations only; '" + Peek().text +
                         "' is not one");
        return false;
      }
      if (Peek().text == "frame") {
        Fail(Peek(), "frames do not nest");
        return false;
      }
      Op op;
      if (!ParseOp(&op)) return false;
      frame.ops.push_back(std::move(op));
    }
    if (frame.ops.empty()) {
      Error(frame.line, 1, "frame \"" + frame.name + "\" is empty");
      return false;
    }
    doc->hasFrames = true;
    doc->frames.push_back(std::move(frame));
    return true;
  }
  bool ParsePoint(Point *point) {
    if (!ExpectPunct("(", "to start a coordinate")) return false;
    if (!ExpectNumber(&point->x, "for x")) return false;
    if (!ExpectPunct(",", "between x and y")) return false;
    if (!ExpectNumber(&point->y, "for y")) return false;
    if (!ExpectPunct(")", "to close a coordinate")) return false;
    return true;
  }
  bool ParseColorValue(std::string *out, const char *context) {
    if (Peek().type != Token::Type::Word || Peek().text != "color") {
      Fail(Peek(), std::string("expected color=<colour> ") + context);
      return false;
    }
    Take();  // color
    if (!ExpectPunct("=", "after color")) return false;
    if (Peek().type != Token::Type::Word && Peek().type != Token::Type::Hex) {
      Fail(Peek(), std::string("expected a colour ") + context);
      return false;
    }
    *out = Take().text;
    return true;
  }
  bool ParseOp(Op *op) {
    const Token keyword = Take();
    op->line = keyword.line;
    op->column = keyword.column;
    const std::string &word = keyword.text;

    if (word == "erase") {
      op->erase = true;
      if (Peek().type != Token::Type::Word || !IsOpWord(Peek().text)) {
        Fail(Peek(), "erase needs a shape: pixel, rect, line, circle, polygon, "
                     "fill_area");
        return false;
      }
      const std::string shape = Take().text;
      if (shape == "scatter" || shape == "fill" || shape == "erase") {
        Fail(keyword, "erase " + shape + " is not a thing");
        return false;
      }
      return ParseShape(shape, op, /*needsColor=*/false);
    }
    if (word == "fill") {
      op->kind = Op::Kind::Fill;
      if (Peek().type != Token::Type::Word && Peek().type != Token::Type::Hex) {
        Fail(Peek(), "fill needs a colour");
        return false;
      }
      op->color = Take().text;
      return true;
    }
    if (word == "scatter") {
      op->kind = Op::Kind::Scatter;
      return ParseScatter(op);
    }
    // Everything else is a shape with a mandatory color= argument.
    op->kind = Op::Kind::Pixel;
    return ParseShape(word, op, /*needsColor=*/true);
  }
  bool ParseShape(const std::string &shape, Op *op, bool needsColor) {
    if (shape == "pixel") {
      op->kind = Op::Kind::Pixel;
      if (!ParsePoint(&op->points.emplace_back())) return false;
    } else if (shape == "rect") {
      op->kind = Op::Kind::Rect;
      if (!ParsePoint(&op->points.emplace_back())) return false;
      if (!ParsePoint(&op->points.emplace_back())) return false;
    } else if (shape == "line") {
      op->kind = Op::Kind::Line;
      if (!ParsePoint(&op->points.emplace_back())) return false;
      if (!ParsePoint(&op->points.emplace_back())) return false;
    } else if (shape == "circle") {
      op->kind = Op::Kind::Circle;
      if (!ParsePoint(&op->points.emplace_back())) return false;
      if (!ParseKeyNumber("radius", &op->radius)) return false;
      if (op->radius < 0 || op->radius > 64) {
        Error(op->line, op->column,
              "circle radius is 0 to 64 pixels, not " + std::to_string(op->radius));
        return false;
      }
    } else if (shape == "polygon") {
      op->kind = Op::Kind::Polygon;
      while (Peek().type == Token::Type::Punct && Peek().text == "(") {
        if (!ParsePoint(&op->points.emplace_back())) return false;
      }
      if (op->points.size() < 3) {
        Error(op->line, op->column,
              "a polygon needs at least three vertices; this one has " +
                  std::to_string(op->points.size()));
        return false;
      }
    } else if (shape == "fill_area") {
      op->kind = Op::Kind::Area;
      if (!ParsePoint(&op->points.emplace_back())) return false;
    } else {
      Error(op->line, op->column, "unknown shape '" + shape + "'");
      return false;
    }
    if (needsColor) {
      if (!ParseColorValue(&op->color, "after the shape's arguments")) return false;
    } else {
      op->color.clear();
    }
    return true;
  }
  bool ParseKeyNumber(const char *key, int *value) {
    if (Peek().type != Token::Type::Word || Peek().text != key) {
      Fail(Peek(), std::string("expected ") + key + "=<number>");
      return false;
    }
    Take();
    if (!ExpectPunct("=", "after radius")) return false;
    return ExpectNumber(value, std::string("for ") + key);
  }
  bool ParseScatter(Op *op) {
    if (Peek().type != Token::Type::Word || Peek().text != "color") {
      Fail(Peek(), "scatter needs color=<ramp entry>[,<ramp entry>...]");
      return false;
    }
    Take();
    if (!ExpectPunct("=", "after color")) return false;
    while (true) {
      if (Peek().type != Token::Type::Word && Peek().type != Token::Type::Hex) {
        Fail(Peek(), "expected a colour in the scatter list");
        return false;
      }
      op->colors.push_back(Take().text);
      if (!MatchPunct(",")) break;
    }
    while (Peek().type == Token::Type::Word) {
      const std::string key = Peek().text;
      if (key == "density") {
        Take();
        if (!ExpectPunct("=", "after density")) return false;
        if (!ParseDecimal(&op->density, "for density")) return false;
        op->hasDensity = true;
      } else if (key == "seed") {
        Take();
        if (!ExpectPunct("=", "after seed")) return false;
        if (Peek().type != Token::Type::Number) {
          Fail(Peek(), "seed needs a whole number");
          return false;
        }
        op->seed = std::atoll(Take().text.c_str());
        op->hasSeed = true;
      } else if (key == "cluster") {
        Take();
        if (!ExpectPunct("=", "after cluster")) return false;
        if (!ExpectNumber(&op->cluster, "for cluster")) return false;
      } else if (key == "inside") {
        Take();
        op->inside = true;
      } else {
        break;
      }
    }
    if (op->colors.empty()) {
      Fail(Peek(), "scatter needs at least one colour");
      return false;
    }
    if (!op->hasDensity) {
      Error(op->line, op->column,
            "scatter needs density=<0..1>: without it there is no way to say how "
            "much grain to add");
      return false;
    }
    if (!op->hasSeed) {
      Error(op->line, op->column,
            "scatter needs seed=<n>: a scattered texture that changes every "
            "compile cannot be reviewed or diffed");
      return false;
    }
    if (op->density < 0.0 || op->density > 1.0) {
      Error(op->line, op->column,
            "scatter density is a fraction of the canvas, so it is between 0 and "
            "1, not " + std::to_string(op->density));
      return false;
    }
    if (op->cluster < 1 || op->cluster > 16) {
      Error(op->line, op->column, "scatter cluster is 1 to 16 pixels, not " +
                                      std::to_string(op->cluster));
      return false;
    }
    return true;
  }

  std::vector<Token> tokens_;
  std::vector<Diagnostic> *diags_;
  int cursor_ = 0;
};

// ---------------------------------------------------------------------------
// Colour resolution
// ---------------------------------------------------------------------------

struct ColorContext {
  const Ramps *ramps = nullptr;
  const std::map<std::string, Rgba> *palette = nullptr;
  std::map<Rgba, bool> *provenance = nullptr;  // true when the colour came from a ramp
};

bool ParseHexColour(const std::string &text, Rgba *out) {
  if (text.size() != 7 && text.size() != 9) return false;
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  int values[4] = {0, 0, 0, 255};
  const size_t components = (text.size() - 1) / 2;
  for (size_t index = 0; index < components; ++index) {
    const int high = hex(text[1 + index * 2]);
    const int low = hex(text[2 + index * 2]);
    if (high < 0 || low < 0) return false;
    values[index] = high * 16 + low;
  }
  out->r = static_cast<unsigned char>(values[0]);
  out->g = static_cast<unsigned char>(values[1]);
  out->b = static_cast<unsigned char>(values[2]);
  out->a = static_cast<unsigned char>(values[3]);
  return true;
}

bool ResolveColor(const std::string &raw, const ColorContext &context, Rgba *out,
                  std::string *error) {
  if (!raw.empty() && raw[0] == '#') {
    if (!ParseHexColour(raw, out)) {
      *error = "'" + raw + "' is not a valid #rrggbb or #rrggbbaa literal";
      return false;
    }
    if (context.provenance != nullptr) (*context.provenance)[*out] = false;
    return true;
  }
  if (context.palette != nullptr) {
    const auto found = context.palette->find(raw);
    if (found != context.palette->end()) {
      *out = found->second;
      return true;
    }
  }
  const size_t dot = raw.find_last_of('.');
  if (dot != std::string::npos && dot > 0 && dot + 1 < raw.size()) {
    const std::string rampName = raw.substr(0, dot);
    const std::string index = raw.substr(dot + 1);
    if (context.ramps == nullptr || !context.ramps->loaded) {
      *error = "'" + raw + "' needs the ramp '" + rampName +
               "', but " +
               (context.ramps != nullptr && !context.ramps->error.empty()
                    ? context.ramps->error
                    : std::string("no ramps are loaded"));
      return false;
    }
    const auto found = context.ramps->byName.find(rampName);
    if (found == context.ramps->byName.end()) {
      std::string known;
      for (const auto &entry : context.ramps->byName) {
        if (!known.empty()) known += ", ";
        known += entry.first;
      }
      *error = "there is no ramp called '" + rampName + "'; " +
               (known.empty() ? std::string("none are defined") : "known: " + known);
      return false;
    }
    const Ramp &ramp = found->second;
    if (index == "base") {
      if (!ramp.hasBase) {
        *error = "ramp '" + rampName + "' has no base entry";
        return false;
      }
      *out = ramp.base;
    } else {
      const bool numeric = !index.empty() &&
                           std::all_of(index.begin(), index.end(),
                                       [](char c) { return IsDigit(c); });
      if (!numeric) {
        *error = "'" + raw +
                 "' is not a ramp entry: use " + rampName + ".0, " + rampName +
                 ".1, ..., or " + rampName + ".base";
        return false;
      }
      const int step = std::atoi(index.c_str());
      if (step < 0 || step >= static_cast<int>(ramp.steps.size())) {
        *error = "ramp '" + rampName + "' has " +
                 std::to_string(ramp.steps.size()) + " steps, so " + raw +
                 " is out of range";
        return false;
      }
      *out = ramp.steps[static_cast<size_t>(step)];
    }
    if (context.provenance != nullptr) (*context.provenance)[*out] = true;
    return true;
  }
  *error = "unknown colour '" + raw +
           "': name a palette entry, a ramp entry (wood.2, wood.base), or a #hex "
           "literal";
  return false;
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

// splitmix64: the scatter RNG. Deliberately not <random>, because std::mt19937's
// output is implementation-defined enough that "same seed, same sprite" would not
// hold across compilers -- and reproducibility is the whole point of seeding.
struct Rng {
  std::uint64_t state = 0;
  explicit Rng(std::uint64_t seed) : state(seed + 0x9E3779B97F4A7C15ULL) {}
  std::uint64_t Next() {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  int Range(int low, int high) {  // inclusive
    const int span = high - low + 1;
    if (span <= 1) return low;
    return low + static_cast<int>(Next() % static_cast<std::uint64_t>(span));
  }
  double Unit() {
    return static_cast<double>(Next() >> 11) / 9007199254740992.0;  // 2^53
  }
};

class Canvas {
 public:
  Canvas(int width, int height, bool wrap)
      : width_(width), height_(height), wrap_(wrap),
        pixels_(static_cast<size_t>(width) * height * 4, 0) {}

  int width() const { return width_; }
  int height() const { return height_; }
  size_t clipped() const { return clipped_; }
  void ResetClipped() { clipped_ = 0; }
  const std::vector<unsigned char> &pixels() const { return pixels_; }

  Rgba At(int x, int y) const {
    const size_t index = Index(x, y);
    Rgba colour;
    colour.r = pixels_[index + 0];
    colour.g = pixels_[index + 1];
    colour.b = pixels_[index + 2];
    colour.a = pixels_[index + 3];
    return colour;
  }

  bool InBounds(int x, int y) const {
    return x >= 0 && y >= 0 && x < width_ && y < height_;
  }
  bool Wrap() const { return wrap_; }

  void Plot(int x, int y, const Rgba &colour) {
    if (wrap_) {
      x = Mod(x, width_);
      y = Mod(y, height_);
    } else if (!InBounds(x, y)) {
      ++clipped_;
      return;
    }
    const size_t index = Index(x, y);
    pixels_[index + 0] = colour.r;
    pixels_[index + 1] = colour.g;
    pixels_[index + 2] = colour.b;
    pixels_[index + 3] = colour.a;
  }

  void Fill(const Rgba &colour) {
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) Plot(x, y, colour);
    }
  }

  void Rect(int x1, int y1, int x2, int y2, const Rgba &colour) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    if (wrap_) {
      // One period is enough: drawing the same span twice changes nothing.
      if (x2 - x1 + 1 > width_) x2 = x1 + width_ - 1;
      if (y2 - y1 + 1 > height_) y2 = y1 + height_ - 1;
    }
    for (int y = y1; y <= y2; ++y) {
      for (int x = x1; x <= x2; ++x) Plot(x, y, colour);
    }
  }

  void Line(int x1, int y1, int x2, int y2, const Rgba &colour) {
    int dx = std::abs(x2 - x1);
    int dy = -std::abs(y2 - y1);
    const int sx = x1 < x2 ? 1 : -1;
    const int sy = y1 < y2 ? 1 : -1;
    int error = dx + dy;
    while (true) {
      Plot(x1, y1, colour);
      if (x1 == x2 && y1 == y2) break;
      const int doubled = 2 * error;
      if (doubled >= dy) {
        error += dy;
        x1 += sx;
      }
      if (doubled <= dx) {
        error += dx;
        y1 += sy;
      }
    }
  }

  void Circle(int cx, int cy, int radius, const Rgba &colour) {
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        if (dx * dx + dy * dy <= radius * radius) Plot(cx + dx, cy + dy, colour);
      }
    }
  }

  // Scanline, even-odd, vertices in any order. Each plotted pixel goes through
  // Plot, which is what makes a wrapped polygon land on both sides of the seam
  // without a second copy of the geometry.
  void Polygon(const std::vector<Point> &points, const Rgba &colour) {
    if (points.size() < 3) return;
    int minY = points[0].y;
    int maxY = points[0].y;
    for (const Point &point : points) {
      minY = std::min(minY, point.y);
      maxY = std::max(maxY, point.y);
    }
    if (wrap_ && maxY - minY + 1 > height_) minY = maxY - height_ + 1;
    std::vector<double> intersections;
    for (int y = minY; y <= maxY; ++y) {
      intersections.clear();
      for (size_t index = 0; index < points.size(); ++index) {
        const Point &a = points[index];
        const Point &b = points[(index + 1) % points.size()];
        if (a.y == b.y) continue;
        const int lowY = std::min(a.y, b.y);
        const int highY = std::max(a.y, b.y);
        if (y < lowY || y >= highY) continue;
        const double t = static_cast<double>(y - a.y) / static_cast<double>(b.y - a.y);
        intersections.push_back(a.x + t * (b.x - a.x));
      }
      std::sort(intersections.begin(), intersections.end());
      for (size_t index = 0; index + 1 < intersections.size(); index += 2) {
        int start = static_cast<int>(std::ceil(intersections[index]));
        int end = static_cast<int>(std::floor(intersections[index + 1]));
        if (wrap_ && end - start + 1 > width_) end = start + width_ - 1;
        for (int x = start; x <= end; ++x) Plot(x, y, colour);
      }
    }
  }

  // Flood fill from a seed, 4-connected. Wrap makes the fill cross the seam,
  // which is what a tile needs and what an object never asks for.
  bool FloodFill(int seedX, int seedY, const Rgba &colour) {
    if (!InBounds(seedX, seedY)) {
      if (!wrap_) return false;
      seedX = Mod(seedX, width_);
      seedY = Mod(seedY, height_);
    }
    const Rgba target = At(seedX, seedY);
    if (target == colour) return true;
    std::vector<Point> stack;
    stack.push_back({seedX, seedY});
    const size_t cap = static_cast<size_t>(width_) * height_ * 4;
    size_t visits = 0;
    while (!stack.empty()) {
      const Point point = stack.back();
      stack.pop_back();
      if (!InBounds(point.x, point.y)) continue;
      if (!(At(point.x, point.y) == target)) continue;
      Plot(point.x, point.y, colour);
      if (++visits > cap) return false;
      const Point neighbours[4] = {{point.x + 1, point.y},
                                   {point.x - 1, point.y},
                                   {point.x, point.y + 1},
                                   {point.x, point.y - 1}};
      for (const Point &neighbour : neighbours) {
        int nx = neighbour.x;
        int ny = neighbour.y;
        if (wrap_) {
          nx = Mod(nx, width_);
          ny = Mod(ny, height_);
        } else if (!InBounds(nx, ny)) {
          continue;
        }
        if (At(nx, ny) == target) stack.push_back({nx, ny});
      }
    }
    return true;
  }

  size_t OpaqueCount() const {
    size_t count = 0;
    for (size_t index = 3; index < pixels_.size(); index += 4) {
      if (pixels_[index] > 0) ++count;
    }
    return count;
  }

  void ColourBounds(int *minX, int *minY, int *maxX, int *maxY) const {
    *minX = width_;
    *minY = height_;
    *maxX = -1;
    *maxY = -1;
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        if (At(x, y).a == 0) continue;
        *minX = std::min(*minX, x);
        *minY = std::min(*minY, y);
        *maxX = std::max(*maxX, x);
        *maxY = std::max(*maxY, y);
      }
    }
  }

  void Scatter(const std::vector<Rgba> &colours, const Op &op) {
    const size_t pool = op.inside ? OpaqueCount()
                                  : static_cast<size_t>(width_) * height_;
    if (pool == 0) return;
    const size_t target = static_cast<size_t>(
        std::llround(std::min(1.0, std::max(0.0, op.density)) * pool));
    Rng rng(static_cast<std::uint64_t>(op.seed));
    size_t written = 0;
    // Bounded: a fully opaque canvas asked for 100% still terminates, and a
    // density that cannot be met (all `inside` pixels already the same colour
    // doesn't matter -- writes always count) cannot spin forever.
    const size_t guardLimit = target * 40 + 4096;
    size_t guard = 0;
    auto inPool = [&](int x, int y) {
      if (!InBounds(x, y)) return false;
      return !op.inside || At(x, y).a > 0;
    };
    while (written < target && guard++ < guardLimit) {
      const int x = rng.Range(0, width_ - 1);
      const int y = rng.Range(0, height_ - 1);
      if (!inPool(x, y)) continue;
      const Rgba colour = colours[static_cast<size_t>(rng.Range(
          0, static_cast<int>(colours.size()) - 1))];
      std::vector<Point> blob = {{x, y}};
      // Blob growth, not independent pixels: the pack's grain comes in 1-3 pixel
      // clusters, and i.i.d. noise reads as television static at this size.
      const int cluster = std::max(1, op.cluster);
      for (int step = 1; step < cluster; ++step) {
        if (rng.Unit() > 0.55) continue;
        const Point &from = blob[static_cast<size_t>(rng.Range(0, static_cast<int>(blob.size()) - 1))];
        static const int dx[4] = {1, -1, 0, 0};
        static const int dy[4] = {0, 0, 1, -1};
        const int direction = rng.Range(0, 3);
        int nx = from.x + dx[direction];
        int ny = from.y + dy[direction];
        if (wrap_) {
          nx = Mod(nx, width_);
          ny = Mod(ny, height_);
        }
        if (!inPool(nx, ny)) continue;
        blob.push_back({nx, ny});
      }
      for (const Point &point : blob) {
        if (written >= target) break;
        Plot(point.x, point.y, colour);
        ++written;
      }
    }
  }

 private:
  static int Mod(int value, int modulus) {
    const int remainder = value % modulus;
    return remainder < 0 ? remainder + modulus : remainder;
  }
  size_t Index(int x, int y) const {
    return (static_cast<size_t>(y) * width_ + x) * 4;
  }

  int width_;
  int height_;
  bool wrap_;
  std::vector<unsigned char> pixels_;
  size_t clipped_ = 0;
};

// ---------------------------------------------------------------------------
// Rendering a frame + its structural checks
// ---------------------------------------------------------------------------

struct RenderContext {
  Canvas *canvas = nullptr;
  const ColorContext *colours = nullptr;
};

// A coordinate is allowed well outside the canvas (clipping and wrapping both
// need that) but not arbitrarily outside it: an unbounded line or rect is a
// billion-iteration frame stall, and a document is untrusted input whether a
// model or a person wrote it.
bool ValidateBounds(const std::vector<Op> &ops, int width, int height,
                    std::vector<Diagnostic> *diags) {
  const int limit = std::max(width, height) * 8;
  for (const Op &op : ops) {
    for (const Point &point : op.points) {
      if (std::abs(point.x) > limit || std::abs(point.y) > limit) {
        diags->push_back({Severity::Error, op.line, op.column,
                          "coordinate (" + std::to_string(point.x) + "," +
                              std::to_string(point.y) + ") is more than " +
                              std::to_string(limit) +
                              " pixels from the origin; that is a typo, not a shape"});
        return false;
      }
    }
    if (op.radius > 64) {
      diags->push_back({Severity::Error, op.line, op.column,
                        "circle radius is 0 to 64 pixels"});
      return false;
    }
    if (op.points.size() > 64) {
      diags->push_back({Severity::Error, op.line, op.column,
                        "a polygon has at most 64 vertices"});
      return false;
    }
  }
  return true;
}

bool RenderOps(const std::vector<Op> &ops, RenderContext &context,
               std::vector<Diagnostic> *diags) {
  for (const Op &op : ops) {
    context.canvas->ResetClipped();
    if (op.kind == Op::Kind::Scatter) {
      std::vector<Rgba> colours;
      for (const std::string &raw : op.colors) {
        Rgba colour;
        std::string error;
        if (!ResolveColor(raw, *context.colours, &colour, &error)) {
          diags->push_back({Severity::Error, op.line, op.column, error});
          return false;
        }
        colours.push_back(colour);
      }
      context.canvas->Scatter(colours, op);
    } else if (op.kind == Op::Kind::Fill) {
      Rgba colour;
      std::string error;
      if (!ResolveColor(op.color, *context.colours, &colour, &error)) {
        diags->push_back({Severity::Error, op.line, op.column, error});
        return false;
      }
      context.canvas->Fill(colour);
    } else {
      Rgba colour;
      if (!op.erase) {
        std::string error;
        if (!ResolveColor(op.color, *context.colours, &colour, &error)) {
          diags->push_back({Severity::Error, op.line, op.column, error});
          return false;
        }
      } else {
        colour = Rgba{0, 0, 0, 0};
      }
      switch (op.kind) {
        case Op::Kind::Pixel:
          context.canvas->Plot(op.points[0].x, op.points[0].y, colour);
          break;
        case Op::Kind::Rect:
          context.canvas->Rect(op.points[0].x, op.points[0].y, op.points[1].x,
                               op.points[1].y, colour);
          break;
        case Op::Kind::Line:
          context.canvas->Line(op.points[0].x, op.points[0].y, op.points[1].x,
                               op.points[1].y, colour);
          break;
        case Op::Kind::Circle:
          context.canvas->Circle(op.points[0].x, op.points[0].y, op.radius, colour);
          break;
        case Op::Kind::Polygon:
          context.canvas->Polygon(op.points, colour);
          break;
        case Op::Kind::Area:
          if (!context.canvas->InBounds(op.points[0].x, op.points[0].y) &&
              !context.canvas->Wrap()) {
            diags->push_back(
                {Severity::Error, op.line, op.column,
                 "fill_area seed (" + std::to_string(op.points[0].x) + "," +
                     std::to_string(op.points[0].y) +
                     ") is outside the canvas; with wrap off there is nothing to fill"});
            return false;
          }
          if (!context.canvas->FloodFill(op.points[0].x, op.points[0].y, colour)) {
            diags->push_back({Severity::Error, op.line, op.column,
                              "fill_area did not terminate; the region is larger "
                              "than the canvas, which should not be possible"});
            return false;
          }
          break;
        case Op::Kind::Fill:
        case Op::Kind::Scatter:
          break;
      }
    }
    if (context.canvas->clipped() > 0) {
      diags->push_back(
          {Severity::Warning, op.line, op.column,
           std::to_string(context.canvas->clipped()) +
               " pixel(s) of this operation fell outside the canvas and were "
               "clipped" +
               (op.erase ? " (erasing nothing)" : "") +
               "; move the shape inward, or put `wrap on` in the header"});
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

size_t Result::ErrorCount() const {
  return static_cast<size_t>(std::count_if(
      diagnostics.begin(), diagnostics.end(), [](const Diagnostic &diagnostic) {
        return diagnostic.severity == Severity::Error;
      }));
}

size_t Result::WarningCount() const {
  return diagnostics.size() - ErrorCount();
}

Result Compile(const std::string &text, const Config &config) {
  Result result;
  std::vector<Diagnostic> diags;

  bool lexOk = true;
  std::vector<Token> tokens = Lex(text, &diags, &lexOk);
  if (!lexOk) {
    result.diagnostics = std::move(diags);
    return result;
  }

  Document document;
  Parser parser(std::move(tokens), &diags);
  if (!parser.Parse(&document)) {
    result.diagnostics = std::move(diags);
    return result;
  }

  result.width = document.width;
  result.height = document.height;
  result.kind = document.kind;
  result.material = document.material;
  result.wrap = document.wrap == -1 ? document.kind == "tile" : document.wrap == 1;

  // Always loaded, and small (a few KB). Deferring it until "the document looks
  // like it needs ramps" means scanning every operation for a dot, and the first
  // version only scanned the palette block -- so a document using `stone.2`
  // directly was told "no ramps are loaded" while the file sat right there.
  Ramps ramps;
  LoadRamps(config.rampsPath, &ramps);

  if (!document.material.empty() && !ramps.loaded) {
    diags.push_back({Severity::Warning, 1, 1,
                     "material '" + document.material +
                         "' could not be checked: " + ramps.error});
  }
  if (!document.material.empty() && ramps.loaded &&
      ramps.byName.find(document.material) == ramps.byName.end()) {
    std::string known;
    for (const auto &entry : ramps.byName) {
      if (!known.empty()) known += ", ";
      known += entry.first;
    }
    result.diagnostics = {{Severity::Error, 1, 1,
                           "there is no material ramp called '" +
                               document.material + "'; the art pack defines: " +
                               known}};
    return result;
  }

  std::map<std::string, Rgba> palette;
  std::map<Rgba, bool> provenance;
  ColorContext colourContext;
  colourContext.ramps = &ramps;
  colourContext.palette = &palette;
  colourContext.provenance = &provenance;

  // Palette entries first, in declaration order. An entry may be a hex literal or
  // a ramp reference, not another palette name: alias chains need cycle detection
  // for no benefit a ramp does not already provide.
  for (const std::string &name : document.paletteNames) {
    const std::string &raw = document.paletteRaw[name];
    if (palette.find(raw) != palette.end()) {
      result.diagnostics = {
          {Severity::Error, 1, 1,
           "palette entry '" + name + "' points at another palette entry ('" + raw +
               "'); entries are ramp references or #hex literals"}};
      return result;
    }
    Rgba colour;
    std::string error;
    if (!ResolveColor(raw, colourContext, &colour, &error)) {
      result.diagnostics = {{Severity::Error, 1, 1,
                             "palette entry '" + name + "': " + error}};
      return result;
    }
    palette[name] = colour;
  }

  std::vector<FrameAst> frames;
  if (document.hasFrames) {
    frames = document.frames;
  } else {
    FrameAst single;
    single.name.clear();
    single.ops = document.ops;
    frames.push_back(std::move(single));
  }

  PaletteReport paletteReport = LoadPaletteReport(config.palettePath);

  // Recorded while rendering, where the bounds are already computed for the
  // margin checks, so the summary does not walk every frame a second time.
  bool haveObjectBounds = false;
  int objectMinX = 0, objectMinY = 0, objectMaxX = 0, objectMaxY = 0;

  for (const FrameAst &frame : frames) {
    if (frame.ops.size() > 4096) {
      diags.push_back({Severity::Error, frame.line, 1,
                       "a frame holds at most 4096 operations; this one has " +
                           std::to_string(frame.ops.size())});
      result.diagnostics = std::move(diags);
      return result;
    }
    if (!ValidateBounds(frame.ops, document.width, document.height, &diags)) {
      result.diagnostics = std::move(diags);
      return result;
    }
    Canvas canvas(document.width, document.height, result.wrap);
    RenderContext context;
    context.canvas = &canvas;
    context.colours = &colourContext;
    if (!RenderOps(frame.ops, context, &diags)) {
      result.diagnostics = std::move(diags);
      return result;
    }

    Frame output;
    output.name = frame.name;
    output.width = document.width;
    output.height = document.height;
    output.rgba = canvas.pixels();

    const size_t total = static_cast<size_t>(document.width) * document.height;
    const size_t opaque = canvas.OpaqueCount();
    const std::string frameLabel =
        frame.name.empty() ? std::string("(default)")
                           : "\"" + frame.name + "\"";

    if (document.kind == "tile") {
      // A tile is ground and the renderer draws it full-bleed: a transparent pixel
      // would show the clear colour through the floor, so this is an error rather
      // than a warning.
      if (opaque != total) {
        int firstX = -1;
        int firstY = -1;
        for (int y = 0; y < document.height && firstX < 0; ++y) {
          for (int x = 0; x < document.width; ++x) {
            if (canvas.At(x, y).a == 0) {
              firstX = x;
              firstY = y;
              break;
            }
          }
        }
        diags.push_back(
            {Severity::Error, frame.line, 1,
             "tile " + frameLabel + " is not full-bleed: " +
                 std::to_string(total - opaque) + " pixel(s) are transparent, the "
                 "first at (" + std::to_string(firstX) + "," + std::to_string(firstY) +
                 "). Start with `fill <colour>` and build up from there"});
        result.diagnostics = std::move(diags);
        return result;
      }
    } else {
      if (opaque == 0) {
        diags.push_back({Severity::Error, frame.line, 1,
                         "object " + frameLabel +
                             " rendered nothing: every pixel is transparent"});
        result.diagnostics = std::move(diags);
        return result;
      }
      int minX = 0, minY = 0, maxX = 0, maxY = 0;
      canvas.ColourBounds(&minX, &minY, &maxX, &maxY);
      if (!haveObjectBounds) {
        haveObjectBounds = true;
        objectMinX = minX;
        objectMinY = minY;
        objectMaxX = maxX;
        objectMaxY = maxY;
      }
      const int width = maxX - minX + 1;
      const int height = maxY - minY + 1;
      if (minX <= 0 || minY <= 0 || maxX >= document.width - 1 ||
          maxY >= document.height - 1) {
        diags.push_back(
            {Severity::Warning, frame.line, 1,
             "object " + frameLabel + " touches the canvas edge; the pack's props "
             "keep a 1px transparent margin so they do not look cropped"});
      }
      if (width < 6 || height < 6) {
        diags.push_back(
            {Severity::Warning, frame.line, 1,
             "object " + frameLabel + " is only " + std::to_string(width) + "x" +
                 std::to_string(height) +
                 " opaque pixels; at 32px that reads as a speck, not a thing"});
      }
    }

    result.frames.push_back(std::move(output));
  }

  // --- summary -------------------------------------------------------------
  const size_t total = static_cast<size_t>(document.width) * document.height;
  std::set<Rgba> colours;
  size_t opaqueSum = 0;
  size_t inPalette = 0;
  for (const Frame &frame : result.frames) {
    for (size_t index = 0; index < frame.rgba.size(); index += 4) {
      if (frame.rgba[index + 3] == 0) continue;
      ++opaqueSum;
      Rgba colour;
      colour.r = frame.rgba[index + 0];
      colour.g = frame.rgba[index + 1];
      colour.b = frame.rgba[index + 2];
      colour.a = frame.rgba[index + 3];
      colours.insert(colour);
      if (paletteReport.loaded && paletteReport.colours.count(PackRgb(colour)) > 0) {
        ++inPalette;
      }
    }
  }
  size_t fromRamps = 0;
  size_t literals = 0;
  for (const Rgba &colour : colours) {
    const auto found = provenance.find(colour);
    if (found != provenance.end() && found->second) {
      ++fromRamps;
    } else {
      ++literals;
    }
  }

  std::ostringstream summary;
  summary << "kind: " << document.kind << ", " << document.width << "x"
          << document.height << ", wrap " << (result.wrap ? "on" : "off");
  if (!document.material.empty()) summary << ", material " << document.material;
  result.summary.push_back(summary.str());
  result.summary.push_back(
      "frames: " + std::to_string(result.frames.size()) +
      (result.frames.size() == 1
           ? (result.frames[0].name.empty() ? std::string(" (unnamed)")
                                            : " (" + result.frames[0].name + ")")
           : std::string()));
  result.summary.push_back("colours: " + std::to_string(colours.size()) + " (" +
                           std::to_string(fromRamps) + " from ramps, " +
                           std::to_string(literals) + " literal)");
  {
    std::ostringstream line;
    line.setf(std::ios::fixed);
    line.precision(1);
    line << "opaque: " << (100.0 * static_cast<double>(opaqueSum) /
                           static_cast<double>(total * result.frames.size()))
         << "% (" << opaqueSum << "/" << total * result.frames.size() << ")";
    result.summary.push_back(line.str());
  }
  if (paletteReport.loaded) {
    std::ostringstream line;
    line.setf(std::ios::fixed);
    line.precision(1);
    line << "palette coverage: "
         << (opaqueSum == 0
                 ? 0.0
                 : 100.0 * static_cast<double>(inPalette) /
                       static_cast<double>(opaqueSum))
         << "% of opaque pixels are in " << config.palettePath
         << " (informational: ramp tones are the pack's own, and " << config.palettePath
         << " is a 128-colour approximation of them)";
    result.summary.push_back(line.str());
  } else if (!config.palettePath.empty() && !paletteReport.error.empty()) {
    result.summary.push_back("palette coverage: not measured -- " +
                             paletteReport.error);
  }
  if (ramps.loaded) {
    result.summary.push_back("ramps: " + std::to_string(ramps.byName.size()) +
                             " loaded (" + config.rampsPath +
                             "; --sprite-grammar lists them)");
  }
  if (document.kind == "object" && haveObjectBounds) {
    result.summary.push_back(
        "silhouette: " + std::to_string(objectMaxX - objectMinX + 1) + "x" +
        std::to_string(objectMaxY - objectMinY + 1) + " at (" +
        std::to_string(objectMinX) + "," + std::to_string(objectMinY) + ")");
  }


  result.diagnostics = std::move(diags);
  result.ok = result.ErrorCount() == 0;
  return result;
}

Result CompileFile(const std::string &path, const Config &config) {
  std::ifstream file(path);
  if (!file.is_open()) {
    Result result;
    result.diagnostics.push_back(
        {Severity::Error, 0, 0, "could not read " + path});
    return result;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  Result result = Compile(buffer.str(), config);
  return result;
}

bool WritePng(const Frame &frame, const std::string &path) {
  if (frame.width <= 0 || frame.height <= 0 || frame.rgba.empty()) return false;
  Image image = {};
  // raylib's ExportImage does not modify the data, but the struct's field is
  // non-const; the const_cast is confined to this call and the buffer outlives it.
  image.data = const_cast<unsigned char *>(frame.rgba.data());
  image.width = frame.width;
  image.height = frame.height;
  image.mipmaps = 1;
  image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
  // CPU-side writer: no window, no GL. That is what lets --sprite-compile run
  // before InitWindow, next to --tile-design and --gaps.
  return ExportImage(image, path.c_str());
}

std::string Describe(const Result &result) {
  std::ostringstream out;
  out << (result.ok ? "OK" : "REFUSED");
  if (result.width > 0) {
    out << " -- " << result.width << "x" << result.height << " " << result.kind;
    if (!result.material.empty()) out << ", material " << result.material;
    if (result.frames.size() > 1) {
      out << ", " << result.frames.size() << " frames";
    }
  }
  out << "\n";
  for (const Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.severity != Severity::Error) continue;
    out << "  error:   ";
    if (diagnostic.line > 0) out << "line " << diagnostic.line << ": ";
    out << diagnostic.message << "\n";
  }
  for (const Diagnostic &diagnostic : result.diagnostics) {
    if (diagnostic.severity == Severity::Error) continue;
    out << "  warning: ";
    if (diagnostic.line > 0) out << "line " << diagnostic.line << ": ";
    out << diagnostic.message << "\n";
  }
  for (const std::string &line : result.summary) {
    out << "  " << line << "\n";
  }
  return out.str();
}

std::string GrammarReference() {
  return R"(# The sprite DSL -- language reference (compact)

A .sprite document is a header, an optional palette, and either a list of
operations or a list of named frames. It compiles to a 32x32 RGBA sprite.

## Header (order does not matter, each at most once)
  version 1                 // optional; omit for version 1
  sprite 32x32              // required; square, 8..128
  kind tile                 // tile (default) or object
  material stone            // optional: picks the art pack's ramp family
  wrap on                   // default: on for tile, off for object
  palette { <name> = <colour expr> ... }   // optional

## Colour expressions
  wood.2            a ramp step: ramp name, dot, step index (0 = darkest)
  wood.base         the ramp's dominant tone
  #181718           literal #rrggbb or #rrggbbaa
  ink               a palette name, when the palette block defines it
There is no way to invent a colour: every value is a ramp entry the art pack
itself uses, or a literal you wrote on purpose.

## Operations (in order; later ops draw over earlier ones)
  fill <colour>
  pixel (x,y) color=<colour>
  rect (x1,y1) (x2,y2) color=<colour>
  line (x1,y1) (x2,y2) color=<colour>
  circle (cx,cy) radius=<r> color=<colour>
  polygon (x,y) (x,y) (x,y) ... color=<colour>      // 3+ vertices, any order
  fill_area (x,y) color=<colour>                     // flood fill from a seed
  scatter color=<c>[,<c>...] density=<0..1> seed=<n> [cluster=<1..16>] [inside]
  erase pixel|rect|line|circle|polygon|fill_area ...  // sets pixels transparent

  scatter: adds clustered grain. `density` is a fraction of the canvas (or of
  the opaque pixels with `inside`), `seed` makes it reproducible, `cluster` is
  the maximum blob size. This is the op that makes a surface read as a material
  instead of a flat colour, and the one the reference DSL does not have.

## Frames (variants: growth stages, open/closed, full/empty)
  frame "mature" { <operations> }
  frame "sprout" { <operations> }
One document produces one sprite per frame. A document is either operations or
frames, never both.

## Rules the compiler enforces
  - a tile is full-bleed: every pixel opaque, or refused
  - an object must render something, and should keep a 1px transparent margin
  - coordinates outside the canvas warn (and are clipped) unless wrap is on
  - every colour must resolve; every operation must be well-formed

## Typical structure
  sprite 32x32
  kind tile
  material stone
  fill stone.2
  fill_area (5,4) color=stone.3
  line (0,12) (31,12) color=stone.0
  scatter color=stone.1,stone.4 density=0.2 seed=7 cluster=2
)";
}

std::string RampSummary(const Config &config, std::string *error) {
  Ramps ramps;
  if (!LoadRamps(config.rampsPath, &ramps)) {
    if (error != nullptr) *error = ramps.error;
    return {};
  }
  std::ostringstream out;
  for (const auto &entry : ramps.byName) {
    out << entry.first << " (" << entry.second.steps.size()
        << (entry.second.steps.size() == 1 ? " step" : " steps");
    if (entry.second.hasBase) {
      char hex[16];
      std::snprintf(hex, sizeof(hex), ", base #%02x%02x%02x",
                    entry.second.base.r, entry.second.base.g, entry.second.base.b);
      out << hex;
    }
    out << ")\n";
  }
  return out.str();
}

}  // namespace PixelDSL
