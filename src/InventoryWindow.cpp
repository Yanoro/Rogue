#define IMGUI_DEFINE_MATH_OPERATORS
#include "InventoryWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Components.h"
#include "EquipmentRuntime.hpp"
#include "Game.h"
#include "IconPaths.hpp"
#include "InventoryDrag.hpp"
#include "InventorySlotDraw.hpp"
#include "InventoryView.hpp"
#include "ItemRegistry.h"
#include "RaceRegistry.h"
#include "ResourceManager.h"
#include "ResourceSystem.h"
#include "Settings.h"
#include "SpritePaths.hpp"
#include "imgui.h"

namespace {
struct SlotLayout {
  const char *id;
  const char *accepts;
  const char *icon;
  float x;
  float y;
  float size;
};

// One hardcoded table. IDs win over accepts, so main_hand/off_hand and ring_1/2
// can sit on opposite sides while a race that only says "hand" still gets a
// sensible default.
const SlotLayout kSlotLayouts[] = {
    {"head", "head", "slot_head", 0.50f, 0.07f, 0.18f},
    {"neck", "neck", "slot_neck", 0.50f, 0.17f, 0.16f},
    {"torso", "torso", "slot_torso", 0.50f, 0.34f, 0.22f},
    {"back", "back", "slot_back", 0.74f, 0.31f, 0.19f},
    {"main_hand", "hand", "slot_hand", 0.20f, 0.48f, 0.18f},
    {"off_hand", "hand", "slot_hand", 0.80f, 0.48f, 0.18f},
    {"hand", "hand", "slot_hand", 0.50f, 0.47f, 0.16f},
    {"ring_1", "ring", "slot_ring", 0.30f, 0.59f, 0.14f},
    {"ring_2", "ring", "slot_ring", 0.70f, 0.59f, 0.14f},
    {"ring", "ring", "slot_ring", 0.50f, 0.59f, 0.14f},
    {"legs", "legs", "slot_legs", 0.50f, 0.70f, 0.22f},
    {"feet", "feet", "slot_feet", 0.50f, 0.85f, 0.18f},
};

const char *IconForAccepts(const std::string &accepts) {
  for (const SlotLayout &layout : kSlotLayouts) {
    if (accepts == layout.accepts) return layout.icon;
  }
  return "slot_hand";
}

const BodySlotAnchor *AnchorFor(const BodySilhouetteDef *silhouette,
                               const SlotSpec &slot) {
  if (silhouette == nullptr) return nullptr;
  for (const BodySlotAnchor &anchor : silhouette->anchors) {
    if (anchor.slot == slot.id) return &anchor;
  }
  for (const BodySlotAnchor &anchor : silhouette->anchors) {
    if (anchor.slot == slot.accepts) return &anchor;
  }
  return nullptr;
}

const SlotLayout *LayoutFor(const SlotSpec &slot) {
  for (const SlotLayout &layout : kSlotLayouts) {
    if (slot.id == layout.id) return &layout;
  }
  for (const SlotLayout &layout : kSlotLayouts) {
    if (slot.accepts == layout.accepts) return &layout;
  }
  return nullptr;
}

const Texture2D *ItemTexture(flecs::entity item, ResourceManager &resources) {
  if (!item.is_alive()) return nullptr;
  if (const Sprite *sprite = item.get<Sprite>()) {
    return resources.GetTexture(sprite->path);
  }
  return nullptr;
}

std::string ItemCountLabel(const InventoryCell &cell) {
  if (!cell.stackable || cell.count <= 1) return {};
  return std::to_string(cell.count);
}
}

InventoryWindow::InventoryWindow(Game *game) : game(game) {}

