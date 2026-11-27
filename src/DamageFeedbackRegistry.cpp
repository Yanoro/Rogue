#include "DamageFeedbackRegistry.h"

#include "DebugLog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace {
std::string Slugify(const std::string &text) {
  std::string out;
  bool lastUnderscore = false;
  for (unsigned char c : text) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      lastUnderscore = false;
    } else if (!out.empty() && !lastUnderscore) {
      out.push_back('_');
      lastUnderscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}
bool Parse(const nlohmann::json &doc, DamageFeedbackDef &out, std::string *error,
           std::vector<std::string> *warnings) {
  if (!doc.is_object()) {
    if (error) *error = "damage feedback is not an object";
    return false;
  }
  out.document = doc;
  out.name = doc.value("name", std::string());
  out.id = Slugify(out.name);
  if (out.id.empty()) {
    if (error) *error = "damage feedback has no name";
    return false;
  }
  out.description = doc.value("description", std::string());
  out.flashEnabled = doc.value("flashEnabled", true);
  out.flashColor = doc.value("flashColor", std::string("#FF3C3CFF"));
  out.flashStyle = doc.value("flashStyle", std::string("silhouette"));
  out.flashSeconds = std::max(0.0f, doc.value("flashSeconds", 0.12f));
  out.shakeEnabled = doc.value("shakeEnabled", true);
  out.shakeMagnitude = std::max(0.0f, doc.value("shakeMagnitude", 3.0f));
  out.shakeSeconds = std::max(0.0f, doc.value("shakeSeconds", 0.14f));
  out.hitClip = doc.value("hitClip", std::string("hit"));
  (void)warnings;
  return true;
}
}  // namespace

void DamageFeedbackRegistry::Load(const std::string &directoryPath) {
  definitions.clear();
  if (directoryPath.empty()) return;
  std::error_code ec;
  const std::filesystem::path root(directoryPath);
  if (!std::filesystem::exists(root, ec)) return;
  for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec || !entry.is_regular_file() ||
        entry.path().extension() != ".json") continue;
    std::ifstream input(entry.path());
    if (!input) continue;
    nlohmann::json doc;
    try { input >> doc; } catch (...) { continue; }
    DamageFeedbackDef parsed;
    std::string error;
    if (!Parse(doc, parsed, &error, nullptr)) {
      if (debugLog) debugLog->LogWarning("Damage feedback '" +
                                         entry.path().string() + "': " + error);
      continue;
    }
    definitions[parsed.id] = std::move(parsed);
  }
}
const DamageFeedbackDef *DamageFeedbackRegistry::Get(const std::string &id) const {
  const auto found = definitions.find(id);
  return found == definitions.end() ? nullptr : &found->second;
}
std::vector<const DamageFeedbackDef *> DamageFeedbackRegistry::GetAll() const {
  std::vector<const DamageFeedbackDef *> out;
  for (const auto &[id, def] : definitions) { (void)id; out.push_back(&def); }
  return out;
}
nlohmann::json DamageFeedbackRegistry::ToJson(const std::string &id) const {
  const DamageFeedbackDef *def = Get(id);
  return def ? def->document : nlohmann::json::object();
}
bool DamageFeedbackRegistry::AddOrReplace(const nlohmann::json &document,
                                          const std::string &source,
                                          std::string *error,
                                          std::vector<std::string> *warnings) {
  (void)source;
  DamageFeedbackDef parsed;
  if (!Parse(document, parsed, error, warnings)) return false;
  definitions[parsed.id] = std::move(parsed);
  return true;
}
std::string DamageFeedbackRegistry::IdFor(const nlohmann::json &document) {
  return Slugify(document.value("name", std::string()));
}
