#include "IconRegistry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "DebugLog.h"

namespace {

std::string Slugify(const std::string &text) {
  std::string out;
  bool lastWasSeparator = false;
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) {
      out.push_back(static_cast<char>(std::tolower(u)));
      lastWasSeparator = false;
    } else if (!out.empty() && !lastWasSeparator) {
      out.push_back('_');
      lastWasSeparator = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

}  // namespace

std::string IconRegistry::IdFor(const nlohmann::json &document) {
  return Slugify(document.value("name", std::string()));
}

void IconRegistry::Load(const std::string &directoryPath) {
  if (directoryPath.empty()) return;
  std::error_code ec;
  if (!std::filesystem::is_directory(directoryPath, ec)) return;

  for (const auto &entry : std::filesystem::directory_iterator(directoryPath, ec)) {
    if (entry.path().extension() != ".json") continue;
    std::ifstream file(entry.path());
    if (!file.is_open()) continue;
    nlohmann::json document;
    try {
      file >> document;
    } catch (const std::exception &error) {
      if (debugLog) {
        debugLog->LogError("Icons: " + entry.path().string() + " is not JSON: " +
                           error.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, entry.path().string(), &error, &warnings)) {
      if (debugLog) {
        debugLog->LogError("Icons: " + entry.path().string() + " refused: " + error);
      }
      continue;
    }
    if (debugLog) {
      for (const std::string &warning : warnings) {
        debugLog->LogWarning("Icons: " + warning);
      }
    }
  }
}

bool IconRegistry::AddOrReplace(const nlohmann::json &document,
                                const std::string &source, std::string *error,
                                std::vector<std::string> *warnings) {
  (void)source;
  (void)warnings;
  if (!document.is_object()) {
    if (error != nullptr) *error = "the entry is not a JSON object";
    return false;
  }
  IconDef definition;
  definition.id = IdFor(document);
  if (definition.id.empty()) {
    if (error != nullptr) *error = "the entry has no \"name\", so it has no id";
    return false;
  }
  definition.name = document.value("name", definition.id);
  definition.description = document.value("description", std::string());
  definition.sprite = document.value("sprite", std::string());
  definition.style = document.value("style", std::string());
  icons[definition.id] = std::move(definition);
  return true;
}

const IconDef *IconRegistry::Get(const std::string &id) const {
  auto it = icons.find(id);
  return it == icons.end() ? nullptr : &it->second;
}

std::vector<const IconDef *> IconRegistry::GetAll() const {
  std::vector<const IconDef *> defs;
  defs.reserve(icons.size());
  for (const auto &pair : icons) defs.push_back(&pair.second);
  std::sort(defs.begin(), defs.end(), [](const IconDef *a, const IconDef *b) {
    return a->name < b->name;
  });
  return defs;
}

nlohmann::json IconRegistry::ToJson(const IconDef &definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["name"] = definition.name;
  if (!definition.description.empty()) {
    document["description"] = definition.description;
  }
  if (!definition.sprite.empty()) {
    document["sprite"] = definition.sprite;
  }
  document["style"] = definition.style;
  return document;
}
