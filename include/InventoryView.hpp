#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <flecs.h>

#include "Components.h"
#include "ItemDef.hpp"
#include "ItemRegistry.h"

// One visible cell of a holder's carried items: the player's inventory or a container's
// contents. Stackable items share a cell up to the definition's maxStack; equipment is
// always one item per cell. Equipped items are excluded: they are owned through Holds but
// are not inventory.
struct InventoryCell {
  std::string itemId;
  std::string name;
  const ItemDef *definition = nullptr;
  std::vector<flecs::entity> items;
  int count = 0;
  bool stackable = false;
  // How many of the same id this cell holds. Comes from the object's Stackable component
  // first, then the ItemDef; 1 for a non-stackable object.
  int maxStack = 1;
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
    const Stackable *stack = item.get<Stackable>();
    bool stackable = false;
    int maxStack = 1;
    // The object template's Stackable wins over the ItemDef: the CE checkbox edits the
    // object, and an object without an item definition must be able to stack too.
    if (stack != nullptr && stack->maxStack > 1) {
      stackable = true;
      maxStack = stack->maxStack;
    } else if (definition != nullptr && definition->stackable &&
               definition->maxStack > 1) {
      stackable = true;
      maxStack = definition->maxStack;
    }
    const std::string id = type != nullptr ? type->id : InventoryItemName(item);

