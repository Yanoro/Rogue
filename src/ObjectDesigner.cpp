#include "ObjectDesigner.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

#include "DebugLog.h"
#include "SpriteReply.hpp"
#include "TileDesigner.hpp"
#include "TileRegistry.h"

namespace ObjectDesigner {

namespace {

// Reports a problem so *both* audiences see it: the run's debug log (which the in-game
// window shows) and stderr (which a headless run and its operator see).
//
// This exists because the overlay loader originally reported only through the log, and
// the log is a bounded in-memory buffer that nothing prints -- so `--sprite-design
// object` silently loaded nothing when an overlay file was refused, with no line
// anywhere saying why. A message only one of the two audiences can see is the same bug
// as no message for the other.
void Report(DebugLog* log, bool isError, const std::string& message) {
  if (log != nullptr) {
    if (isError) log->LogError(message); else log->LogWarning(message);
  }
  std::cerr << message << "\n";
}

// The keys a definition may not keep: everything the designer produced for the *user*
// rather than for the engine. Stripped before the definition is written, exactly as the
// tile side strips its notes, so a hand-read overlay file holds only engine fields.
constexpr const char* kDesignerNotesKey = "designerNotes";

// The keys the designer itself owns. `key` is honoured when present because the key is
// the sprite convention id, but it is not asked for: AddRuntime derives it from the
// name, and a model that sets them inconsistently would produce a sprite named after
// one thing and an object named after another.
bool IsDesignerKey(const std::string& key) {
  return key == kDesignerNotesKey;
}

std::string JoinNames(const std::vector<std::string>& names) {
  std::string out;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out += i + 1 == names.size() ? " and " : ", ";
    out += names[i];
  }
  return out;
}

// How close `candidate` is to `known`, for "did you mean" in a refusal. A refusal that
// names the closest real component turns a guessing game into a correction, which is
// the difference between one retry and four.
bool LooksLikeTypo(const std::string& candidate, const std::string& known) {
  if (candidate.empty() || known.empty()) return false;
  // Case-insensitive prefix, or a shared prefix of four characters. Deliberately crude:
  // a real edit distance would rank "Storage" near "Portable" and suggest the wrong
  // thing, and the point is only to catch "storage" for "Storage".
  if (candidate.size() >= 3) {
    std::string a = candidate;
    std::string b = known;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    if (a == b) return true;  // right word, wrong case
    if (a.size() >= 3 && b.rfind(a, 0) == 0) return true;
  }
  return false;
}

std::string SuggestComponent(const std::string& wanted,
                             const std::vector<std::string>& known) {
  for (const std::string& name : known) {
    if (LooksLikeTypo(wanted, name)) return name;
  }
  return {};
}

}  // namespace

std::string ExtractObjectJson(const std::string& text, bool* fromBlock) {
  // The fence is shared with the tile designer rather than reimplemented: it is one
  // contract, and a second parser would be a second thing able to disagree about what a
  // reply is.
  const std::string block = TileDesigner::ExtractJsonBlock(text);
  if (!block.empty()) {
    if (fromBlock != nullptr) *fromBlock = true;
    return block;
  }
  if (fromBlock != nullptr) *fromBlock = false;
  return text;
}

