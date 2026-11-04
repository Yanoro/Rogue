#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Components.h"

// One resource row in the player portrait. Stored as JSON in
// hud.portrait.resources so the settings file stays readable and forward
// compatible when a race gains a resource.
struct PortraitResourceEntry {
  std::string id;
  bool visible = true;
  bool showIcon = true;
};

namespace PortraitResources {

inline std::vector<PortraitResourceEntry> Parse(const nlohmann::json &value) {
  std::vector<PortraitResourceEntry> out;
  if (!value.is_array()) return out;
  for (const auto &item : value) {
    if (!item.is_object()) continue;
    PortraitResourceEntry entry;
    entry.id = item.value("id", std::string());
    if (entry.id.empty()) continue;
    entry.visible = item.value("visible", true);
    entry.showIcon = item.value("showIcon", true);
    out.push_back(std::move(entry));
  }
  return out;
}

inline nlohmann::json ToJson(const std::vector<PortraitResourceEntry> &entries) {
  nlohmann::json out = nlohmann::json::array();
  for (const PortraitResourceEntry &entry : entries) {
    out.push_back({{"id", entry.id},
                   {"visible", entry.visible},
                   {"showIcon", entry.showIcon}});
  }
  return out;
}

// Returns one row per current resource pool, in configured order. A resource
// missing from the configuration is appended in pool order; a configured entry
// the character no longer has is omitted. An empty configuration means
// "everything visible, in registry/pool order".
inline std::vector<PortraitResourceEntry> Build(
    const ResourcePools *pools, const nlohmann::json &configured) {
  std::vector<PortraitResourceEntry> out;
  if (pools == nullptr) return out;
  const std::vector<PortraitResourceEntry> config = Parse(configured);
  if (config.empty()) {
    for (const ResourcePool &pool : pools->pools) {
      out.push_back({pool.id, true, true});
    }
    return out;
  }
  for (const PortraitResourceEntry &entry : config) {
    if (pools->Find(entry.id) == nullptr) continue;
    out.push_back(entry);
  }
  for (const ResourcePool &pool : pools->pools) {
    if (std::find_if(out.begin(), out.end(),
                     [&](const PortraitResourceEntry &entry) {
                       return entry.id == pool.id;
                     }) != out.end()) {
      continue;
    }
    out.push_back({pool.id, true, true});
  }
  return out;
}

}  // namespace PortraitResources
