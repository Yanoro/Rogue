#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class DebugLog;

// One slot anchor on a body silhouette. Coordinates are normalised to the
// silhouette image's canvas (0..1), so they survive window resizing.
struct BodySlotAnchor {
  std::string slot;  // slot id ("main_hand") or accepts kind ("hand")
  float x = 0.5f;
  float y = 0.5f;
  float size = 0.15f;
};

struct BodySilhouetteDef {
  std::string id;
  std::string name;
  std::string description;
  // IconRegistry id for the transparent silhouette.
  std::string icon;
  std::vector<BodySlotAnchor> anchors;
};

// Authored body paper-doll layouts. A silhouette's anchors are relative to its
// own art, not to the inventory panel, which is what lets a designed body put
// the head, hands and feet where they actually belong.
class BodySilhouetteRegistry {
public:
  void Load(const std::string &directoryPath);

  const BodySilhouetteDef *Get(const std::string &id) const;
  std::vector<const BodySilhouetteDef *> GetAll() const;
  size_t Count() const { return silhouettes.size(); }

  nlohmann::json ToJson(const BodySilhouetteDef &definition) const;
  bool AddOrReplace(const nlohmann::json &document, const std::string &source,
                    std::string *error, std::vector<std::string> *warnings);
  static std::string IdFor(const nlohmann::json &document);

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, BodySilhouetteDef> silhouettes;
  std::vector<std::string> order;
  DebugLog *debugLog = nullptr;
};
