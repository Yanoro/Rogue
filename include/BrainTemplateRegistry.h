#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class DebugLog;

// What drives an NPC, and nothing about its body.
//
// The counterpart of NpcTemplate: a body can be spawned without a brain (a wolf, a
// training dummy), and one brain can drive many bodies (a town brain shared by the
// blacksmith, the guard and the innkeeper). That separation is why LLM-ness is not a
// registry split -- it is whether this reference is present.
//
// It is called a brain rather than a class because it is *context*: background, goals and
// the model to talk to. A brain does not prescribe behaviour; the agent behind it decides
// what to do with what it knows. The runtime counterpart is AgentBrain; this is data.
struct BrainTemplate {
  std::string id;
  std::string name;
  std::string description;
  // An ai.profiles id (see SettingsSchema). Recorded for the spawn report; per-NPC
  // backends are not wired yet, so a value other than the agent's model is reported
  // rather than acted on.
  std::string modelProfile;
  // The %BACKGROUND% text handed to the agent.
  std::string background;
  // Authored life goals, seeded read-only into the TaskList at spawn (the map's
  // "goals": {"terminal": [...]} keeps the same meaning).
  std::vector<std::string> terminalGoals;
};

// The brain catalogue, loaded from data/brains/<id>.json. Same contract as
// ActorSpriteRegistry: a typed view for the game plus AddOrReplace/ToJson for the
// Content Editor.
class BrainTemplateRegistry {
public:
  void Load(const std::string& directoryPath);

  const BrainTemplate* Get(const std::string& id) const;
  std::vector<const BrainTemplate*> GetAll() const;
  size_t Count() const { return brains.size(); }

  nlohmann::json ToJson(const BrainTemplate& definition) const;

  bool AddOrReplace(const nlohmann::json& document, const std::string& source,
                    std::string* error, std::vector<std::string>* warnings);

  static std::string IdFor(const nlohmann::json& document);

  const std::string& GetDirectory() const { return directory; }

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  std::map<std::string, BrainTemplate> brains;
  std::string directory;
  DebugLog* debugLog = nullptr;
};
