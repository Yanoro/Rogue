#pragma once

#include <string>
#include <vector>

#include <flecs.h>

#include "InventoryView.hpp"

// The cell currently being dragged between the player's inventory and a container.
//
// ImGui's drag-drop payload is only a token: a std::vector is not trivially copyable, and a
// drop target needs the whole cell (a stack can be several item entities). The token is the
// source holder's id and this state carries the rest for the duration of the drag.
namespace InventoryDrag {

inline constexpr const char *kPayload = "AIROGUE_HELD_CELL";

struct State {
  flecs::entity source = flecs::entity::null();
  std::vector<flecs::entity> items;
  std::string name;
  bool active = false;
};

// One instance across translation units: the inventory and the storage window are separate
// TUs, and the whole point is that both see the same drag.
inline State &Active() {
  static State state;
  return state;
}

inline void Begin(flecs::entity source, const InventoryCell &cell) {
  State &state = Active();
  state.source = source;
  state.items = cell.items;
  state.name = cell.name;
  state.active = true;
}

inline void Clear() { Active() = State{}; }

}  // namespace InventoryDrag
