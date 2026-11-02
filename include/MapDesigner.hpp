#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class ObjectFactory;
class TileRegistry;

// Turning a designer reply into a real map.
//
// The model does not write a map file: a 32x32 map is 1024 palette indices, and a
// model that miscounts one shifts every cell after it. So the reply is a *design*
// -- a legend plus ASCII rows, the shape a model is actually good at -- and this
// module expands it into the same document Map::Load reads. What it validates is
// what Map would reject later anyway, just earlier and with a sentence the
// designer can retry from.
namespace MapDesigner {

// Bigger maps cost tokens per row without adding interest; smaller ones cannot hold
// a place. Both ends are a contract, not a policy, but they are the only numbers the
// prompt and the validator have to agree on, so they live in one place.
inline constexpr int kMinSide = 8;
inline constexpr int kMaxSide = 64;

// What the pane previews and Save writes. `document` is already the full map JSON:
// the reply's compact rows are expanded once, at validation time, so nothing
// downstream has to redo it.
struct Draft {
  std::string name;
  std::string description;
  int width = 0;
  int height = 0;
  int tileWidth = 32;
  int tileHeight = 32;
  int objectCount = 0;
  nlohmann::json document;
  // Things that were odd but not fatal -- a tile used in the legend and nowhere on
  // the map, an object placed on a blocking cell.
  std::vector<std::string> warnings;
};

struct ApplyResult {
  bool ok = false;
  // The sentence fed back to the model for a retry.
  std::string error;
  Draft draft;
  std::string notes;
};

// Validates the reply and expands it. Never writes anything.
ApplyResult ApplyReply(const nlohmann::json &reply, const TileRegistry &tiles,
                       const ObjectFactory &objects);

// The system prompt: the instruction file with %TILES% and %OBJECTS% replaced by
// the live vocabulary, so a model cannot invent a tile name the world lacks.
std::string BuildSystemPrompt(const std::string &promptFile,
                              const TileRegistry &tiles,
                              const ObjectFactory &objects);

// Writes maps/<slug>.json. Refuses -- with the colliding path named -- when that
// file already exists, because a generated map must never silently replace a
// hand-authored one.
bool Save(const Draft &draft, const std::string &mapsDirectory,
          std::string *savedPath, std::string *error);

}  // namespace MapDesigner