ApplyResult ApplyObject(const nlohmann::json& json, ObjectFactory& objects,
                        const std::string& overlayDirectory,
                        const TileRegistry* tiles) {
  ApplyResult result;

  if (!json.is_object()) {
    result.error = "the definition is not a JSON object";
    return result;
  }

  result.name = json.value("name", std::string());
  result.definition = json;
  // designerNotes is pulled out before anything else looks at the definition, so no
  // later step can accidentally include it in the key, the write, or the preview.
  if (json.contains(kDesignerNotesKey)) {
    if (json[kDesignerNotesKey].is_string()) {
      result.notes = json[kDesignerNotesKey].get<std::string>();
    }
    result.definition.erase(kDesignerNotesKey);
  }

  if (result.name.empty()) {
    result.error =
        "the definition has no non-empty \"name\" string. Every object needs a name: "
        "it is what a blueprint, a spawn or another agent references";
    return result;
  }
  if (result.notes.empty()) {
    result.error =
        "the definition has no non-empty \"designerNotes\" string. Every object must "
        "carry your own thoughts on it";
    return result;
  }

  // An unknown field is a *warning* here, not a refusal, and that is a deliberate
  // difference from tiles. A tile is four fields and a wrong one is almost certainly a
  // misunderstanding of the grammar; an object is an open set of capability parameters,
  // so the useful message is not "refused" but "the engine will ignore this key".
  {
    const std::vector<std::string> known = objects.KnownFieldNames();
    for (const auto& pair : json.items()) {
      if (IsDesignerKey(pair.key())) continue;
      if (std::find(known.begin(), known.end(), pair.key()) != known.end()) continue;
      result.warnings.push_back("\"" + pair.key() +
                                "\" is not a field the engine reads, so it will be "
                                "ignored");
    }
  }

  // A cross-kind collision is a warning, not a refusal. The two registries are
  // genuinely separate -- data/objects/iron_scythe.json and the iron scythe item already
  // share a key, and 'anvil' is both a tile and an object -- so refusing one half of a
  // pair that already ships would be wrong. It is reported because an agent reading the
  // world will ask which one it is looking at.
  if (tiles != nullptr && tiles->Get(result.name) != nullptr) {
    result.crossKindNameCollision = true;
    result.warnings.push_back("a *tile* named \"" + result.name +
                              "\" already exists. The two are separate namespaces and "
                              "this is allowed -- the world already has an 'anvil' that "
                              "is both -- but anything reading a map will show the same "
                              "name for two different things");
  }

  // Art the engine will accept but a designer should not have produced.
  //
  // These are *warnings*, not refusals, and the distinction is deliberate. The engine's
  // art parser is lenient by design (`include/AsciiArt.hpp` says so in as many words): a
  // ragged row is padded and a codepoint missing from the palette takes white. Both are
  // reasonable for hand-authored content and both are wrong for an object an agent just
  // designed -- a ragged block was a mistake, and a palette gap means the designer wrote
  // a colour map that does not match its own art. So the engine stays lenient and the
  // designer says so, which costs a retry and leaves the world's loader untouched.
  if (json.contains("art") && json["art"].is_array()) {
    size_t width = 0;
    bool ragged = false;
    std::set<std::string> used;
    for (const auto& line : json["art"]) {
      if (!line.is_string()) continue;
      const std::string text = line.get<std::string>();
      // Counted in codepoints, not bytes: art may use multi-byte block characters and
      // one of those is one cell.
      auto advance = [](const std::string& s, size_t i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        return (c < 0x80) ? size_t{1} : (c < 0xE0) ? size_t{2} : (c < 0xF0) ? size_t{3}
                                                                           : size_t{4};
      };
      size_t cells = 0;
      for (size_t i = 0; i < text.size(); i += advance(text, i)) ++cells;

      if (width == 0) {
        width = cells;
      } else if (cells != width) {
        ragged = true;
      }
      for (size_t i = 0; i < text.size(); i += advance(text, i)) {
        used.insert(text.substr(i, advance(text, i)));
      }
    }
    if (ragged) {
      result.warnings.push_back(
          "the \"art\" rows are not all the same width, so the block will be padded and "
          "the drawing lopsided. Every row needs the same number of characters");
    }
    if (json.contains("palette") && json["palette"].is_object()) {
      std::vector<std::string> missing;
      for (const std::string& glyph : used) {
        if (glyph == " ") continue;  // a space needs no colour
        if (!json["palette"].contains(glyph)) missing.push_back(glyph);
      }
      if (!missing.empty()) {
        std::string list;
        for (size_t i = 0; i < missing.size(); ++i) {
          if (i > 0) list += ", ";
          list += "'" + missing[i] + "'";
        }
        result.warnings.push_back("the \"palette\" has no colour for " + list +
                                  ", so those cells draw white instead of the colour "
                                  "you intended");
      }
    }
  }

  // A revision of something the designer itself made is allowed; replacing hand-authored
  // content is not, and AddRuntime is what tells the two apart. Recognised *before* the
  // add so the verdict can report a revision rather than a creation.
  const std::string predictedKey = ObjectFactory::KeyFor(result.definition);
  result.revised = !predictedKey.empty() && objects.IsRuntimeAdded(predictedKey);

  // The validation itself, through the setters that will load it. This is where an
  // unknown component is caught, and the refusal is the setter's own words.
  const ObjectFactory::RuntimeAddResult added =
      objects.AddRuntime(result.definition, "(object designer)",
                         /*replaceExisting=*/true);
  result.key = added.key;
  for (const std::string& warning : added.warnings) {
    result.warnings.push_back(warning);
  }
  if (!added.ok) {
    result.error = added.error;
    // A refusal that names the closest real component turns a guess into a correction.
    // Detected from the message rather than plumbed through, because the alternative is
    // AddRuntime returning a structured "unknown name" for one caller's benefit.
    const std::string marker = "Unknown component type: ";
    const size_t at = result.error.find(marker);
    if (at != std::string::npos) {
      std::string wanted = result.error.substr(at + marker.size());
      while (!wanted.empty() && (wanted.back() == '\n' || wanted.back() == ' ')) {
        wanted.pop_back();
      }
      const std::string suggestion = SuggestComponent(wanted, objects.ComponentNames());
      if (!suggestion.empty()) {
        result.error += ". Did you mean \"" + suggestion + "\"?";
      } else {
        result.error += ". The components the engine has are: " +
                        JoinNames(objects.ComponentNames());
      }
    }
    return result;
  }

  // ---- persist -----------------------------------------------------------------
  //
  // Written after acceptance, so a refused definition never leaves a file behind. The
  // directory may be empty, in which case the object exists for this run only -- which
  // is what a headless run wants and what the window warns about.
  if (!overlayDirectory.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(overlayDirectory, ec);
    if (ec) {
      result.saveFailed = true;
      result.warnings.push_back("could not create " + overlayDirectory + ": " +
                                ec.message());
    } else {
      const std::filesystem::path path =
          std::filesystem::path(overlayDirectory) / (result.key + ".json");
      std::ofstream out(path);
      if (!out.is_open()) {
        result.saveFailed = true;
        result.warnings.push_back("could not write " + path.string());
      } else {
        out << WriteObjectDef(result.definition).dump(2) << "\n";
        result.savedPath = path.string();
      }
    }
  }

  result.ok = true;
  return result;
}

