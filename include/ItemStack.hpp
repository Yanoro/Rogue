#pragma once

#include <string>

// A counted request for a kind of item: "3 Iron Ingot". Deliberately
// dependency-free (no flecs, no raylib) so the trade grammar and its test stay
// pure string logic.
//
// The engine stores each carried unit as its own entity, so counts are always
// explicit and one ItemStack can describe a recipe input, an NPC's starting
// inventory, or one side of a trade offer.
struct ItemStack {
  // Player-facing DisplayName, e.g. "Iron Ingot". Trade terms are written the
  // way [INVENTORY] prints them, not as content ids.
  std::string item;
  int count = 1;
};