void InventoryWindow::Draw() {
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
  ImGui::Begin("Inventory", &open, ImGuiWindowFlags_None);
  if (!open) {
    if (game != nullptr) game->inventoryWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }
  if (game == nullptr || !game->playerEntity.is_alive()) {
    ImGui::TextDisabled("The player entity is not available.");
    ImGui::End();
    return;
  }

  Resources::EnsureInitialized(game->playerEntity);
  const int capacity = InventoryCapacity(
      game->playerEntity, Settings().player.inventorySlots);
  const int pageCount = std::max(1, (capacity + 29) / 30);
  page = std::clamp(page, 0, pageCount - 1);

  DrawPageControls(pageCount);
  ImGui::Separator();

  ImGui::BeginChild("##inventory_grid_panel", ImVec2(460, 0),
                    ImGuiChildFlags_Borders);
  DrawGrid();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##inventory_body_panel", ImVec2(0, 0),
                    ImGuiChildFlags_Borders);
  DrawPaperDoll();
  ImGui::EndChild();

  if (!status.empty()) {
    ImGui::Separator();
    ImGui::TextWrapped("%s", status.c_str());
  }

  ImGui::End();
}

void InventoryWindow::DrawPageControls(int pageCount) {
  if (pageCount > 1) {
    if (ImGui::Button("<") && page > 0) --page;
    ImGui::SameLine();
    ImGui::Text("Page %d / %d", page + 1, pageCount);
    ImGui::SameLine();
    if (ImGui::Button(">") && page + 1 < pageCount) ++page;
    ImGui::SameLine();
  }
  ImGui::TextDisabled("Double-click equipment to equip; double-click a body slot to take it off.");
}

