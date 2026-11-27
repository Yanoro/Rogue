#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class DebugLog;

struct DamageFeedbackDef {
  std::string id;
  std::string name;
  std::string description;
  bool flashEnabled = true;
  std::string flashColor = "#FF3C3CFF";
  std::string flashStyle = "silhouette";
  float flashSeconds = 0.12f;
  bool shakeEnabled = true;
  float shakeMagnitude = 3.0f;
  float shakeSeconds = 0.14f;
  std::string hitClip = "hit";
  nlohmann::json document;
};

class DamageFeedbackRegistry {
public:
  void Load(const std::string &directoryPath);
  const DamageFeedbackDef *Get(const std::string &id) const;
  std::vector<const DamageFeedbackDef *> GetAll() const;
  size_t Count() const { return definitions.size(); }
  nlohmann::json ToJson(const std::string &id) const;
  bool AddOrReplace(const nlohmann::json &document, const std::string &source,
                    std::string *error, std::vector<std::string> *warnings);
  static std::string IdFor(const nlohmann::json &document);
  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::map<std::string, DamageFeedbackDef> definitions;
  DebugLog *debugLog = nullptr;
};
