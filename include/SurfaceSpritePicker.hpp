#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "ResourceManager.h"
#include "SurfaceSpriteRegistry.h"
#include "SpritePaths.hpp"
#include "SpritePicker.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"

// The surface-sprite chooser used by the Content Editor's surface definition
// form. A surface sprite is reusable art; the definition still owns the
// mechanics, timers and triggers.
namespace SurfaceSpritePicker {

inline std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return text;
}

inline std::optional<std::string> Draw(const SurfaceSpriteRegistry &sprites,
                                       ResourceManager &resources,
                                       const std::string &current,
                                       const char *childId, std::string &filter) {
  ImGui::SetNextItemWidth(190.0f);
  ImGui::InputTextWithHint("##surfaceSpriteFilter", "filter surfaces", &filter);
  ImGui::SameLine();
  ImGui::TextDisabled("%s", current.empty() ? "(none)" : current.c_str());

  std::vector<SpritePicker::Item> items;
  std::vector<std::string> keys;
  const auto addCell = [&](const std::string &key, const std::string &label,
                           const std::string &description,
                           const std::string &placeholder,
                           const Texture2D *texture) {
    SpritePicker::Item item;
    item.label = label;
    item.description = description;
    item.placeholder = placeholder;
    item.texture = texture;
    item.selected = (current == key);
    items.push_back(std::move(item));
    keys.push_back(key);
  };

  addCell(std::string(), "None", "No sprite; the definition id is the convention.",
          "none", nullptr);

  const std::string needle = Lower(filter);
  for (const SurfaceSpriteDef *definition : sprites.GetAll()) {
    if (!needle.empty() &&
        Lower(definition->id).find(needle) == std::string::npos &&
        Lower(definition->name).find(needle) == std::string::npos &&
        Lower(definition->description).find(needle) == std::string::npos) {
      continue;
    }
    const Texture2D *texture =
        ResolveSpriteTexture(std::string(), definition->id, resources, nullptr);
    addCell(definition->id,
            definition->name.empty() ? definition->id : definition->name,
            definition->description, "?", texture);
  }

  SpritePicker::Options options;
  options.cell = 48.0f;
  options.rows = 3;
  options.childId = childId;
  const int clicked = SpritePicker::Draw(items, options);
  if (clicked >= 0) return keys[static_cast<size_t>(clicked)];
  if (items.size() == 1) {
    ImGui::TextDisabled("No surface sprites yet. Create one in the Designer's "
                        "Surface sprite mode.");
  }
  return std::nullopt;
}

}  // namespace SurfaceSpritePicker