void InventoryWindow::DrawGrid() {
  const int capacity = InventoryCapacity(
      game->playerEntity, Settings().player.inventorySlots);
  // Cells placed in the holder's remembered slots, not in Holds order: that is what makes a
  // drop target a specific empty slot and a reorder stick.
  SlottedInventory view =
      BuildSlottedInventory(game->playerEntity, &game->itemRegistry, capacity);
  const int first = page * 30;

  // The shared cell plate (visual asset "inventory_slot"). Null when the asset has no art
  // yet, which the helper answers with the flat rectangle the window used before.
  const Texture2D *slotPlate = IconPaths::TryResolveIconTexture(
      game->iconRegistry, game->resourceManager, "inventory_slot",
      game->debugLog.get());

  // Drops land on one named slot. An occupied slot refuses rather than swapping, and a drop
  // from this same window is the reorder: the cell moves to the empty slot and nothing is
  // written to Holds.
  const auto acceptDroppedCell = [this, &view](int slot) {
    const InventoryDrag::State &drag = InventoryDrag::Active();
    if (!drag.active) return;
    if (slot < 0 || slot >= view.slotCount) {
      InventoryDrag::Clear();
      return;
    }
    if (view.slotToCell[static_cast<size_t>(slot)] >= 0) {
      status = "That slot is occupied.";
      InventoryDrag::Clear();
      return;
    }
    if (drag.source == game->playerEntity) {
      PlaceItemsAtSlot(game->playerEntity, drag.items, slot);
      status = "Moved " + drag.name + ".";
      InventoryDrag::Clear();
      return;
    }
    // The slot is empty, so the cell fits by definition; the move is all-or-nothing because
    // a slot is the unit.
    MoveHeldItems(drag.source, game->playerEntity, drag.items);
    PlaceItemsAtSlot(game->playerEntity, drag.items, slot);
    status = "Took " + drag.name + ".";
    if (game->debugLog) game->debugLog->LogInfo(status);
    InventoryDrag::Clear();
  };

  constexpr int kColumns = 10;
  constexpr float kCell = 42.0f;
  ImGui::BeginTable("##inventory_grid", kColumns,
                    ImGuiTableFlags_SizingFixedFit |
                        ImGuiTableFlags_NoSavedSettings |
                        ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_BordersInnerH);
  for (int col = 0; col < kColumns; ++col) {
    ImGui::TableSetupColumn("##slot_col", ImGuiTableColumnFlags_WidthFixed,
                            kCell + 4.0f);
  }
  for (int row = 0; row < 3; ++row) {
    ImGui::TableNextRow();
    for (int col = 0; col < kColumns; ++col) {
      ImGui::TableSetColumnIndex(col);
      ImGui::PushID(row * kColumns + col);
      const int index = first + row * kColumns + col;
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
      if (index >= capacity) {
        const ImVec2 cellMin = ImGui::GetCursorScreenPos();
        DrawInventorySlotPlate(ImGui::GetWindowDrawList(), cellMin, kCell,
                               slotPlate, /*available=*/false);
        if (const Texture2D *locked = IconPaths::ResolveIconTexture(
                game->iconRegistry, game->resourceManager, "inventory_locked",
                game->debugLog.get())) {
          ImGui::Image(static_cast<ImTextureID>(
                           static_cast<intptr_t>(locked->id)),
                       ImVec2(kCell, kCell), ImVec2(0, 0), ImVec2(1, 1),
                       ImVec4(1, 1, 1, 0.55f), ImVec4(0, 0, 0, 0));
        } else {
          ImGui::Dummy(ImVec2(kCell, kCell));
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
        continue;
      }

      const int slotCell =
          index < view.slotCount ? view.slotToCell[static_cast<size_t>(index)] : -1;
      InventoryCell *cell =
          slotCell >= 0 ? &view.cells[static_cast<size_t>(slotCell)] : nullptr;
      // Enabled even when empty: an empty cell has to accept a drop, and a disabled ImGui
      // item accepts no payload.
      if (ImGui::InvisibleButton("##cell", ImVec2(kCell, kCell))) {
        // Single click selects nothing yet; double-click is the action below.
      }
      const ImVec2 cellMin = ImGui::GetItemRectMin();
      DrawInventorySlotPlate(ImGui::GetWindowDrawList(), cellMin, kCell, slotPlate,
                             /*available=*/true);
      const bool hovered = ImGui::IsItemHovered();
      const bool doubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                                 hovered;
      if (cell != nullptr) {
        // The drag preview is the sprite alone, centred exactly on the cursor. ImGui's own
        // drag tooltip is suppressed because it anchors to the bottom-right of the mouse;
        // the foreground draw list puts the picture where the pointer is.
        if (ImGui::BeginDragDropSource(
                ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
          InventoryDrag::Begin(game->playerEntity, *cell);
          const uint64_t token = static_cast<uint64_t>(game->playerEntity.id());
          ImGui::SetDragDropPayload(InventoryDrag::kPayload, &token, sizeof(token));
          const Texture2D *texture =
              cell->items.empty()
                  ? nullptr
                  : ItemTexture(cell->items.front(), game->resourceManager);
          if (texture == nullptr) {
            texture = game->resourceManager.TryGetTexture(UnknownSpritePath());
          }
          if (texture != nullptr) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const float half = kCell * 0.5f;
            ImGui::GetForegroundDrawList()->AddImage(
                static_cast<ImTextureID>(static_cast<intptr_t>(texture->id)),
                ImVec2(mouse.x - half, mouse.y - half),
                ImVec2(mouse.x + half, mouse.y + half));
          }
          ImGui::EndDragDropSource();
        }
      }
      // Every real slot accepts a drop; the accept helper decides whether the slot is free.
      if (ImGui::BeginDragDropTarget()) {
        if (ImGui::AcceptDragDropPayload(InventoryDrag::kPayload) != nullptr) {
          acceptDroppedCell(index);
        }
        ImGui::EndDragDropTarget();
      }
      if (cell != nullptr) {
        const ImVec2 min = cellMin;
        if (const Texture2D *texture = ItemTexture(
                cell->items.empty() ? flecs::entity::null() : cell->items.front(),
                game->resourceManager)) {
          ImGui::GetWindowDrawList()->AddImage(
              static_cast<ImTextureID>(static_cast<intptr_t>(texture->id)), min,
              min + ImVec2(kCell, kCell));
        } else {
          ImGui::GetWindowDrawList()->AddText(
              ImVec2(min.x + 3, min.y + 3), IM_COL32(220, 220, 220, 255),
              cell->name.c_str());
        }
        const std::string count = ItemCountLabel(*cell);
        if (!count.empty()) {
          const ImVec2 size = ImGui::CalcTextSize(count.c_str());
          ImGui::GetWindowDrawList()->AddText(
              ImVec2(min.x + kCell - size.x - 2, min.y + kCell - size.y - 1),
              IM_COL32(255, 240, 180, 255), count.c_str());
        }
        if (hovered) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(cell->name.c_str());
          if (cell->stackable && cell->count > 1) {
            ImGui::Text("Count: %d", cell->count);
          }
          if (cell->definition != nullptr) {
            if (cell->definition->damage > 0.0f) {
              ImGui::Text("Damage: %.0f", cell->definition->damage);
            }
            if (cell->definition->armor > 0.0f) {
              ImGui::Text("Armour: %.0f", cell->definition->armor);
            }
            ImGui::TextDisabled("Double-click to equip");
          }
          ImGui::TextDisabled("Drag to a storage window to store it");
          ImGui::EndTooltip();
        }
        if (doubleClicked && !cell->items.empty() && cell->definition != nullptr &&
            !cell->definition->slot.empty()) {
          const EquipOutcome outcome =
              EquipItem(game->playerEntity, cell->items.front(), &game->itemRegistry);
          status = outcome.message;
          if (game->debugLog) game->debugLog->LogInfo(outcome.message);
        }
        // The item definition is editable content; the row opens it in the Content Editor.
        // Drawn after the tooltip on purpose: a tooltip and a popup cannot both be open.
        if (ImGui::BeginPopupContextItem("##item_menu")) {
          ContentEditorMenu::Draw(*game, game->ContentTargetsForItem(cell->itemId),
                                  false);
          ImGui::EndPopup();
        }
      }
      ImGui::PopStyleVar();
      ImGui::PopID();
    }
  }
  ImGui::EndTable();

  if (capacity > 30) {
    ImGui::TextDisabled("Page %d of %d; %d cells total.", page + 1,
                        std::max(1, (capacity + 29) / 30), capacity);
  } else if (capacity < 30) {
    ImGui::TextDisabled("%d of 30 cells available on this page.", capacity);
  }
}

