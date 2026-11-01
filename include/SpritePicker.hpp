#pragma once

#include "SpriteDraw.hpp"

#include "imgui.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

// A wrapped grid of sprite thumbnails, shared by the two places that choose one.
//
// It exists because the settings window's actor-sprite picker and the Content Editor's
// variant chooser answer the same question -- "which of these pictures do I want" -- and
// had grown two different layouts: a grid and a list of rows. One drawing function keeps
// them the same size, the same selection emphasis and the same tooltip, and means a fix
// to one is a fix to both.
//
// Deliberately not a widget class: it keeps no state. The caller owns the model (which
// items exist, which is selected, what a click means) and this only reports the index a
// cell was clicked at. Selection is therefore never guessed here.
namespace SpritePicker {

struct Item {
  // Tooltip title, and the visible label when `texture` is null.
  std::string label;
  // Second tooltip line: a path, a description, or nothing.
  std::string description;
  // Null draws `placeholder` instead: a catalogue entry can exist before its art does.
  const Texture2D *texture = nullptr;
  bool selected = false;
  // Shown in the cell when there is no texture. "?" reads as missing art; "race" as a
  // deliberate default.
  std::string placeholder = "?";
};

struct Options {
  float cell = 52.0f;
  // Height cap, in rows. The grid scrolls past it rather than pushing the rest of the
  // window down.
  int rows = 3;
  // Unique per call: two grids on one window would share scroll state otherwise.
  const char *childId = "##sprite-picker";
  // Right-click entries for the cell at `index`, drawn inside the item's own menu. Use
  // MenuItem; the menu closes itself. Empty means no context menu.
  std::function<void(int index)> contextMenu;
};

// Draws the grid and returns the index of a left-clicked cell, or -1. A left click is the
// primary action; anything destructive belongs in `contextMenu`.
inline int Draw(const std::vector<Item> &items, const Options &options) {
  const ImGuiStyle &style = ImGui::GetStyle();
  const float spacing = style.ItemSpacing.x;
  const float available = ImGui::GetContentRegionAvail().x;
  const int columns = std::max(
      1, static_cast<int>((available + spacing) / (options.cell + spacing)));

  int clicked = -1;
  ImGui::BeginChild(options.childId,
                    ImVec2(0.0f, options.rows * (options.cell + spacing) + 4.0f),
                    ImGuiChildFlags_Borders);
  int index = 0;
  for (const Item &item : items) {
    ImGui::PushID(index);
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##cell", ImVec2(options.cell, options.cell));
    const bool isClicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 bottomRight(corner.x + options.cell, corner.y + options.cell);
    draw->AddRectFilled(corner, bottomRight, IM_COL32(26, 26, 30, 255));

    if (item.texture != nullptr) {
      const SpriteRect fit =
          SpriteFitRect(*item.texture, corner.x, corner.y, options.cell, options.cell);
      draw->AddImage(
          static_cast<ImTextureID>(static_cast<intptr_t>(item.texture->id)),
          ImVec2(fit.x, fit.y), ImVec2(fit.x + fit.width, fit.y + fit.height));
    } else {
      draw->AddText(ImVec2(corner.x + 6.0f, corner.y + options.cell * 0.35f),
                    IM_COL32(150, 150, 150, 255), item.placeholder.c_str());
    }

    const ImU32 border = item.selected   ? IM_COL32(255, 214, 90, 255)
                         : hovered       ? IM_COL32(170, 170, 170, 255)
                                         : IM_COL32(62, 62, 68, 255);
    draw->AddRect(corner, bottomRight, border, 0.0f, 0,
                  item.selected ? 2.0f : 1.0f);

    if (hovered) {
      ImGui::SetTooltip("%s%s%s%s", item.label.c_str(),
                        item.selected ? " (in use)" : "",
                        item.description.empty() ? "" : "\n",
                        item.description.c_str());
    }

    if (options.contextMenu && ImGui::BeginPopupContextItem("##cell-menu")) {
      options.contextMenu(index);
      ImGui::EndPopup();
    }

    ImGui::PopID();
    if (isClicked) {
      clicked = index;
    }
    ++index;
    if (index % columns != 0) {
      ImGui::SameLine();
    }
  }
  ImGui::EndChild();
  return clicked;
}

}  // namespace SpritePicker
