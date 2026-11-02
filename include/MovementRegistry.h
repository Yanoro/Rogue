#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class DebugLog;

// A shared movement template: what "walking" is, in words and timing.
//
// A movement is not an animation. It is the semantic half -- description, frame count,
// cadence, the per-frame pose plan, which parts participate, and what equipment it needs.
// The rig holds the other half: the frame sprites for its own body, generated from this.
// The same "walk" therefore becomes different images on every rig, while the intent is
// written once.
//
// The point of the split is a vocabulary. A skill says `animation: "kick"`; a developer
// adding a Kick movement does not touch any rig, and generating it is a Designer action.
struct MovementDef {
  std::string id;
  std::string name;
  // "locomotion", "attack", "cast", "social"... Free text, for grouping in the picker.
  std::string category;
  // The whole-body motion, in prose. This is what the per-part requests are cut from.
  std::string description;
  int frames = 4;
  int fps = 8;
  bool loop = true;
  // Part ids that participate in this movement. Empty means every part of the rig.
  // A part that does not participate keeps its static sprite in the clip, which is how
  // "a kick does not animate the hair" is expressed.
  std::vector<std::string> parts;
  // Capabilities the rig or its equipment must provide before this movement can be
  // drawn honestly ("weapon", "focus"). The Designer refuses to generate these until a
  // worn layer can follow the body; the entry itself is still authorable so skills and
  // prompts can name it.
  std::vector<std::string> needs;
  // One line per frame: the pose the part requests are cut from. Not validated for
  // motion -- the compiler cannot see it -- but this is the quality lever.
  std::vector<std::string> poses;
};

// The movement registry, loaded from data/movements/<id>.json.
//
// Same contract as the other content registries: a directory of JSON documents, an id
// from the name, add-or-replace for the Content Editor, and a document round trip.
class MovementRegistry {
public:
  void Load(const std::string& directoryPath);

  // nullptr for an unknown id. The pointer is stable until the next add or replace.
  const MovementDef* Get(const std::string& id) const;
  std::vector<const MovementDef*> GetAll() const;
  size_t Count() const { return movements.size(); }

  // The document as the Content Editor edits it. The id is omitted: the editor injects it
  // from the selection.
  nlohmann::json ToJson(const MovementDef& definition) const;

  // Validates and stores `document`, replacing any entry with the same id.
  bool AddOrReplace(const nlohmann::json& document, const std::string& source,
                    std::string* error, std::vector<std::string>* warnings);

  // The id a document is stored under: the slug of its name.
  static std::string IdFor(const nlohmann::json& document);

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  std::unordered_map<std::string, MovementDef> movements;
  DebugLog* debugLog = nullptr;
};
