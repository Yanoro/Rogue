#include "StorageWindow.h"
#include "AsciiArtDraw.hpp"
#include "SpriteDraw.hpp"
#include "Components.h"
#include "ResourceManager.h"
#include "StringUtils.hpp"
#include "imgui.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

// One row per *distinct* item, not per entity: a chest holding five apples
// shows a single "Apple (5)" entry. The glyph and colors come from the first
// instance seen, so the row looks like the item does on the map.
struct StorageEntry {
  char ch = '?';
  Color characterColor = {255, 255, 255, 255};
  Color backgroundColor = {0, 0, 0, 0};
  // Copied, not referenced: the rows are rebuilt from the live ECS every frame
  // and the item entity can be destroyed in between.
  AsciiArt art;
  bool hasArt = false;
  // The item's raster sprite, when it has one. Also copied for the same reason,
  // and resolved once per rebuild rather than per draw so the row keeps looking
  // like the item even if the entity goes away.
  const Texture2D *sprite = nullptr;
  std::string name;
  int count = 0;
};

ImVec4 ToImVec4(const Color &color) {
  return ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                color.a / 255.0f);
}

constexpr float kGlyphSize = 32.0f;

} // namespace

StorageWindow::StorageWindow(flecs::entity container, flecs::entity owner,
                             ResourceManager *resources)
    : container(container), owner(owner), resources(resources) {}

void StorageWindow::Draw() {
  // The chest can be destroyed (map reload, editor delete) while the window is
  // open; closing here keeps the next frame from reading a dangling entity.
  if (!container.is_alive()) {
    if (owner.is_alive()) {
      owner.remove<ActiveWindow>();
    }
    return;
  }

  std::string title =
      container.has<DisplayName>() ? container.get<DisplayName>()->name
                                   : "Storage";
  // The "###" id keys ImGui's per-window state to this container; including the
  // container id keeps several inventory windows distinct so they can be open at
  // the same time instead of collapsing into one.
  title += "###StorageWindow" + std::to_string(container.id());

  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(300, 260), ImGuiCond_FirstUseEver);
  ImGui::Begin(title.c_str(), &isOpen, ImGuiWindowFlags_None);

  if (!isOpen) {
    if (owner.is_alive()) {
      owner.remove<ActiveWindow>();
    }
    ImGui::End();
    return;
  }

  // Aggregate by display name, case-insensitively and in first-seen order, the
  // same rule StringUtils::StackNames uses for the [INVENTORY] and chest-examine
  // text. Capacity is counted per entity, since each holds one slot.
  std::vector<StorageEntry> entries;
  int usedSlots = 0;
  container.each<Holds>([&](flecs::entity item) {
    if (!item.is_alive()) {
      return;
    }
    usedSlots++;

    std::string name =
        item.has<DisplayName>() ? item.get<DisplayName>()->name : "Item";
    auto existing = std::find_if(
        entries.begin(), entries.end(), [&name](const StorageEntry &entry) {
          return StringUtils::EqualsIgnoreCase(entry.name, name);
        });
    if (existing != entries.end()) {
      existing->count++;
      return;
    }

    StorageEntry entry;
    entry.name = name;
    if (const DrawAscii *ascii = item.get<DrawAscii>()) {
      entry.ch = ascii->ch;
      entry.characterColor = ascii->characterColor;
      entry.backgroundColor = ascii->backgroundColor;
    }
    if (const AsciiArt *art = item.get<AsciiArt>()) {
      entry.hasArt = !art->lines.empty();
      entry.art = *art;
    }
    // The item's sprite, if it was given one at spawn. The path in the component
    // was already validated when it was attached, so this is a cache hit; the
    // null check is only for an item spawned while no manager was set.
    if (resources != nullptr) {
      if (const Sprite *sprite = item.get<Sprite>()) {
        entry.sprite = resources->TryGetTexture(sprite->path);
      }
    }
    entry.count = 1;
    entries.push_back(entry);
  });

  // A character has no Storage component, only containers do, so the capacity
  // is only meaningful there. Showing "N / 0 slots" for an NPC would be wrong.
  if (const Storage *storage = container.get<Storage>()) {
    ImGui::TextDisabled("%d / %d slots, %d distinct", usedSlots,
                        storage->capacity, static_cast<int>(entries.size()));
  } else {
    ImGui::TextDisabled("%d item(s), %d distinct", usedSlots,
                        static_cast<int>(entries.size()));
  }
  ImGui::Separator();

  if (entries.empty()) {
    ImGui::TextDisabled("Empty");
    ImGui::End();
    return;
  }

  // Scroll region so a container with many distinct items stays inside a small
  // window instead of being clipped.
  ImGui::BeginChild("##StorageContents", ImVec2(0, 0));
  for (size_t i = 0; i < entries.size(); ++i) {
    const StorageEntry &entry = entries[i];

    ImVec4 background = ToImVec4(entry.backgroundColor);
    if (background.w <= 0.0f) {
      // Item backgrounds are usually fully transparent (on the map they let the
      // tile show through); in a window that leaves a floating glyph, so give
      // the cell a neutral plate instead.
      background = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    }

    ImGui::PushID(static_cast<int>(i));
    ImGui::PushStyleColor(ImGuiCol_Button, background);
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(entry.characterColor));

    // A sprite or an art block draws its own pixels over the plate; anything else
    // keeps the single-glyph label. The button is only a coloured plate either
    // way -- its return value is deliberately unused here.
    if (entry.sprite != nullptr) {
      ImGui::Button("##glyph", ImVec2(kGlyphSize, kGlyphSize));
      DrawSpriteImGui(*entry.sprite, ImGui::GetItemRectMin(),
                      ImGui::GetItemRectSize());
    } else if (entry.hasArt) {
      ImGui::Button("##glyph", ImVec2(kGlyphSize, kGlyphSize));
      DrawAsciiArtImGui(entry.art, entry.characterColor,
                        ImGui::GetItemRectMin(), ImGui::GetItemRectSize());
    } else {
      const std::string glyph = std::string(1, entry.ch) + "##glyph";
      ImGui::Button(glyph.c_str(), ImVec2(kGlyphSize, kGlyphSize));
    }

    ImGui::PopStyleColor(2);
    ImGui::PopID();

    ImGui::SameLine();
    std::string label = entry.name;
    if (entry.count > 1) {
      label += " (" + std::to_string(entry.count) + ")";
    }
    ImGui::TextUnformatted(label.c_str());
  }
  ImGui::EndChild();

  ImGui::End();
}
