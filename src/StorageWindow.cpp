#define IMGUI_DEFINE_MATH_OPERATORS
#include "StorageWindow.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "Components.h"
#include "ContentEditorTargets.hpp"
#include "Game.h"
#include "IconPaths.hpp"
#include "InventoryDrag.hpp"
#include "InventorySlotDraw.hpp"
#include "InventoryView.hpp"
#include "ResourceManager.h"
#include "Settings.h"
#include "SpritePaths.hpp"
#include "imgui.h"

StorageWindow::StorageWindow(flecs::entity container, flecs::entity owner,
                             Game *game, ResourceManager *resources)
    : container(container), owner(owner), game(game), resources(resources) {}

void StorageWindow::Draw() {
  // The container can be destroyed (map reload, editor delete) while the window is open;
  // closing here keeps the next frame from reading a dangling entity.
  if (!container.is_alive()) {
    if (owner.is_alive()) {
      owner.remove<ActiveWindow>();
    }
    return;
  }

  std::string title =
      container.has<DisplayName>() ? container.get<DisplayName>()->name
                                   : "Storage";
  // The "###" id keys ImGui's per-window state to this container; including the container
  // id keeps several windows distinct so they can be open at the same time.
  title += "###StorageWindow" + std::to_string(container.id());

  bool isOpen = true;
  // Wide enough for the ten-column cell grid; a narrower window would clip the last
  // columns into a horizontal scrollbar.
  ImGui::SetNextWindowSize(ImVec2(520, 340), ImGuiCond_FirstUseEver);
  ImGui::Begin(title.c_str(), &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    if (owner.is_alive()) {
      owner.remove<ActiveWindow>();
    }
    ImGui::End();
    return;
  }
  if (game == nullptr) {
    ImGui::TextDisabled("The window has no game to read from.");
    ImGui::End();
    return;
  }

  // Walking out of the container's access range closes the window and says why. Checked in
  // Draw because this is the only place the window knows it is on screen; a chest behind a
  // wall stops being usable the moment you step away.
  if (container.has<Storage>() && !game->WithinAccessRange(container)) {
    if (owner.is_alive()) {
      owner.remove<ActiveWindow>();
    }
    game->ShowPlayerNotice(Settings().storage.closeMessage);
    ImGui::End();
    return;
  }

  // A Storage container can be looted; a character inspected through "See Inventory" is
  // read-only. The difference is the component, not the window: corpses and boxes are
  // Storage, a living NPC is an Actor.
  const bool lootable = container.has<Storage>();
  const int capacity = HeldCapacity(container, Settings().player.inventorySlots);
  SlottedInventory view =
      BuildSlottedInventory(container, &game->itemRegistry, capacity);
  const Texture2D *slotPlate = IconPaths::TryResolveIconTexture(
      game->iconRegistry, game->resourceManager, "inventory_slot",
      game->debugLog.get());

  int itemCount = 0;
  for (const InventoryCell &cell : view.cells) itemCount += cell.count;
  ImGui::TextDisabled("%d / %d slots, %d item(s)",
                      static_cast<int>(view.cells.size()), capacity, itemCount);
  ImGui::Separator();
  if (!status.empty()) {
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::Separator();
  }

  // Takes one whole cell (a stack is several item entities) into the player's inventory.
  // All or nothing: a half-moved stack is worse than a refusal.
  const auto takeCell = [&](const InventoryCell &cell) {
    if (!lootable) return;
    const int playerCapacity =
        InventoryCapacity(game->playerEntity, Settings().player.inventorySlots);
    if (!CanAddItems(game->playerEntity, cell.items, &game->itemRegistry,
                     playerCapacity)) {
      status = "Not enough room in your inventory for " + cell.name + ".";
      return;
    }
    MoveHeldItems(container, game->playerEntity, cell.items);
    status = "Took " + cell.name +
             (cell.count > 1 ? " x" + std::to_string(cell.count) : "") + ".";
    if (game->debugLog) game->debugLog->LogInfo(status);
  };

  // A drop lands on one named slot. An occupied slot refuses rather than swapping; a drop
  // from this same window is the reorder, and moves nothing in the ECS.
  const auto acceptDroppedCell = [&](int slot) {
    if (!lootable) return;
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
    if (drag.source == container) {
      PlaceItemsAtSlot(container, drag.items, slot);
      status = "Moved " + drag.name + ".";
      InventoryDrag::Clear();
      return;
    }
    MoveHeldItems(drag.source, container, drag.items);
    PlaceItemsAtSlot(container, drag.items, slot);
    status = "Stored " + drag.name + ".";
    if (game->debugLog) game->debugLog->LogInfo(status);
    InventoryDrag::Clear();
  };

  ImGui::BeginChild("##StorageContents", ImVec2(0, 0));
  constexpr int kColumns = 10;
  constexpr float kCell = 42.0f;
  const int rows = std::max(1, (view.slotCount + kColumns - 1) / kColumns);
  ImGui::BeginTable("##storage_grid", kColumns,
                    ImGuiTableFlags_SizingFixedFit |
                        ImGuiTableFlags_NoSavedSettings |
                        ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_BordersInnerH);
  for (int col = 0; col < kColumns; ++col) {
    ImGui::TableSetupColumn("##slot_col", ImGuiTableColumnFlags_WidthFixed,
                            kCell + 4.0f);
  }
  for (int row = 0; row < rows; ++row) {
    ImGui::TableNextRow();
    for (int col = 0; col < kColumns; ++col) {
      const int index = row * kColumns + col;
      if (index >= view.slotCount) {
        continue;
      }
      ImGui::TableSetColumnIndex(col);
      ImGui::PushID(index);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
      ImGui::InvisibleButton("##slot", ImVec2(kCell, kCell));
      const ImVec2 cellMin = ImGui::GetItemRectMin();
      DrawInventorySlotPlate(ImGui::GetWindowDrawList(), cellMin, kCell,
                             slotPlate, /*available=*/true);
      const bool hovered = ImGui::IsItemHovered();
      const int slotCell =
          index < view.slotCount ? view.slotToCell[static_cast<size_t>(index)] : -1;
      InventoryCell *cell =
          slotCell >= 0 ? &view.cells[static_cast<size_t>(slotCell)] : nullptr;

      if (cell != nullptr && lootable) {
        // The sprite alone, centred on the cursor. The tooltip preview is suppressed because
        // ImGui anchors it to the mouse's bottom-right; drawing on the foreground list puts
        // the picture where the pointer is.
        if (ImGui::BeginDragDropSource(
                ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
          InventoryDrag::Begin(container, *cell);
          const uint64_t token = static_cast<uint64_t>(container.id());
          ImGui::SetDragDropPayload(InventoryDrag::kPayload, &token, sizeof(token));
          const Texture2D *texture = nullptr;
          if (resources != nullptr && !cell->items.empty()) {
            if (const Sprite *sprite = cell->items.front().get<Sprite>()) {
              texture = resources->TryGetTexture(sprite->path);
            }
          }
          if (texture == nullptr && resources != nullptr) {
            texture = resources->TryGetTexture(UnknownSpritePath());
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
      if (lootable && ImGui::BeginDragDropTarget()) {
        if (ImGui::AcceptDragDropPayload(InventoryDrag::kPayload) != nullptr) {
          acceptDroppedCell(index);
        }
        ImGui::EndDragDropTarget();
      }

      if (cell != nullptr) {
        const Texture2D *texture = nullptr;
        if (resources != nullptr && !cell->items.empty()) {
          if (const Sprite *sprite = cell->items.front().get<Sprite>()) {
            texture = resources->TryGetTexture(sprite->path);
          }
        }
        if (texture != nullptr) {
          ImGui::GetWindowDrawList()->AddImage(
              static_cast<ImTextureID>(static_cast<intptr_t>(texture->id)),
              cellMin, cellMin + ImVec2(kCell, kCell));
        } else {
          ImGui::GetWindowDrawList()->AddText(
              ImVec2(cellMin.x + 3, cellMin.y + 3), IM_COL32(220, 220, 220, 255),
              cell->name.c_str());
        }
        if (cell->count > 1) {
          char count[16];
          std::snprintf(count, sizeof(count), "%d", cell->count);
          const ImVec2 size = ImGui::CalcTextSize(count);
          ImGui::GetWindowDrawList()->AddText(
              ImVec2(cellMin.x + kCell - size.x - 2,
                     cellMin.y + kCell - size.y - 1),
              IM_COL32(255, 240, 180, 255), count);
        }
        if (hovered) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(cell->name.c_str());
          if (cell->count > 1) {
            ImGui::Text("Count: %d", cell->count);
          }
          ImGui::TextDisabled(lootable
                                  ? "Right-click to take; drag to your inventory"
                                  : "Read-only: this is a character's inventory");
          ImGui::EndTooltip();
        }
        if (ImGui::BeginPopupContextItem("##storage_slot_menu")) {
          bool anyRows = false;
          if (lootable) {
            if (ImGui::Selectable("Take")) {
              takeCell(*cell);
            }
            anyRows = true;
          }
          // The item definition is still editable content, even in a read-only window.
          ContentEditorMenu::Draw(*game, game->ContentTargetsForItem(cell->itemId),
                                  anyRows);
          ImGui::EndPopup();
        }
      }
      ImGui::PopStyleVar();
      ImGui::PopID();
    }
  }
  ImGui::EndTable();
  ImGui::EndChild();

  ImGui::End();
}
