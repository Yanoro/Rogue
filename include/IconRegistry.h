#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class DebugLog;

// One named icon: a picture with an identity, not a mechanic.
//
// Icons are the UI vocabulary -- skill buttons, the death/corpse marker, the
// missing-asset interrogation mark, and future status/command glyphs. The
// sprite is optional because a newly created icon may not have art yet; an
// empty sprite resolves through the icon id as its sprite convention, and a
// failed resolution falls back to the fixed "missing" entry.
struct IconDef {
  std::string id;
  std::string name;
  std::string description;
  // Explicit path or sprite convention id. Empty means "use this icon's id as
  // the convention id", which is what lets the Content Editor generate `<id>.png`.
  std::string sprite;
};

class IconRegistry {
public:
  void Load(const std::string &directoryPath);

  const IconDef *Get(const std::string &id) const;
  std::vector<const IconDef *> GetAll() const;
  size_t Count() const { return icons.size(); }

  // The document as the Content Editor edits it.
  nlohmann::json ToJson(const IconDef &definition) const;

  // Validates and stores `document`, replacing any entry with the same id.
  bool AddOrReplace(const nlohmann::json &document, const std::string &source,
                    std::string *error, std::vector<std::string> *warnings);

  // The id a document is stored under: the slug of its name. This is also the
  // default sprite convention when `sprite` is empty.
  static std::string IdFor(const nlohmann::json &document);

  void SetDebugLog(DebugLog *log) { debugLog = log; }

private:
  std::unordered_map<std::string, IconDef> icons;
  DebugLog *debugLog = nullptr;
};
