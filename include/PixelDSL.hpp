#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The sprite DSL: a small declarative document that renders to a 32x32 RGBA
// sprite, deterministically, with no image model and no network.
//
// Why this exists, in one paragraph. The image pipeline produces a picture and
// then spends six stages correcting it (cut, inset, gutter wash, magenta key,
// trim, palette lock). A 32x32 ground tile or prop does not need a picture; it
// needs a *program*, and a program can be palette-correct by construction,
// rendered in a millisecond, diffed in review, and re-rendered for free when the
// art pack changes. The language is deliberately pixlmao-shaped
// (https://github.com/metdxt/pixlmao, MIT) because that project already proved
// the shape works for LLM authoring: a few declarative ops, no control flow, a
// `check` that refuses before rendering, and a grammar reference that can be
// pasted into a prompt. What it does not have is the three ops this project's
// art style actually needs -- seeded clustered scatter, canvas wrap, and ramps
// derived from the pack -- so those are added here.
//
// **This does not replace image generation.** It is a second source behind the
// same seam: `TileArtQueue` still runs whatever command `art.generation.command`
// names, and a document that fails to compile falls back to the current path.
// See docs/pixel-dsl.md.
//
// Two rules the compiler enforces rather than trusts:
//
//   1. A colour is either a ramp entry (a tone the art pack itself uses) or an
//      explicit literal, and every colour in a document comes from one of those.
//      There is no way for a document to invent a value the pack does not use,
//      which is why DSL art needs no palette-lock stage at all.
//   2. A result is validated structurally before it is returned: a tile is
//      full-bleed and fully opaque; an object has a silhouette and transparent
//      margins. Art has no test suite otherwise -- this is the part that is
//      checkable without looking at it, and it is the part the agent's retry
//      loop consumes.
namespace PixelDSL {

// What a compile needs from disk. Both paths are optional in the sense that a
// document using only literals compiles with no ramps at all; a document naming
// `wood.base` fails with a message saying which file was missing, which is the
// only useful thing to be told.
struct Config {
  // Material ramps, derived from the art pack by tools/derive_ramps.py.
  std::string rampsPath = "data/style/ramps.json";
  // The pack palette, used only to *report* how much of a result is inside it.
  // Empty skips the report. It is not a lock: ramps already hold the pack's own
  // tones, and data/palette.json is a 128-colour approximation of them.
  std::string palettePath = "data/palette.json";
};

enum class Severity {
  Error,
  Warning,
};

// One thing to tell the author. Errors refuse the compile; warnings are kept
// even on success, because "the tile is fine but its outline is off the canvas"
// is exactly the kind of thing a retry should see.
struct Diagnostic {
  Severity severity = Severity::Error;
  int line = 0;
  int column = 0;
  std::string message;
};

// One rendered image. `rgba` is row-major, 4 bytes per pixel, alpha 0 meaning
// transparent. A single-sprite document produces exactly one frame; a document
// with `frame` blocks produces one per block, in source order.
struct Frame {
  std::string name;
  int width = 0;
  int height = 0;
  std::vector<unsigned char> rgba;
};

struct Result {
  // True when no error-severity diagnostic was produced. A result with
  // warnings is still ok.
  bool ok = false;
  std::vector<Diagnostic> diagnostics;
  std::vector<Frame> frames;

  int width = 0;
  int height = 0;
  std::string kind;      // "tile" or "object", as the document declared it
  std::string material;  // empty when the document named none
  bool wrap = false;

  // Human-readable facts about the result, one per line: size, distinct
  // colours, opaque share, palette coverage, ramp/literal split, silhouette box.
  // These are what the window and the log show, and what a designer agent reads
  // back instead of looking at the image.
  std::vector<std::string> summary;

  size_t ErrorCount() const;
  size_t WarningCount() const;
};

// Compiles `text` in memory. Never throws; every failure is a diagnostic.
Result Compile(const std::string &text, const Config &config);

// Reads `path` and compiles it. An unreadable file is one error diagnostic.
Result CompileFile(const std::string &path, const Config &config);

// Writes one frame as a PNG. Uses raylib's CPU-side image writer, so this works
// with no window and no GL context -- which is what makes the whole feature
// verifiable headlessly, the same reason `--tile-design` and `--gaps` exist.
bool WritePng(const Frame &frame, const std::string &path);

// The diagnostics and the summary as one printable block. Used by the CLI and by
// the window's transcript, so the two cannot word the same verdict differently.
std::string Describe(const Result &result);

// The compact language reference, for a designer's system prompt. Kept here
// rather than in a data file so the ops it documents and the ops the parser
// accepts cannot drift apart silently: a new op is added in this file or it is
// not documented. (`%SPRITE_DSL%` is the prompt substitution it goes into.)
std::string GrammarReference();

// The ramp names and their step counts, for a prompt or a report. Empty when the
// ramps file could not be read, with the reason in `error`.
std::string RampSummary(const Config &config, std::string *error = nullptr);

}  // namespace PixelDSL
