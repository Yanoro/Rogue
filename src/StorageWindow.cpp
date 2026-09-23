#include "StorageWindow.h"
#include "Components.h"
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
  std::string name;
  int count = 0;
};

ImVec4 ToImVec4(const Color &color) {
  return ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                color.a / 255.0f);
}

constexpr float kGlyphSize = 32.0f;

} // namespace

StorageWindow::StorageWindow(flecs::entity container, flecs::entity owner)
    : container(container), owner(owner) {}

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
  // The "###" id keeps ImGui's window state stable across examined containers.
  title += "###StorageWindow";

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
    entry.count = 1;
    entries.push_back(entry);
  });

  const int capacity =
      container.has<Storage>() ? container.get<Storage>()->capacity : 0;
  ImGui::TextDisabled("%d / %d slots, %d distinct", usedSlots, capacity,
                      static_cast<int>(entries.size()));
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

    const std::string glyph = std::string(1, entry.ch) + "##glyph";
    ImGui::Button(glyph.c_str(), ImVec2(kGlyphSize, kGlyphSize));

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
