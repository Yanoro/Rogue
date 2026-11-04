#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include <flecs.h>

#include "Components.h"
#include "ItemDef.hpp"
#include "ItemRegistry.h"

// One visible inventory cell. Stackable items share a cell up to the definition's
// maxStack; equipment is always one item per cell. Equipped items are excluded:
// they are owned through Holds but are not inventory.
struct InventoryCell {
  std::string itemId;
  std::string name;
  const ItemDef *definition = nullptr;
  std::vector<flecs::entity> items;
  int count = 0;
  bool stackable = false;
};

inline const ItemDef *InventoryItemDef(flecs::entity item,
                                       const ItemRegistry *items) {
  if (items == nullptr || !item.is_alive()) return nullptr;
  const ItemType *type = item.get<ItemType>();
  return type != nullptr ? items->Get(type->id) : nullptr;
}

inline std::string InventoryItemName(flecs::entity item) {
  if (const DisplayName *name = item.get<DisplayName>()) return name->name;
  if (const ItemType *type = item.get<ItemType>()) return type->id;
  return "Item";
}

inline std::vector<InventoryCell> BuildInventoryCells(
    flecs::entity owner, const ItemRegistry *items) {
  std::vector<InventoryCell> cells;
  if (!owner.is_alive()) return cells;

  std::unordered_map<std::string, size_t> openStack;
  owner.each<Holds>([&](flecs::entity item) {
    if (!item.is_alive() || item.has<Equipped>()) return;
    const ItemType *type = item.get<ItemType>();
    const ItemDef *definition = InventoryItemDef(item, items);
    const bool stackable = definition != nullptr && definition->stackable &&
                           definition->maxStack > 1;
    const std::string id = type != nullptr ? type->id : InventoryItemName(item);

    if (stackable) {
      const int maxStack = std::max(1, definition->maxStack);
      auto found = openStack.find(id);
      if (found != openStack.end()) {
        InventoryCell &cell = cells[found->second];
        if (cell.count < maxStack) {
          cell.items.push_back(item);
          cell.count++;
          return;
        }
      }
      InventoryCell cell;
      cell.itemId = id;
      cell.name = InventoryItemName(item);
      cell.definition = definition;
      cell.stackable = true;
      cell.count = 1;
      cell.items.push_back(item);
      openStack[id] = cells.size();
      cells.push_back(std::move(cell));
      return;
    }

    InventoryCell cell;
    cell.itemId = id;
    cell.name = InventoryItemName(item);
    cell.definition = definition;
    cell.count = 1;
    cell.items.push_back(item);
    cells.push_back(std::move(cell));
  });
  return cells;
}

inline int InventoryCapacity(flecs::entity owner, int fallback = 30) {
  if (!owner.is_alive()) return fallback;
  const InventorySlots *slots = owner.get<InventorySlots>();
  return slots != nullptr ? std::max(0, slots->capacity) : fallback;
}

inline bool InventoryCanAdd(flecs::entity owner, const ItemDef *definition,
                            const ItemRegistry *items, int capacityFallback = 30) {
  if (!owner.is_alive()) return false;
  const int capacity = InventoryCapacity(owner, capacityFallback);
  const std::vector<InventoryCell> cells = BuildInventoryCells(owner, items);
  if (definition != nullptr && definition->stackable &&
      definition->maxStack > 1) {
    for (const InventoryCell &cell : cells) {
      if (cell.itemId == definition->id && cell.count < definition->maxStack) {
        return true;
      }
    }
  }
  return static_cast<int>(cells.size()) < capacity;
}

inline flecs::entity FindEquippedInSlot(flecs::entity owner,
                                        const std::string &slotId,
                                        const ItemRegistry *items) {
  (void)items;
  flecs::entity found = flecs::entity::null();
  owner.each<Holds>([&](flecs::entity item) {
    if (!item.is_alive() || !item.has<Equipped>()) return;
    const Equipped *equipped = item.get<Equipped>();
    if (equipped == nullptr) return;
    if (std::find(equipped->slots.begin(), equipped->slots.end(), slotId) !=
        equipped->slots.end()) {
      found = item;
    }
  });
  return found;
}