void InventoryWindow::DrawPaperDoll() {
  const EquipmentSlots *body = game->playerEntity.get<EquipmentSlots>();
  if (body == nullptr || body->slots.empty()) {
    ImGui::TextDisabled("This character has no body slots.");
    return;
  }

  const std::string raceId =
      Settings().player.race.empty() ? "human" : Settings().player.race;
  const RaceDef *race = game->raceRegistry.Get(raceId);
  const std::string silhouetteId =
      race != nullptr && !race->silhouetteIcon.empty()
          ? race->silhouetteIcon
          : "body_human";
  const BodySilhouetteDef *silhouette =
      game->bodySilhouetteRegistry.Get(silhouetteId);
  const std::string silhouetteIcon =
      silhouette != nullptr && !silhouette->icon.empty()
          ? silhouette->icon
          : silhouetteId;

  const ImVec2 panelOrigin = ImGui::GetCursorScreenPos();
  const ImVec2 available = ImGui::GetContentRegionAvail();
  const float panelWidth = std::max(220.0f, available.x);
  const float panelHeight = std::max(260.0f, available.y - 8.0f);
  ImDrawList *draw = ImGui::GetWindowDrawList();

  // The silhouette and every slot live in one fixed-aspect canvas. Previously
  // the image was fitted independently of the slot percentages, so a short/wide
  // panel shrank the body while the slots kept spreading across the panel.
  const Texture2D *bodyTexture = IconPaths::ResolveIconTexture(
      game->iconRegistry, game->resourceManager, silhouetteIcon,
      game->debugLog.get());
  const float bodyAspect =
      bodyTexture != nullptr && bodyTexture->height > 0
          ? static_cast<float>(bodyTexture->width) /
                static_cast<float>(bodyTexture->height)
          : 2.0f / 3.0f;
  const float maxBodyWidth = panelWidth * 0.94f;
  const float maxBodyHeight = panelHeight * 0.94f;
  ImVec2 bodySize;
  if (maxBodyWidth / maxBodyHeight > bodyAspect) {
    bodySize = ImVec2(maxBodyHeight * bodyAspect, maxBodyHeight);
  } else {
    bodySize = ImVec2(maxBodyWidth, maxBodyWidth / bodyAspect);
  }
  const ImVec2 bodyOrigin(
      panelOrigin.x + (panelWidth - bodySize.x) * 0.5f,
      panelOrigin.y + (panelHeight - bodySize.y) * 0.5f);

  if (bodyTexture != nullptr) {
    draw->AddImage(static_cast<ImTextureID>(
                       static_cast<intptr_t>(bodyTexture->id)),
                   bodyOrigin, bodyOrigin + bodySize, ImVec2(0, 0),
                   ImVec2(1, 1), IM_COL32(255, 255, 255, 62));
  }

  for (const SlotSpec &slot : body->slots) {
    const BodySlotAnchor *anchor = AnchorFor(silhouette, slot);
    const SlotLayout *layout = LayoutFor(slot);
    if (anchor == nullptr && layout == nullptr) continue;
    const float slotSize =
        bodySize.x * (anchor != nullptr ? anchor->size : layout->size);
    const float slotX = anchor != nullptr ? anchor->x : layout->x;
    const float slotY = anchor != nullptr ? anchor->y : layout->y;
    const ImVec2 size(slotSize, slotSize);
    const ImVec2 at(bodyOrigin.x + bodySize.x * slotX - size.x * 0.5f,
                    bodyOrigin.y + bodySize.y * slotY - size.y * 0.5f);

    ImGui::SetCursorScreenPos(at);
    ImGui::PushID(slot.id.c_str());
    ImGui::InvisibleButton("##slot", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool doubleClicked =
        hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    flecs::entity worn = FindEquippedInSlot(game->playerEntity, slot.id,
                                            &game->itemRegistry);
    if (worn.is_alive()) {
      const Texture2D *texture = ItemTexture(worn, game->resourceManager);
      if (texture != nullptr) {
        draw->AddImage(static_cast<ImTextureID>(
                           static_cast<intptr_t>(texture->id)),
                       at, at + size);
      } else {
        draw->AddRectFilled(at, at + size, IM_COL32(35, 38, 42, 220), 3.0f);
        draw->AddText(ImVec2(at.x + 4, at.y + 4), IM_COL32(230, 230, 230, 255),
                      ItemLabel(worn).c_str());
      }
      if (doubleClicked) {
        status = UnequipItem(game->playerEntity, worn);
        if (game->debugLog) game->debugLog->LogInfo(status);
      }
      // A worn item is still its definition; open it in the Content Editor.
      if (ImGui::BeginPopupContextItem("##worn_menu")) {
        if (const ItemType *type = worn.get<ItemType>()) {
          ContentEditorMenu::Draw(*game, game->ContentTargetsForItem(type->id),
                                  false);
        }
        ImGui::EndPopup();
      }
    } else {
      if (const Texture2D *icon = IconPaths::ResolveIconTexture(
              game->iconRegistry, game->resourceManager, layout->icon,
              game->debugLog.get())) {
        draw->AddImage(static_cast<ImTextureID>(
                           static_cast<intptr_t>(icon->id)),
                       at, at + size, ImVec2(0, 0), ImVec2(1, 1),
                       IM_COL32(255, 255, 255, 105));
      }
      draw->AddRect(at, at + size, IM_COL32(180, 180, 190, 80), 3.0f);
    }
    if (hovered) {
      ImGui::BeginTooltip();
      ImGui::Text("%s", slot.id.c_str());
      ImGui::TextDisabled("Accepts: %s", slot.accepts.c_str());
      if (worn.is_alive()) {
        ImGui::Text("Worn: %s", ItemLabel(worn).c_str());
        if (const Equipped *equipped = worn.get<Equipped>()) {
          ImGui::TextDisabled("Slots: %s", JoinSlotIds(equipped->slots).c_str());
        }
        ImGui::TextDisabled("Double-click to take off");
      } else {
        ImGui::TextDisabled("Empty");
      }
      ImGui::EndTooltip();
    }
    ImGui::PopID();
  }

  // Unknown race slots are auto-arranged down the right edge. They remain fully
  // usable: icon, worn item, tooltip and double-click unequip all behave like a
  // known slot. The icon falls back to the hand silhouette when the accepts kind
  // has no dedicated icon.
  int fallback = 0;
  for (const SlotSpec &slot : body->slots) {
    if (AnchorFor(silhouette, slot) != nullptr || LayoutFor(slot) != nullptr) {
      continue;
    }
    const float size = bodySize.x * 0.13f;
    const ImVec2 at(bodyOrigin.x + bodySize.x * 0.90f - size * 0.5f,
                    bodyOrigin.y + bodySize.y * (0.10f + fallback * 0.13f));
    ImGui::SetCursorScreenPos(at);
    ImGui::PushID(slot.id.c_str());
    ImGui::InvisibleButton("##fallback_slot", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool doubleClicked =
        hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    flecs::entity worn = FindEquippedInSlot(game->playerEntity, slot.id,
                                            &game->itemRegistry);
    if (worn.is_alive()) {
      if (const Texture2D *texture = ItemTexture(worn, game->resourceManager)) {
        draw->AddImage(static_cast<ImTextureID>(
                           static_cast<intptr_t>(texture->id)),
                       at, at + ImVec2(size, size));
      } else {
        draw->AddRectFilled(at, at + ImVec2(size, size),
                            IM_COL32(35, 38, 42, 220), 3.0f);
        draw->AddText(ImVec2(at.x + 3, at.y + 3),
                      IM_COL32(230, 230, 230, 255),
                      ItemLabel(worn).c_str());
      }
      if (doubleClicked) {
        status = UnequipItem(game->playerEntity, worn);
        if (game->debugLog) game->debugLog->LogInfo(status);
      }
      if (ImGui::BeginPopupContextItem("##worn_menu")) {
        if (const ItemType *type = worn.get<ItemType>()) {
          ContentEditorMenu::Draw(*game, game->ContentTargetsForItem(type->id),
                                  false);
        }
        ImGui::EndPopup();
      }
    } else {
      if (const Texture2D *icon = IconPaths::ResolveIconTexture(
              game->iconRegistry, game->resourceManager,
              IconForAccepts(slot.accepts), game->debugLog.get())) {
        draw->AddImage(static_cast<ImTextureID>(
                           static_cast<intptr_t>(icon->id)),
                       at, at + ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1),
                       IM_COL32(255, 255, 255, 105));
      }
      draw->AddRect(at, at + ImVec2(size, size), IM_COL32(180, 180, 190, 80),
                    3.0f);
    }
    if (hovered) {
      ImGui::BeginTooltip();
      ImGui::Text("%s", slot.id.c_str());
      ImGui::TextDisabled("Accepts: %s", slot.accepts.c_str());
      if (worn.is_alive()) {
        ImGui::Text("Worn: %s", ItemLabel(worn).c_str());
        ImGui::TextDisabled("Double-click to take off");
      } else {
        ImGui::TextDisabled("Empty");
      }
      ImGui::EndTooltip();
    }
    ImGui::PopID();
    ++fallback;
  }
  ImGui::SetCursorScreenPos(ImVec2(panelOrigin.x, panelOrigin.y));
  ImGui::Dummy(ImVec2(panelWidth, panelHeight));
}
