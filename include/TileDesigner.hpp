#pragma once

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "Components.h"
#include "SpriteKinds.hpp"
#include "TileRegistry.h"

// The overlay loader reports through the game's log when there is one. Declared
// rather than included, the same way TileRegistry.h does it.
class DebugLog;

// The tile designer: the half of the feature that turns a model's reply into a
// tile the engine can actually resolve, with no window and no ImGui in the way.
//
// Why it is separate from the window. This repo cannot run the game headless and
// has no test framework, so anything that only exists inside an ImGui Draw() is
// unreachable to verification -- which is why GapLog has a `--gaps` entry point
// (src/main.cpp). A tile designer built the same way would repeat that mistake:
// the window is the least testable part and the validation is the part that
// matters. So the grammar, the extraction and the verdict live here, and both
// `AIRogue --tile-design <file>` and TileDesignerWindow call this one
// implementation. There is exactly one validator.
//
// What this deliberately does NOT do: generate art. Per the owner's decision
// (see .agents/tile-designer-handover.md) a created tile is a glyph and two
// colours for now, and a sprite is hot-swapped in later. The `sprite` field is
// honoured if a reply carries one -- it is part of the tile schema -- but no
// reply is asked for one and nothing here produces an image.
namespace TileDesigner {

// The prose-then-one-fenced-JSON reply contract, the same shape the blueprint
// designer uses (src/DesignerChatWindow.cpp): prose first, then exactly one
// ```json block. Duplicated rather than shared because DesignerChatWindow keeps
// its copies private and a shared "designer reply" utility is a bigger seam than
// two callers justify -- but if a third appears, these two moves to a common
// header.
//
// Pulls the first fenced ```json block out of `text`. Empty when there is none.
std::string ExtractJsonBlock(const std::string &text);

// The tile text itself: the fenced block when there is one, otherwise the whole
// string. `fromBlock` reports which it was.
//
// The fallback exists because the contract and the *file* are different things. A
// model reply is prose plus a fenced block, but what a person has on disk to test
// with -- and what an overlay file contains -- is usually a bare object. Insisting
// on the fence there would make `--tile-design` unable to read the very files the
// tile designer writes, and would turn a perfectly good tile into "no ```json
// block", which is a confusing thing to be told about a file of JSON.
std::string ExtractTileJson(const std::string &text, bool *fromBlock = nullptr);

// Strips "<think>...</think>" reasoning from a complete reply. A pure function
// over the whole text rather than a stateful filter, so the raw reply can still
// be logged and shown with reasoning intact.
std::string StripReasoning(const std::string &text);

// The conversation the model sees: the prompt file with its vocabulary substituted.
// Empty `promptFile` (or an unreadable one) yields the generated fallback rather than
// an empty prompt, so a missing file degrades the designer instead of silencing it.
//
// Kept as a wrapper over BuildPromptForKind for the tile kind, because that is what
// the tile window and the existing tests call. New callers should prefer
// BuildPromptForKind, which does not assume a kind.
std::string BuildSystemPrompt(const std::string &promptFile, const TileRegistry &tiles);

// The vocabulary a prompt is built against, as already-formatted text. Passed in
// rather than derived here because the two kinds derive it from different places -- a
// tile from the tile catalog's glyphs and names, an object from the factory's
// registered components -- and a prompt builder that knew about both would have to
// know about every future kind too.
struct PromptVocabulary {
  // The vocabulary body, one entry per line. Goes into `%VOCAB%` when the file has
  // one, and is appended under the kind's heading when it does not, so a file that
  // forgets the placeholder still carries the vocabulary rather than silently losing
  // it. Empty falls back to the kind's `emptyVocabularyNote`.
  std::string body;

