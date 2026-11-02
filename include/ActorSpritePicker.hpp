#pragma once

// The actor-sprite chooser shared by Settings -> Player and the Content Editor's
// NPC template form.
//
// The two callers answer the same question -- "which of these pictures do I
// want" -- so they draw the same filter, the same "without art" toggle, the same
// Race default cell and the same thumbnail grid. Only the result handling
// differs: Settings applies it to the player immediately, the Content Editor
// writes it into the working document and waits for Save.
//
// Header-only because it is a pure view helper with no state of its own; the
// filter and checkbox state belong to the caller.

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "ActorSpriteRegistry.h"
#include "ResourceManager.h"
#include "SpritePaths.hpp"
#include "SpritePicker.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"

namespace ActorSpritePicker {

inline std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return text;
}

// Draws the picker. Returns the selected key when a cell was clicked:
//  * a non-empty ActorSpriteRegistry id selects that sprite;
//  * an empty string selects Race default.
// Returns nullopt when nothing was clicked.
//
// `filter` and `showWithoutArt` are caller-owned so each window keeps its own
// session state, exactly as the Settings window did before this was shared.
inline std::optional<std::string> Draw(const ActorSpriteRegistry &sprites,
                                       ResourceManager &resources,
                                       const std::string &current,
                                       const char *childId, std::string &filter,
                                       bool &showWithoutArt) {
  ImGui::SetNextItemWidth(190.0f);
  ImGui::InputTextWithHint("##filter", "filter sprites", &filter);
  ImGui::SameLine();
  ImGui::Checkbox("without art", &showWithoutArt);
  ImGui::SameLine();
  ImGui::TextDisabled("%s", current.empty() ? "(race default)" : current.c_str());

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

  addCell(std::string(), "Race default",
          "The race's own look, and then the ASCII art.", "race", nullptr);

  const std::string needle = Lower(filter);
  const auto matches = [&needle](const std::string &id, const std::string &name,
                                 const std::string &description) {
    if (needle.empty()) return true;
    return Lower(id).find(needle) != std::string::npos ||
           Lower(name).find(needle) != std::string::npos ||
           Lower(description).find(needle) != std::string::npos;
  };

  int shown = 0;
  for (const ActorSpriteDef *definition : sprites.GetAll()) {
    const std::string name =
        definition->name.empty() ? definition->id : definition->name;
    if (!matches(definition->id, name, definition->description)) continue;
    const Texture2D *texture =
        ResolveSpriteTexture(std::string(), definition->id, resources, nullptr);
    if (texture == nullptr && !showWithoutArt) continue;
    addCell(definition->id, name, definition->description, "?", texture);
    ++shown;
  }

  SpritePicker::Options options;
  options.cell = 52.0f;
  options.rows = 3;
  options.childId = childId;
  const int clicked = SpritePicker::Draw(items, options);
  if (clicked >= 0) {
    return keys[static_cast<size_t>(clicked)];
  }

  if (shown == 0) {
    if (sprites.Count() == 0) {
      ImGui::TextDisabled("No actor sprites are registered (data/actor_sprites).");
    } else if (filter.empty() && !showWithoutArt) {
      ImGui::TextDisabled(
          "No actor sprite has art yet. Generate them in the Content Editor's "
          "actor sprites, or tick \"without art\".");
    } else {
      ImGui::TextDisabled("No actor sprites match.");
    }
  }
  return std::nullopt;
}

}  // namespace ActorSpritePicker