ApplyResult ApplyReply(const std::string& reply, ObjectFactory& objects,
                       const std::string& overlayDirectory,
                       const TileRegistry* tiles) {
  ApplyResult result;

  // Reasoning first: a complete reply may carry a `<think>` block, and the fence has to be
  // found in what is left. Shared with the tile side rather than reimplemented -- it is a
  // pure function over the text and a second version would be a second thing to get wrong.
  const std::string cleaned = TileDesigner::StripReasoning(reply);
  bool fromBlock = false;
  const std::string block = ExtractObjectJson(cleaned, &fromBlock);
  if (block.empty()) {
    result.error =
        "the reply was empty. Send the object as one ```json block, as "
        "OBJECTDESIGNER.md requires.";
    return result;
  }
  // A bare object is accepted (see ExtractObjectJson), but the contract asks for a fence
  // and a reply that ignored that is worth saying so about -- once, on a successful
  // attempt, without refusing it. Refusing a valid object over its packaging would teach
  // the model nothing about objects.
  const bool unfenced = !fromBlock;

  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(block);
  } catch (const std::exception& e) {
    result.error = std::string("the object is not valid JSON: ") + e.what();
    return result;
  }
  if (!parsed.is_object()) {
    result.error = "the json block must be a JSON object describing one object.";
    return result;
  }

  result = ApplyObject(parsed, objects, overlayDirectory, tiles);
  if (result.ok && unfenced) {
    // A warning rather than a refusal: the object is good, and the thing to fix is the
    // packaging. The model sees this on the next prompt through the transcript.
    result.warnings.push_back(
        "the object was sent without a ```json fence. Send the whole object inside one "
        "fenced block next time -- it is what the contract asks for");
  }
  return result;
}