  // The glyph legend with collisions named, for the tile kind only. Empty for a kind
  // that has no glyphs; such a file simply has no `%GLYPHS%` placeholder and nothing
  // is appended for it.
  std::string glyphs;
};

// The tile catalog as the prompt's vocabulary list: one line per tile, marked when it
// blocks movement, because that is the one mechanic a tile carries and the one a
// designer has to reason about. Exposed because the builder no longer derives the
// vocabulary itself -- the caller assembles it per kind.
std::string TileVocabulary(const TileRegistry &tiles);

// The system prompt for an arbitrary kind.
//
// `contract` is the shared reply contract -- the format, the refusal protocol, the
// rule that the fenced block is JSON and nothing else. It is read from one file and
// shared by every kind rather than copied into each kind's document, because that is
// the part that must not drift: a tile and an object are both refused the same way.
//
// `%CONTRACT%`, `%GLYPHS%` and `%VOCAB%` are replaced when present.
std::string BuildPromptForKind(const SpriteKinds::Descriptor &kind,
                               const std::string &promptFile,
                               const std::string &contractFile,
                               const PromptVocabulary &vocabulary);

// The glyph table, one line per glyph, as it goes into the prompt. Exposed for
// the tile designer window's legend pane so the two cannot disagree about what
// the model was told.
//
// Collisions are named rather than hidden: this is the parked-bug honesty the
// handover asks for. The catalog already shares '#', '.', '\'' and others, so a
// legend that quietly listed one name per glyph would be lying about the world
// the agent perceives (see GetSurroundingsRadar's legend[glyph] = name).
std::vector<std::string> GlyphLegend(const TileRegistry &tiles);

// What one reply produced. `ok` is the verdict; everything else is why, or what
// to say about a tile that was accepted anyway.
struct ApplyResult {
  bool ok = false;
  // Blocking reason. Empty when ok. Worded to be fed straight back to the model
  // on the next attempt.
  std::string error;
  // Non-blocking observations: a glyph already in use, a "description" that
  // means nothing. Kept even on success.
  std::vector<std::string> warnings;
  // The accepted definition, for the preview pane and for logging. Null on
  // failure. Points into the registry, so it is stable for the run.
  const TileDef *definition = nullptr;
  // The name of the accepted tile, copied out because a preview that outlives a
  // RemoveRuntimeTiles reload must still be able to say what it was showing.
  std::string name;
  // Where the definition was written, when `overlayDirectory` was given. Empty
  // when the tile was only accepted in memory or when writing failed.
  std::string savedPath;
  // True when the write failed but the tile was accepted. The caller decides
  // whether that is a refusal; this function reports it rather than hiding it.
  bool saveFailed = false;
  // The model's own critique, pulled out of the reply and stripped before the
  // definition was written. Empty for ApplyJson, which has no reply to read it
  // from. Kept beside the verdict rather than pushed into `warnings` because the
  // window shows it as its own thing, exactly as the blueprint designer does.
  std::string notes;
  // True when the name already existed before this call. Not an error on its own
  // -- the name check inside AddRuntime is what refuses it -- but the window uses
  // it to warn before a model spends an attempt on an unusable name.
  bool nameTaken = false;
};

// Parses `reply`, validates the tile, adds it to `tiles`, and optionally writes
// it under `overlayDirectory` (<name-slug>.json). This is the whole creation
// path from §4.6 minus the art step, and the only place it exists.
//
// `tiles` is the live catalog and is modified on success. `overlayDirectory` may
// be empty, in which case the tile exists for this run only -- which is what
// `--tile-design` wants when it is being used to test the validator.
//
// Not thread safe, and not meant to be: it is called on the main thread, after a
// stream finishes, exactly like DesignerChatWindow::InspectFinishedExchange.
ApplyResult ApplyReply(const std::string &reply, TileRegistry &tiles,
                       const std::string &overlayDirectory);

// Reads `path` and applies its contents, accepting either form ExtractTileJson
// accepts. This is what `--tile-design` calls, so the mode cannot drift from the
// window about what a reply is.
ApplyResult ApplyFile(const std::string &path, TileRegistry &tiles,
                      const std::string &overlayDirectory);

// The same thing, for a tile that is already parsed JSON. Used by the overlay
// loader and by anything that reads a definition from a file rather than from a
// model reply.
ApplyResult ApplyJson(const nlohmann::json &tileJson, TileRegistry &tiles,
                      const std::string &overlayDirectory);

// True when the name is already in the catalog, hand-authored or runtime. The
// cheap pre-check a window runs before sending a request, so an agent is not
// asked for a name it cannot have.
bool NameTaken(const TileRegistry &tiles, const std::string &name);

// The definition as it is written to the overlay: the same fields the catalog
// uses, in a stable order, with a trailing newline. Field names must stay in sync
// with TileRegistry's parser; it is the one thing both ends have to agree on.
nlohmann::json WriteTileDef(const TileDef &def);

// The filename a name is written under: lowercase, spaces and punctuation to
// underscores. Only used for the overlay, where the file stem carries no meaning
// -- the definition's `name` is the identity (see TileRegistry.h).
std::string SlugifyFileName(const std::string &name);

// Loads every *.json in `overlayDirectory` through the registry's runtime-add
// path. Returns how many definitions were added; malformed files are reported
// through `log` (or stderr) and skipped, exactly like a catalog load failure. A
// missing directory is not an error -- most runs have no created tiles.
size_t LoadOverlayDirectory(const std::string &overlayDirectory,
                            TileRegistry &tiles, DebugLog *log);

}  // namespace TileDesigner
