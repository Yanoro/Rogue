#include "GapLog.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "BlueprintRegistry.h"
#include "Components.h"
#include "DebugLog.h"
#include "ObjectFactory.h"
#include "Settings.h"
#include "SpritePaths.hpp"
#include "TileRegistry.h"

namespace GapLog {

namespace {

// Sorted so the file is stable: re-running the report against unchanged content
// must produce identical bytes, or a diff stops meaning "the content changed".
void SortReport(Report &report) {
  std::sort(report.coverage.begin(), report.coverage.end(),
            [](const CoverageGap &a, const CoverageGap &b) {
              return a.kind != b.kind ? a.kind < b.kind : a.name < b.name;
            });
  std::sort(report.vocabulary.begin(), report.vocabulary.end(),
            [](const VocabularyGap &a, const VocabularyGap &b) {
              if (a.blueprint != b.blueprint) return a.blueprint < b.blueprint;
              if (a.kind != b.kind) return a.kind < b.kind;
              return a.name < b.name;
            });
}

std::string DescribeRoots() {
  const SpriteRootsInfo roots = CurrentSpriteRoots();
  if (!roots.artPackSprites.empty()) {
    std::string text = roots.artPackSprites;
    if (!roots.generatedSprites.empty()) {
      text += " then " + roots.generatedSprites;
    }
    return text;
  }
  return std::string(SPRITE_DIRECTORY);
}

}  // namespace

Report Build(const TileRegistry *tiles, const ObjectFactory *objects,
             const BlueprintRegistry *blueprints) {
  Report report;
  report.artPack = Settings().art.pack;
  report.lookedIn = DescribeRoots();

  // Coverage: everything the world can put on screen, probed for a sprite. This
  // walks the *whole* vocabulary rather than only what a blueprint happens to
  // mention, because "what can this art pack not draw" is the question the
  // generator needs answered and a blueprint is only one way to want a thing.
  if (tiles != nullptr) {
    for (const TileDef *definition : tiles->GetAll()) {
      if (definition == nullptr) continue;
      report.tilesScanned++;
      const std::string conventionId = TileSpriteId(definition->name);
      if (!SpriteResolves(definition->sprite, conventionId)) {
        report.coverage.push_back({"tile", definition->name, definition->name,
                                   conventionId, definition->sprite});
      }
    }
  }

  if (objects != nullptr) {
    for (const auto &entry : objects->GetTemplates()) {
      const std::string &id = entry.first;
      const nlohmann::json &tmpl = entry.second;
      report.objectsScanned++;
      // An object template's convention id is its template id, which is also the
      // key a blueprint's "place" names.
      const std::string explicitPath =
          tmpl.is_object() ? tmpl.value("sprite", std::string()) : std::string();
      if (!SpriteResolves(explicitPath, id)) {
        const std::string label =
            tmpl.is_object() ? tmpl.value("name", id) : id;
        report.coverage.push_back({"object", id, label, id, explicitPath});
      }
    }
  }

  // Vocabulary: what the designs ask for and the world cannot supply. The records
  // come from FindMissingVocabulary so this and the load-time warning can never
  // disagree about what is missing.
  if (blueprints != nullptr) {
    for (const Blueprint *blueprint : blueprints->GetAll()) {
      if (blueprint == nullptr) continue;
      report.blueprintsScanned++;
      for (const MissingMaterial &missing :
           FindMissingVocabulary(*blueprint, tiles, objects)) {
        report.vocabulary.push_back(
            {blueprint->id, missing.kind, missing.material, missing.partIds});
      }
    }
  }

  SortReport(report);
  return report;
}

std::string DefaultPath() {
  const SpriteRootsInfo roots = CurrentSpriteRoots();
  std::filesystem::path base;
  if (!roots.generatedSprites.empty()) {
    // generated/<pack>/sprites -> generated/<pack>/gaps.json
    base = std::filesystem::path(roots.generatedSprites).parent_path();
  } else if (!roots.artPackSprites.empty()) {
    // No generated layer derived (a pack named by a single path component), so
    // the report sits beside the pack rather than nowhere.
    base = std::filesystem::path(roots.artPackSprites).parent_path();
  } else {
    // No art pack: beside the art it describes, which is data/sprites/.
    //
    // Stripping the separator explicitly rather than just calling parent_path():
    // SPRITE_DIRECTORY is "data/sprites/", and a trailing separator makes
    // filename() empty and parent_path() return "data/sprites" itself, so the
    // report would land *inside* the sprite directory. The report belongs beside
    // it, where palette.json already is.
    std::string sprites(SPRITE_DIRECTORY);
    while (!sprites.empty() && (sprites.back() == '/' || sprites.back() == '\\')) {
      sprites.pop_back();
    }
    base = std::filesystem::path(sprites).parent_path();
  }
  return (base / "gaps.json").string();
}

nlohmann::json ToJson(const Report &report) {
  nlohmann::json coverage = nlohmann::json::array();
  for (const CoverageGap &gap : report.coverage) {
    coverage.push_back({{"kind", gap.kind},
                        {"name", gap.name},
                        {"label", gap.label},
                        {"conventionId", gap.conventionId},
                        {"explicitPath", gap.explicitPath}});
  }

  nlohmann::json vocabulary = nlohmann::json::array();
  for (const VocabularyGap &gap : report.vocabulary) {
    vocabulary.push_back({{"blueprint", gap.blueprint},
                          {"kind", gap.kind},
                          {"name", gap.name},
                          {"parts", gap.parts}});
  }

  return nlohmann::json{
      {"_readme",
       "Art coverage and vocabulary gaps. 'coverage' is the generation "
       "worklist: the definition exists but no sprite resolves for it. "
       "'vocabulary' is a blueprint naming something the world does not define, "
       "which is a content bug or a creation candidate, not art to draw. "
       "Generated by AIRogue --gaps; see docs/asset-generation-design.md."},
      {"artPack", report.artPack},
      {"lookedIn", report.lookedIn},
      {"scanned", {{"tiles", report.tilesScanned},
                   {"objects", report.objectsScanned},
                   {"blueprints", report.blueprintsScanned}}},
      {"counts", {{"coverage", report.coverage.size()},
                  {"vocabulary", report.vocabulary.size()}}},
      {"coverage", coverage},
      {"vocabulary", vocabulary}};
}

bool Write(const Report &report, const std::string &path, DebugLog *log) {
  std::error_code directoryError;
  const std::filesystem::path target(path);
  if (target.has_parent_path()) {
    std::filesystem::create_directories(target.parent_path(), directoryError);
    if (directoryError) {
      const std::string message =
          "Gap report: cannot create " + target.parent_path().string() + ": " +
          directoryError.message() + "\n";
      if (log) log->LogError(message);
      else std::cerr << message;
      return false;
    }
  }

  std::ofstream file(path);
  if (!file.is_open()) {
    const std::string message =
        "Gap report: cannot write " + path + "\n";
    if (log) log->LogError(message);
    else std::cerr << message;
    return false;
  }
  file << ToJson(report).dump(2) << "\n";
  return true;
}

std::string Summary(const Report &report) {
  return "art gaps: " + std::to_string(report.coverage.size()) +
         " without a sprite (" + std::to_string(report.tilesScanned) +
         " tiles, " + std::to_string(report.objectsScanned) + " objects), " +
         std::to_string(report.vocabulary.size()) +
         " undefined reference(s) across " +
         std::to_string(report.blueprintsScanned) + " blueprint(s)";
}

}  // namespace GapLog