    if (stackable) {
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
      cell.maxStack = maxStack;
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

// The number of cells a holder has: a container's Storage capacity, or a character's
// InventorySlots capacity. The chest window draws a Storage's cells the same way the
// inventory draws its own, so it needs the same number both windows can agree on.
inline int HeldCapacity(flecs::entity holder, int fallback = 30) {
  if (!holder.is_alive()) return fallback;
  if (const Storage *storage = holder.get<Storage>()) {
    return std::max(0, storage->capacity);
  }
  return InventoryCapacity(holder, fallback);
}

inline bool InventoryCanAdd(flecs::entity owner, const ItemDef *definition,
                            const ItemRegistry *items, int capacityFallback = 30,
                            const std::string &itemId = std::string()) {
  if (!owner.is_alive()) return false;
  const int capacity = InventoryCapacity(owner, capacityFallback);
  const std::vector<InventoryCell> cells = BuildInventoryCells(owner, items);
  // An ItemDef names the id; an object with no definition passes it explicitly. A stack
  // with room means no new cell is needed.
  const std::string wanted = definition != nullptr ? definition->id : itemId;
  if (!wanted.empty()) {
    for (const InventoryCell &cell : cells) {
      if (cell.itemId == wanted && cell.stackable && cell.count < cell.maxStack) {
        return true;
      }
    }
  }
  return static_cast<int>(cells.size()) < capacity;
}

// Whether `holder` can take every item in `items`, counting cells the way the window draws
// them: stackable items fill existing stacks first, and each item that does not fit needs a
// free cell. Checked before a chest drag or take so a move is all-or-nothing rather than a
// half-moved stack.
inline bool CanAddItems(flecs::entity holder,
                        const std::vector<flecs::entity> &items,
                        const ItemRegistry *registry, int capacity) {
  if (!holder.is_alive()) return false;
  const std::vector<InventoryCell> cells = BuildInventoryCells(holder, registry);
  std::unordered_map<std::string, int> stackRoom;
  for (const InventoryCell &cell : cells) {
    if (!cell.stackable) continue;
    stackRoom[cell.itemId] += std::max(0, cell.maxStack - cell.count);
  }
  int newCells = 0;
  for (flecs::entity item : items) {
    if (!item.is_alive()) continue;
    const ItemDef *definition = InventoryItemDef(item, registry);
    const Stackable *stack = item.get<Stackable>();
    bool stackable = false;
    if (stack != nullptr && stack->maxStack > 1) {
      stackable = true;
    } else if (definition != nullptr && definition->stackable &&
               definition->maxStack > 1) {
      stackable = true;
    }
    if (stackable) {
      const ItemType *type = item.get<ItemType>();
      const std::string id =
          definition != nullptr ? definition->id
                               : (type != nullptr ? type->id : std::string());
      auto room = stackRoom.find(id);
      if (room != stackRoom.end() && room->second > 0) {
        --room->second;
        continue;
      }
    }
    ++newCells;
  }
  return static_cast<int>(cells.size()) + newCells <= capacity;
}

// Moves whole cells between two holders. The caller checks CanAddItems first: this does not
// roll back, because a half-moved stack is worse than a refused one.
inline void MoveHeldItems(flecs::entity from, flecs::entity to,
                          const std::vector<flecs::entity> &items) {
  if (!to.is_alive()) return;
  for (flecs::entity item : items) {
    if (!item.is_alive()) continue;
    if (from.is_alive()) from.remove<Holds>(item);
    to.add<Holds>(item);
    item.child_of(to);
  }
}

// A holder's cells placed in slots: `cells` is the same grouped list BuildInventoryCells
// returns, and slotToCell says which cell sits in each slot (-1 for an empty one).
struct SlottedInventory {
  std::vector<InventoryCell> cells;
  std::vector<int> slotToCell;
  int slotCount = 0;
};

// Places a holder's cells according to its HeldSlotLayout and returns the result.
//
// A cell keeps the slot it had; a cell that has none (a new pickup, or one whose stack a
// merge changed) takes the lowest free slot. Slots whose layout entry no longer names a held
// item are cleared, so a stale arrangement heals instead of hiding items. The resolved
// arrangement is written back to the holder, which is what lets a drop target a slot and a
// reorder survive the window closing.
//
// Writes a component on `holder`, so call it from the draw phase (where window code already
// runs inside the ECS defer) rather than from a system iteration.
inline SlottedInventory BuildSlottedInventory(flecs::entity holder,
                                              const ItemRegistry *items,
                                              int capacity) {
  SlottedInventory out;
  out.cells = BuildInventoryCells(holder, items);
  out.slotCount = std::max(capacity, static_cast<int>(out.cells.size()));
  out.slotToCell.assign(static_cast<size_t>(out.slotCount), -1);
  if (!holder.is_alive()) return out;

  std::unordered_map<std::uint64_t, size_t> cellOfEntity;
  for (size_t cell = 0; cell < out.cells.size(); ++cell) {
    for (flecs::entity item : out.cells[cell].items) {
      if (item.is_alive()) {
        cellOfEntity[static_cast<std::uint64_t>(item.id())] = cell;
      }
    }
  }

  HeldSlotLayout *layout = holder.get_mut<HeldSlotLayout>();
  std::vector<std::uint64_t> slots =
      layout != nullptr ? layout->slots : std::vector<std::uint64_t>{};
  slots.resize(static_cast<size_t>(out.slotCount), 0);

  // First pass: honour the arrangement that is still valid.
  std::vector<int> placed(out.cells.size(), -1);
  for (int slot = 0; slot < out.slotCount; ++slot) {
    const std::uint64_t id = slots[static_cast<size_t>(slot)];
    if (id == 0) continue;
    const auto found = cellOfEntity.find(id);
    if (found == cellOfEntity.end()) {
      slots[static_cast<size_t>(slot)] = 0;  // the item left this holder
      continue;
    }
    const size_t cell = found->second;
    if (placed[cell] >= 0) {
      slots[static_cast<size_t>(slot)] = 0;  // same cell already placed elsewhere
      continue;
    }
    placed[cell] = slot;
    out.slotToCell[static_cast<size_t>(slot)] = static_cast<int>(cell);
  }

  // Second pass: cells with no home take the lowest free slot, in cell order.
  int nextFree = 0;
  for (size_t cell = 0; cell < out.cells.size(); ++cell) {
    if (placed[cell] >= 0) continue;
    while (nextFree < out.slotCount && out.slotToCell[static_cast<size_t>(nextFree)] >= 0) {
      ++nextFree;
    }
    if (nextFree >= out.slotCount) break;
    placed[cell] = nextFree;
    out.slotToCell[static_cast<size_t>(nextFree)] = static_cast<int>(cell);
    slots[static_cast<size_t>(nextFree)] =
        out.cells[cell].items.empty()
            ? 0
            : static_cast<std::uint64_t>(out.cells[cell].items.front().id());
    ++nextFree;
  }

  if (layout != nullptr) {
    layout->slots = std::move(slots);
  } else {
    // First sight of this holder: create the component so the next frame can place a drop
    // into a specific slot. The local arrangement above is still what is drawn now.
    holder.set<HeldSlotLayout>({});
  }
  return out;
}

// Puts a cell's items in `slot`, clearing every slot entry that referred to them. Used for a
// drop onto an empty slot: within one holder this is the reorder, and across holders the
// items have already been moved. Creating the layout on demand covers a holder whose window
// opened this frame.
inline void PlaceItemsAtSlot(flecs::entity holder,
                             const std::vector<flecs::entity> &items, int slot) {
  if (!holder.is_alive() || items.empty() || slot < 0) return;
  HeldSlotLayout *layout = holder.get_mut<HeldSlotLayout>();
  if (layout == nullptr) {
    holder.set<HeldSlotLayout>({});
    return;
  }
  if (static_cast<size_t>(slot) >= layout->slots.size()) {
    layout->slots.resize(static_cast<size_t>(slot) + 1, 0);
  }
  std::unordered_set<std::uint64_t> ids;
  std::uint64_t representative = 0;
  for (flecs::entity item : items) {
    if (!item.is_alive()) continue;
    const std::uint64_t id = static_cast<std::uint64_t>(item.id());
    ids.insert(id);
    if (representative == 0) representative = id;
  }
  if (representative == 0) return;
  for (std::uint64_t &entry : layout->slots) {
    if (ids.count(entry) != 0) entry = 0;
  }
  layout->slots[static_cast<size_t>(slot)] = representative;
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
