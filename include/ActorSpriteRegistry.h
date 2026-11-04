#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class DebugLog;

// One actor sprite asset: art for a being, and nothing about being one.
//
// This is not a definition of a creature. The `Actor` tag (include/Components.h) is what
// marks an entity as living; a sprite says only what something looks like, so the same
// goblin art can be an enemy, a shopkeeper or scenery in a cutscene without any of those
// being written down here.
struct ActorSpriteDef {
  // The file stem, and the sprite convention id: `goblin` is this entry, `goblin.sprite`
  // and `goblin.png`. One name for all three, so nothing has to be kept in sync.
  std::string id;
  std::string name;
  std::string description;
  // Optional ArtStyleRegistry override; empty uses the family default.
  std::string style;
};

// The actor-sprite catalogue, loaded from data/actor_sprites/<id>.json.
//
// A registry rather than a directory scan so an entry can carry a readable name and a
// description, and so a sprite can be listed before its art exists (an entry whose sprite
// is still to be drawn shows as "no sprite" in the Content Editor instead of vanishing).
class ActorSpriteRegistry {
public:
  void Load(const std::string& directoryPath);

  // nullptr for an unknown id. The pointer is stable until the next add or replace.
  const ActorSpriteDef* Get(const std::string& id) const;
  std::vector<const ActorSpriteDef*> GetAll() const;
  size_t Count() const { return sprites.size(); }

  // The document as the Content Editor edits it.
  nlohmann::json ToJson(const ActorSpriteDef& definition) const;

  // Validates and stores `document`, replacing any entry with the same id.
  bool AddOrReplace(const nlohmann::json& document, const std::string& source,
                    std::string* error, std::vector<std::string>* warnings);

  // The id a document is stored under: the slug of its name.
  static std::string IdFor(const nlohmann::json& document);

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  std::unordered_map<std::string, ActorSpriteDef> sprites;
  DebugLog* debugLog = nullptr;
};
