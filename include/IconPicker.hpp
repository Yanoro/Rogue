#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "IconPaths.hpp"
#include "IconRegistry.h"
#include "ResourceManager.h"
#include "SpritePicker.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"

// The icon chooser shared by Settings (death/corpse icon) and the Content
// Editor's skill form. Like the actor-sprite picker it is a view only; the caller
// owns filter state and decides what a selection means.
namespace IconPicker {

inline std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return text;
}

// Returns the selected icon id. An empty string is a valid result only when
// `allowNone` is true (clear the field / use the name fallback). nullopt means
// no click.
inline std::optional<std::string> Draw(const IconRegistry &icons,
                                       ResourceManager &resources,
                                       const std::string &current,
                                       const char *childId, std::string &filter,
                                       bool allowNone, DebugLog *debugLog) {
  ImGui::SetNextItemWidth(190.0f);
  ImGui::InputTextWithHint("##iconFilter", "filter icons", &filter);
  ImGui::SameLine();
  ImGui::TextDisabled("%s", current.empty()
                                ? (allowNone ? "(none)" : "(missing)")
                                : current.c_str());

  std::vector<SpritePicker::Item> items;
  std::vector<std::string> keys;
  const auto addCell = [&](const std::string &key, const std::string &label,
                           const std::string &description,
                           const std::string &placeholder,
                           const Texture2D *texture, bool selected) {
    SpritePicker::Item item;
    item.label = label;
    item.description = description;
    item.placeholder = placeholder;
    item.texture = texture;
    item.selected = selected;
    items.push_back(std::move(item));
    keys.push_back(key);
  };

  if (allowNone) {
    addCell(std::string(), "None", "No icon; use the text fallback.", "none",
            nullptr, current.empty());
  }

  const std::string needle = Lower(filter);
  const auto matches = [&](const IconDef &definition) {
    if (needle.empty()) return true;
    return Lower(definition.id).find(needle) != std::string::npos ||
           Lower(definition.name).find(needle) != std::string::npos ||
           Lower(definition.description).find(needle) != std::string::npos;
  };

  for (const IconDef *definition : icons.GetAll()) {
    if (!matches(*definition)) continue;
    const Texture2D *texture =
        IconPaths::ResolveIconTexture(icons, resources, definition->id, debugLog);
    addCell(definition->id,
            definition->name.empty() ? definition->id : definition->name,
            definition->description, "?", texture,
            current == definition->id);
  }

  SpritePicker::Options options;
  options.cell = 48.0f;
  options.rows = 3;
  options.childId = childId;
  const int clicked = SpritePicker::Draw(items, options);
  if (clicked >= 0) {
    return keys[static_cast<size_t>(clicked)];
  }
  if (items.empty()) {
    ImGui::TextDisabled("No icons match.");
  }
  return std::nullopt;
}

}  // namespace IconPicker