SetApplyResult ApplySetReply(const std::string& reply, ObjectFactory& objects,
                             const std::string& overlayDirectory,
                             const TileRegistry* tiles) {
  SetApplyResult result;

  // The same extract the object single path does, and the same one the tile set path uses
  // via SpriteReply: the fence and reasoning stripping are about the reply, the envelope is
  // about its contents.
  const std::string cleaned = TileDesigner::StripReasoning(reply);
  bool fromBlock = false;
  const std::string block = ExtractObjectJson(cleaned, &fromBlock);
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

  for (size_t index = 0; index < parsed.definitions.size(); ++index) {
    const nlohmann::json& definition = parsed.definitions[index];
    ApplyResult applied = ApplyObject(definition, objects, overlayDirectory, tiles);
    if (!applied.ok) {
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

ApplyResult ApplyFile(const std::string& path, ObjectFactory& objects,
                      const std::string& overlayDirectory,
                      const TileRegistry* tiles) {
  ApplyResult result;

  std::ifstream file(path);
  if (!file.is_open()) {
    result.error = "could not read " + path;
    return result;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();

  // Through ApplyReply rather than duplicating the fence/reasoning handling: a file may
  // hold a bare object, and so may a reply, and the two paths must agree about which forms
  // are acceptable or `--sprite-design object` would accept a file the window refuses.
  return ApplyReply(buffer.str(), objects, overlayDirectory, tiles);
}

nlohmann::json WriteObjectDef(const nlohmann::json& definition) {
  nlohmann::json out = nlohmann::json::object();

  // A fixed key order, so two writes of the same object produce byte-identical files
  // and a diff of the overlay means something. nlohmann's default order is
  // alphabetical, which would put "components" before "name"; this puts the identity
  // first because that is what a person reads.
  static const char* kOrder[] = {
      "name",    "description", "components",    "character",
      "characterColor", "backgroundColor", "nameTagColor", "art",
      "palette", "sprite",      "hint",          "fontSize",
  };
  for (const char* key : kOrder) {
    if (definition.contains(key) && !IsDesignerKey(key)) {
      out[key] = definition[key];
    }
  }
  // Everything else -- the capability parameters, and any key a newer component reads
  // that this list has not learned yet. Written last rather than dropped: an unknown key
  // is a warning at validation time, and silently deleting it on the way to disk would
  // turn that warning into data loss.
  for (const auto& pair : definition.items()) {
    if (IsDesignerKey(pair.key())) continue;
    if (out.contains(pair.key())) continue;
    out[pair.key()] = pair.value();
  }
  return out;
}

size_t LoadOverlayDirectory(const std::string& overlayDirectory,
                            ObjectFactory& objects, DebugLog* log) {
  if (overlayDirectory.empty()) return 0;

  std::error_code ec;
  if (!std::filesystem::exists(overlayDirectory, ec)) {
    // Not an error: most runs have no generated objects, and a warning here would fire
    // on every launch of a clean checkout.
    return 0;
  }

  size_t added = 0;
  for (const auto& entry : std::filesystem::directory_iterator(overlayDirectory, ec)) {
    if (ec) break;
    if (entry.path().extension() != ".json") continue;

    std::ifstream file(entry.path());
    if (!file.is_open()) {
      const std::string message =
          "Object overlay: could not read " + entry.path().string();
      Report(log, /*isError=*/false, message);
      continue;
    }

    nlohmann::json json;
    try {
      file >> json;
    } catch (const std::exception& e) {
      const std::string message = "Object overlay: " + entry.path().string() +
                                  " is not valid JSON: " + e.what();
      Report(log, /*isError=*/true, message);
      continue;
    }

    // The "at least one component" rule is relaxed for the overlay: this is content
    // that was already validated when it was designed, and re-checking a rule that a
    // hand-edited file might legitimately break would make the overlay unloadable.
    // Replace is on, so reloading the overlay after a design does not refuse itself.
    const ObjectFactory::RuntimeAddResult result =
        objects.AddRuntime(json, entry.path().string(), /*replaceExisting=*/true,
                           /*requireAtLeastOneComponent=*/false);
    if (!result.ok) {
      const std::string message = "Object overlay: refused " +
                                  entry.path().string() + ": " + result.error;
      Report(log, /*isError=*/false, message);
      continue;
    }
    ++added;
  }
  return added;
}

std::string ComponentVocabulary(const ObjectFactory& objects) {
  std::string out;
  for (const ObjectFactory::ComponentInfo& component :
       objects.ComponentVocabulary()) {
    out += "  - " + component.name;
    if (!component.parameters.empty()) {
      out += "  (";
      for (size_t i = 0; i < component.parameters.size(); ++i) {
        if (i > 0) out += ", ";
        out += component.parameters[i];
      }
      out += ")";
    }
    if (!component.summary.empty()) {
      out += "  -- " + component.summary;
    }
    out += "\n";
  }
  return out;
}

}  // namespace ObjectDesigner
