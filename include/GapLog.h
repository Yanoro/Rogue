#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class BlueprintRegistry;
class DebugLog;
class ObjectFactory;
class TileRegistry;

// The generation worklist: what the art pack cannot draw.
//
// Stage 0 of docs/asset-generation-design.md. It is deliberately a *report* and
// not a planner -- it decides nothing, it names what is missing and where it was
// wanted, and something else (a human, or the generator) decides what to do.
//
// Two kinds of gap, and keeping them apart is most of the value:
//
//   coverage   -- the definition exists, no sprite resolves for it. This is the
//                 batch worklist: generate art, in the art pack's style.
//   vocabulary -- a blueprint names a material or object the world does not
//                 define. That is a content bug or a creation candidate (§4.3),
//                 not something to draw, and it is reported separately so a
//                 40-sprite batch never reads as 40 invented materials.
//
// It never writes into the art pack. The report lands in the generated layer, or
// beside data/sprites/ when no art pack is configured (§1.2, §9.3).
namespace GapLog {

struct CoverageGap {
  std::string kind;          // "tile" or "object"
  std::string name;          // the tile name, or the object template id
  std::string label;         // human-readable name, for the report
  std::string conventionId;  // what the sprite lookup probed
  std::string explicitPath;  // the authored "sprite" key, if there was one
};

struct VocabularyGap {
  std::string blueprint;  // blueprint id
  std::string kind;       // "paint" or "place"
  std::string name;       // the missing material or object
  std::vector<std::string> parts;
};

struct Report {
  std::vector<CoverageGap> coverage;
  std::vector<VocabularyGap> vocabulary;
  size_t tilesScanned = 0;
  size_t objectsScanned = 0;
  size_t blueprintsScanned = 0;
  std::string artPack;
  std::string lookedIn;  // the roots sprites were probed in, for the report
};

// Builds the report. Any registry may be null; a null one contributes nothing
// rather than failing, so a run with no catalog still produces a report instead
// of an error. Both lists come out sorted, so a regenerated report diffs cleanly.
Report Build(const TileRegistry *tiles, const ObjectFactory *objects,
             const BlueprintRegistry *blueprints);

// Where the report belongs: the generated layer for the configured art pack, or
// beside data/sprites/ when there is none.
std::string DefaultPath();

nlohmann::json ToJson(const Report &report);
bool Write(const Report &report, const std::string &path, DebugLog *log);

// One line, e.g. "art gaps: 12 tiles and 5 objects have no sprite".
std::string Summary(const Report &report);

}  // namespace GapLog
