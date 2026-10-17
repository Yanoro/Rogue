#include "TileDesigner.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "DebugLog.h"
#include "SpriteReply.hpp"

namespace TileDesigner {

namespace {

// The language tag that opens the one fenced block a reply must carry. The same
// string DesignerChatWindow matches on; the two windows are answering different
// questions but the contract is deliberately identical, so a model that learned
// one has learned the other.
constexpr const char *kJsonFence = "```json";

constexpr const char *kGlyphPlaceholder = "%GLYPHS%";
// The vocabulary placeholder. It used to be `%TILES%`, which said what the list was
// rather than what the slot is for: an object's list is components, not tiles, and a
// placeholder named after one kind cannot be shared by the next. The old spelling is
// still honoured below so a prompt file written against it keeps working.
constexpr const char *kVocabularyPlaceholder = "%VOCAB%";
constexpr const char *kLegacyTilePlaceholder = "%TILES%";
// The shared reply contract, substituted from `data/prompts/SPRITE_CONTRACT.md`.
constexpr const char *kContractPlaceholder = "%CONTRACT%";

// The contract document, as the in-code default. Every kind's document is written
// against the same contract, because the reply format and the refusal protocol are
// identical for a tile and an object -- that is exactly the part that must not drift
// between two documents. The setting that overrides it is
// `content.spriteContractFile`.
constexpr const char *kDefaultContractFile = "data/prompts/SPRITE_CONTRACT.md";

// The comment fences that mark a shape section inside a kind document. HTML comments so that
// a kind document read on its own, in an editor or on a forge, shows no scaffolding; the
// builder strips the fence that marks the section it did not select.
constexpr const char *kSingleOpen = "<!--%%SINGLE%%-->";
constexpr const char *kSingleClose = "<!--%%/SINGLE%%-->";
constexpr const char *kSetOpen = "<!--%%SET%%-->";
constexpr const char *kSetClose = "<!--%%/SET%%-->";

// Removes every block delimited by `open`/`close`, fences included. A block with no closing
// fence is left alone rather than eating the rest of the document: a broken fence should
// produce a slightly-too-long prompt, not an empty one.
void RemoveSection(std::string &text, const char *open, const char *close) {
  for (;;) {
    const size_t start = text.find(open);
    if (start == std::string::npos) return;
    const size_t end = text.find(close, start);
    if (end == std::string::npos) return;
    text.erase(start, end - start + std::string(close).size());
  }
}

// What a kind's document degrades to when the file is missing or unreadable.
//
// A *document* rather than a rewrite of the builder: substitution is replace-only, so
// the way to guarantee the vocabulary survives a missing file is to supply a document
// that carries the placeholders. This also keeps the failure legible to the model --
// "no kind-specific guidance is available" is a fact it can act on, whereas a prompt
// with a vocabulary section bolted onto the end of nothing is not.
constexpr const char *kFallbackPrompt =
    "You are a sprite designer. Your instruction document could not be read, so no "
    "kind-specific guidance is available for this request; follow the shared contract "
    "below exactly, use only vocabulary from the list at the end, and be conservative.\n"
    "\n"
    "%CONTRACT%\n"
    "\n"
    "## Vocabulary available\n"
    "\n"
    "%VOCAB%\n";

}  // namespace

// One line per tile, marked when it blocks movement, because that is the one mechanic
// a tile carries and the one a designer has to reason about. The empty case is stated
// rather than left blank: "no catalog is loaded" is information, and a heading with
// nothing under it reads as an omission.
//
// Public, so it sits outside the anonymous namespace the prompt helpers live in:
// declared in the header and defined here, because the builder no longer derives the
// vocabulary itself -- the caller assembles it per kind.
std::string TileVocabulary(const TileRegistry &tiles) {
  if (tiles.Count() == 0) {
    return "  (none: no tile catalog is loaded)\n";
  }
  std::string out;
  for (const TileDef *tile : tiles.GetAll()) {
    out += "  - " + tile->name;
    if (tile->blocksTile) {
      out += "  [blocks movement]";
    }
    out += "\n";
  }
  return out;
}

namespace {

// Replaces `placeholder` with `body` when it is present. Nothing else.
//
// There is deliberately no append-when-absent behaviour, and it used to have one. That
// behaviour assumed a document lists its vocabulary only where a placeholder sits, so a
// document carrying its own heading *and* the placeholder got the vocabulary twice: once
// substituted, once appended underneath. It is also the wrong answer in principle -- a
// document with a hand-written list should be left alone, not given a second one -- so
// the fallback for a missing document is to supply a document (see kFallbackPrompt)
// rather than to grow whatever arrived.
void SubstituteOnly(std::string &prompt, const std::string &placeholder,
                    const std::string &body) {
  const size_t at = prompt.find(placeholder);
  if (at != std::string::npos) {
    prompt.replace(at, placeholder.size(), body);
  }
}

// The overlay file is read back by TileRegistry, which keys on the `name` field,
// so the stem is only a filename. Still slugified rather than written verbatim:
// a model may name a tile "Oak Bed (large)" and a filename with parentheses,
// spaces and slashes in it is a bug waiting for a second platform.
std::string SanitizeStem(const std::string &name) {
  std::string out;
  out.reserve(name.size());
  for (const char raw : name) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (std::isalnum(c)) {
      out += static_cast<char>(std::tolower(c));
    } else if (!out.empty() && out.back() != '_') {
      out += '_';
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  if (out.empty()) {
    out = "tile";
  }
  return out;
}

// Writes `def` as <overlayDirectory>/<stem>.json, creating the directory if
// needed. Returns the path on success and an empty string on failure, with the
// reason in `error`.
std::string WriteDefinition(const TileDef &def, const std::string &overlayDirectory,
                            std::string &error) {
  std::error_code ec;
  std::filesystem::create_directories(overlayDirectory, ec);
  if (ec) {
    error = "could not create " + overlayDirectory + ": " + ec.message();
    return {};
  }

  // The write is to a temporary path and then renamed, so a crash or a full disk
  // cannot leave a half-written definition that the next load reports as
  // malformed. The overlay is reloaded on every launch, so a truncated file is
  // not a one-off annoyance, it is a broken tile forever.
  const std::filesystem::path finalPath =
      std::filesystem::path(overlayDirectory) / (SlugifyFileName(def.name) + ".json");
  const std::filesystem::path tempPath = finalPath.string() + ".tmp";

  {
    std::ofstream out(tempPath);
    if (!out.is_open()) {
      error = "could not open " + tempPath.string() + " for writing";
      return {};
    }
    out << WriteTileDef(def).dump(2) << "\n";
    if (!out.good()) {
      error = "writing " + tempPath.string() + " failed";
      std::error_code removeEc;
      std::filesystem::remove(tempPath, removeEc);
      return {};
    }
  }

  std::filesystem::rename(tempPath, finalPath, ec);
  if (ec) {
    error = "could not move " + tempPath.string() + " to " + finalPath.string() +
            ": " + ec.message();
    std::error_code removeEc;
    std::filesystem::remove(tempPath, removeEc);
    return {};
  }
  return finalPath.string();
}

}  // namespace

std::string ExtractJsonBlock(const std::string &text) {
  // Deliberately tolerant about what surrounds the fence and strict about the
  // fence itself, because that is the one thing the prompt is unambiguous about.
  const size_t fence = text.find(kJsonFence);
  if (fence == std::string::npos) {
    return {};
  }
  const size_t bodyStart = text.find('\n', fence);
  if (bodyStart == std::string::npos) {
    return {};
  }
  const size_t close = text.find("```", bodyStart);
  if (close == std::string::npos) {
    // An unterminated fence: take the rest. A model that forgot to close it has
    // usually still produced the object, and "no JSON block" would be a less
    // useful error than whatever the parser says about the text.
    return text.substr(bodyStart + 1);
  }
  return text.substr(bodyStart + 1, close - bodyStart - 1);
}

std::string ExtractTileJson(const std::string &text, bool *fromBlock) {
  const std::string block = ExtractJsonBlock(text);
  if (!block.empty()) {
    if (fromBlock != nullptr) *fromBlock = true;
    return block;
  }
  if (fromBlock != nullptr) *fromBlock = false;
  return text;
}

std::string StripReasoning(const std::string &text) {
  static constexpr const char *kOpen = "<think>";
  static constexpr const char *kClose = "</think>";
  static constexpr size_t kOpenLength = 7;
  static constexpr size_t kCloseLength = 8;

  std::string out;
  out.reserve(text.size());

  size_t i = 0;
  while (i < text.size()) {
    if (text.compare(i, kOpenLength, kOpen) == 0) {
      const size_t close = text.find(kClose, i + kOpenLength);
      if (close == std::string::npos) {
        break;  // an unterminated block swallows the rest
      }
      i = close + kCloseLength;
      continue;
    }
    out += text[i++];
  }
  return out;
}

std::vector<std::string> GlyphLegend(const TileRegistry &tiles) {
  // Grouped by glyph, because the glyph is what an agent perceives (§2.1) and a
  // collision is a fact about the glyph, not about either tile that uses it.
  //
  // Ordered by first appearance in the sorted name list rather than by glyph, so
  // the output is stable across runs: a legend that reshuffles every launch would
  // make two prompt logs impossible to diff.
  std::vector<std::pair<char, std::vector<std::string>>> rows;
  for (const TileDef *tile : tiles.GetAll()) {
    if (!tile->hasCharacter) continue;
    auto it = std::find_if(rows.begin(), rows.end(), [&](const auto &row) {
      return row.first == tile->character;
    });
    if (it == rows.end()) {
      rows.emplace_back(tile->character, std::vector<std::string>{tile->name});
    } else {
      it->second.push_back(tile->name);
    }
  }

  std::vector<std::string> lines;
  lines.reserve(rows.size());
  for (const auto &row : rows) {
    std::string line = "  '";
    line += row.first;
    line += "'  ";
    for (size_t i = 0; i < row.second.size(); ++i) {
      if (i > 0) line += ", ";
      line += row.second[i];
    }
    if (row.second.size() > 1) {
      line += "   [SHARED: " + std::to_string(row.second.size()) +
              " tiles draw this glyph]";
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

std::string BuildSystemPrompt(const std::string &promptFile,
                              const TileRegistry &tiles) {
  PromptVocabulary vocabulary;
  vocabulary.body = TileVocabulary(tiles);

  const std::vector<std::string> legend = GlyphLegend(tiles);
  if (legend.empty()) {
    vocabulary.glyphs = "  (the catalog is empty, so every printable character is free)\n";
  } else {
    for (const std::string &line : legend) {
      vocabulary.glyphs += line + "\n";
    }
  }

  // The shared contract, from the in-code default. A missing contract file leaves a
  // prompt with no reply-format section rather than a broken one: substitution is
  // replace-only, so the only thing that happens is that a placeholder goes
  // unreplaced, which the caller can detect.
  return BuildPromptForKind(SpriteKinds::For(SpriteKinds::Kind::Tile), promptFile,
                            kDefaultContractFile, vocabulary);
}

std::string SelectShape(std::string document, PromptShape shape) {
  // Strip the *other* shape's sections, and the fences of the one that applies. Leaving the
  // fences in would put `<!--%%SET%%-->` in front of the model, which is exactly the kind of
  // machine scaffolding that makes a model treat a document as a template.
  if (shape == PromptShape::Set) {
    RemoveSection(document, kSingleOpen, kSingleClose);
    document = [&] {
      std::string out = document;
      // The SET fences come off, the content stays.
      for (const char *fence : {kSetOpen, kSetClose}) {
        for (size_t at = out.find(fence); at != std::string::npos;
             at = out.find(fence)) {
          out.erase(at, std::string(fence).size());
        }
      }
      return out;
    }();
  } else {
    RemoveSection(document, kSetOpen, kSetClose);
    for (const char *fence : {kSingleOpen, kSingleClose}) {
      for (size_t at = document.find(fence); at != std::string::npos;
           at = document.find(fence)) {
        document.erase(at, std::string(fence).size());
      }
    }
  }
  return document;
}

std::string BuildPromptForKind(const SpriteKinds::Descriptor &kind,
                               const std::string &promptFile,
                               const std::string &contractFile,
                               const PromptVocabulary &vocabulary,
                               PromptShape shape) {
  std::string prompt = kFallbackPrompt;
  if (!promptFile.empty()) {
    std::ifstream file(promptFile);
    if (file.is_open()) {
      std::stringstream buffer;
      buffer << file.rdbuf();
      prompt = buffer.str();
    } else {
      // A missing prompt file leaves the fallback document in place, so the model is
      // under-informed rather than unconstrained: it still gets the contract and the
      // vocabulary, and simply has no kind-specific guidance. Growing whatever arrived
      // is not an option -- substitution is replace-only, deliberately (SubstituteOnly).
      std::cerr << "SpriteDesigner: could not read the " << kind.id << " prompt file "
                << promptFile << "; using the fallback document and the generated "
                << "vocabulary." << std::endl;
    }
  }

  // The shape first, so the contract is substituted into the document the model will
  // actually see. Doing it after would substitute into text that is about to be thrown away,
  // which is harmless but makes the order confusing for no gain.
  prompt = SelectShape(prompt, shape);

  // The contract goes in first, and *only* by replacement.
  //
  // Order matters here in a way that is easy to miss: the contract document explains
  // its own placeholders in prose ("the kind document substitutes this in where it says
  // `%CONTRACT%`"), so anything that scans the assembled prompt for placeholders will
  // find that mention and substitute into the middle of a sentence. The contract's own
  // text is therefore never scanned: the kind document is substituted first, and every
  // replacement after this point is a replace-only pass over the result.
  std::string contract;
  if (!contractFile.empty()) {
    std::ifstream file(contractFile);
    if (file.is_open()) {
      std::stringstream buffer;
      buffer << file.rdbuf();
      contract = buffer.str();
    } else {
      std::cerr << "SpriteDesigner: could not read the shared reply contract "
                << contractFile << "; the " << kind.id
                << " prompt has no reply format section." << std::endl;
    }
  }
  SubstituteOnly(prompt, kContractPlaceholder, contract);

  // Replace-only from here: a document that already lists its vocabulary keeps its list,
  // and one that carries a placeholder gets the live list substituted. Nothing is ever
  // appended (see SubstituteOnly).
  if (!vocabulary.glyphs.empty()) {
    SubstituteOnly(prompt, kGlyphPlaceholder, vocabulary.glyphs);
  }

  // The vocabulary body, falling back to the kind's own note when there is none --
  // "the catalog is empty" is information, and an empty section reads as an omission.
  const std::string body =
      vocabulary.body.empty() ? kind.emptyVocabularyNote + "\n" : vocabulary.body;
  SubstituteOnly(prompt, kVocabularyPlaceholder, body);
  // A file written before the placeholder was generalised still gets its list, and
  // does not get it twice when it has both spellings. The legacy spelling keeps its
  // append-when-absent behaviour, so such a file still carries the vocabulary.
  SubstituteOnly(prompt, kLegacyTilePlaceholder, body);
  return prompt;
}

bool NameTaken(const TileRegistry &tiles, const std::string &name) {
  return tiles.Get(name) != nullptr;
}

nlohmann::json WriteTileDef(const TileDef &def) {
  nlohmann::json out;
  out["name"] = def.name;
  if (def.hasCharacter) {
    // A one-character JSON string. Stored from the raw char, so a glyph that is
    // not printable ASCII round-trips as the byte it was.
    out["character"] = std::string(1, def.character);
  }
  if (def.hasCharacterColor) {
    // Explicitly int: a Color's channels are unsigned char, and nlohmann treats
    // an unsigned char as a character, so the braced list would serialise
    // [255, 1, 2, 255] as [255, '\u0001', '\u0002', 255] rather than as the
    // numbers the schema and TileRegistry's parser expect.
    out["characterColor"] = {static_cast<int>(def.characterColor.r),
                             static_cast<int>(def.characterColor.g),
                             static_cast<int>(def.characterColor.b),
                             static_cast<int>(def.characterColor.a)};
  }
  if (def.hasBackgroundColor) {
    out["backgroundColor"] = {static_cast<int>(def.backgroundColor.r),
                              static_cast<int>(def.backgroundColor.g),
                              static_cast<int>(def.backgroundColor.b),
                              static_cast<int>(def.backgroundColor.a)};
  }
  out["blocksTile"] = def.blocksTile;
  if (!def.sprite.empty()) {
    out["sprite"] = def.sprite;
  }
  if (def.hasHitboxSize) {
    out["hitboxSize"] = def.hitboxSize;
  }
  return out;
}

std::string SlugifyFileName(const std::string &name) { return SanitizeStem(name); }

SetApplyResult ApplySetReply(const std::string &reply, TileRegistry &tiles,
                             const std::string &overlayDirectory) {
  SetApplyResult result;

  // The extract is the same one the single path does, deliberately: the fence and the
  // reasoning stripping are about the *reply*, and the envelope is about what is inside it.
  // Two implementations here would be two things able to disagree about what a reply is.
  const std::string cleaned = StripReasoning(reply);
  bool fromBlock = false;
  const std::string block = ExtractTileJson(cleaned, &fromBlock);
  if (block.empty()) {
    result.error = "the reply was empty. Send the definition as one ```json block, as the "
                   "instructions require.";
    return result;
  }

  const SpriteReply::Parsed parsed = SpriteReply::SplitText(block);
  if (!parsed.ok) {
    result.error = parsed.error;
    return result;
  }

  result.ok = true;
  result.setName = parsed.setName;

  // Applied one at a time, and each one's verdict kept even when an earlier entry failed. The
  // catalog is only changed by the entries that pass, which is what makes a partial set
  // usable rather than all-or-nothing.
  for (size_t index = 0; index < parsed.definitions.size(); ++index) {
    const nlohmann::json &definition = parsed.definitions[index];
    ApplyResult applied = ApplyJson(definition, tiles, overlayDirectory);
    if (!applied.ok) {
      // The index is the whole point of per-entry refusal: the retry message can say
      // "entry 3" and the model knows which one to fix. Prefixing rather than replacing,
      // because the validator's own words are what it has to act on.
      applied.error = "entry " + std::to_string(index) + ": " + applied.error;
    }
    if (applied.ok) {
      ++result.accepted;
    } else {
      ++result.refused;
    }
    result.sprites.push_back(std::move(applied));
  }
  return result;
}

ApplyResult ApplyJson(const nlohmann::json &tileJson, TileRegistry &tiles,
                      const std::string &overlayDirectory) {
  ApplyResult result;

  if (tiles.Get(tileJson.value("name", std::string())) != nullptr) {
    result.nameTaken = true;
  }

  // A hand-authored name is refused before the write path is reached, and the
  // refusal is the registry's, not this function's: one validator, so the window,
  // the headless mode and the overlay loader cannot disagree about what is legal.
  TileRegistry::AddResult added = tiles.AddRuntime(tileJson, "(tile designer)");
  result.warnings = added.warnings;
  if (!added.ok) {
    result.error = added.error;
    result.name = tileJson.value("name", std::string());
    return result;
  }

  result.ok = true;
  result.definition = added.definition;
  result.name = added.definition->name;

  if (!overlayDirectory.empty()) {
    std::string writeError;
    result.savedPath =
        WriteDefinition(*added.definition, overlayDirectory, writeError);
    if (result.savedPath.empty()) {
      // The tile is accepted and live; only persisting it failed. Reported rather
      // than silently downgraded to a refusal, because the tile *does* exist now
      // and saying otherwise would be a lie the caller acts on.
      result.saveFailed = true;
      result.warnings.push_back("the tile was accepted but not saved: " +
                                writeError);
    }
  }
  return result;
}

ApplyResult ApplyReply(const std::string &reply, TileRegistry &tiles,
                       const std::string &overlayDirectory) {
  ApplyResult result;

  const std::string cleaned = StripReasoning(reply);
  bool fromBlock = false;
  const std::string block = ExtractTileJson(cleaned, &fromBlock);
  if (block.empty()) {
    result.error =
        "the reply was empty. Send the tile as one ```json block, as "
        "TILEDESIGNER.md requires.";
    return result;
  }
  // A bare object is accepted (see ExtractTileJson), but the contract asks for a
  // fence, and a reply that ignored that is worth saying so about -- once, on a
  // successful attempt, without refusing it. Refusing a valid tile over its
  // packaging would teach the model nothing about tiles.
  const bool unfenced = !fromBlock;

  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(block);
  } catch (const std::exception &e) {
    result.error = std::string("the tile is not valid JSON: ") + e.what();
    return result;
  }
  if (!parsed.is_object()) {
    result.error = "the json block must be a JSON object describing one tile.";
    return result;
  }

  // Required by the contract, and the reason it is required is the whole point of
  // the window: the model's own judgement of what it invented. Stripped before
  // storage because it is not part of the tile schema, exactly as the blueprint
  // designer strips its own notes.
  if (!parsed.contains("designerNotes") || !parsed["designerNotes"].is_string() ||
      parsed["designerNotes"].get<std::string>().empty()) {
    result.error =
        "the tile has no non-empty \"designerNotes\" string. Every tile must "
        "carry your own thoughts on it.";
    return result;
  }
  const std::string notes = parsed["designerNotes"].get<std::string>();
  parsed.erase("designerNotes");

  ApplyResult applied = ApplyJson(parsed, tiles, overlayDirectory);
  applied.notes = notes;
  if (unfenced) {
    applied.warnings.push_back(
        "the tile was not inside a ```json block; it was accepted anyway, but a "
        "reply should fence it");
  }
  return applied;
}

ApplyResult ApplyFile(const std::string &path, TileRegistry &tiles,
                      const std::string &overlayDirectory) {
  ApplyResult result;
  std::ifstream file(path);
  if (!file.is_open()) {
    result.error = "could not read " + path;
    return result;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return ApplyReply(buffer.str(), tiles, overlayDirectory);
}

size_t LoadOverlayDirectory(const std::string &overlayDirectory,
                            TileRegistry &tiles, DebugLog *log) {
  auto warn = [log](const std::string &message) {
    // Always to stderr as well as the log: DebugLog keeps entries in memory and
    // never prints, so in the headless modes -- the ones that exist to make this
    // path verifiable -- routing through it alone would make a broken overlay
    // tile completely silent. Being told twice in a game run is the cheaper
    // mistake.
    std::cerr << message << std::endl;
    if (log) {
      log->LogWarning(message);
    }
  };

  std::error_code ec;
  if (!std::filesystem::exists(overlayDirectory, ec)) {
    // Not an error, and deliberately not a warning either: a run that has never
    // created a tile is the normal case, and warning about it on every launch
    // would train people to ignore the warning that matters.
    return 0;
  }

  size_t added = 0;
  for (const auto &entry :
       std::filesystem::directory_iterator(overlayDirectory, ec)) {
    if (ec) {
      warn("Tile overlay: could not list " + overlayDirectory + ": " +
           ec.message());
      return added;
    }
    if (entry.path().extension() != ".json") continue;
    // The only files written here are .json and, briefly, .json.tmp -- the
    // extension test above already excludes the temporary one, so nothing extra
    // is needed; a crash mid-write cannot surface as a malformed definition.

    std::ifstream file(entry.path());
    if (!file.is_open()) continue;

    try {
      nlohmann::json j;
      file >> j;
      // Runtime additions may replace other runtime additions -- that is what
      // makes the overlay reloadable -- but never a hand-authored catalog tile,
      // which AddRuntime enforces itself.
      TileRegistry::AddResult result = tiles.AddRuntime(
          j, entry.path().string(), /*replaceExisting=*/true);
      if (!result.ok) {
        warn("Tile overlay: " + entry.path().string() + " was refused: " +
             result.error);
        continue;
      }
      ++added;
    } catch (const std::exception &e) {
      warn("Tile overlay: failed to parse " + entry.path().string() + ": " +
           e.what());
    }
  }
  return added;
}

}  // namespace TileDesigner
