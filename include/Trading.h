#pragma once

#include <string>
#include <vector>

#include "Components.h"
#include "ItemStack.hpp"

// Entity-level trade operations. The command grammar lives in TradeGrammar.hpp
// (pure string logic) and the conversation state machine that drives these
// lives in TalkAction; this layer owns the offer component and the actual item
// movement.
namespace Trading {

// Why a set of requirements cannot be met. Kept apart from "not enough" on
// purpose: a name that matches nothing is almost always a syntax problem (a
// mis-parsed multi-item list), and reporting it as "you are short X" is what
// made a model conclude its inventory was broken and stop haggling.
struct ShortfallInfo {
  std::vector<std::string> unknownItems;     // no item of this name is held
  std::vector<ItemStack> insufficientStacks; // some held, not enough

  bool Empty() const {
    return unknownItems.empty() && insufficientStacks.empty();
  }
};

// The offer `offerer` has made to `responder`, or nullptr when there is none.
const TradeOffer *FindOffer(flecs::entity offerer, flecs::entity responder);

// Records the offer on `offerer`, replacing any offer it already had.
void SetOffer(flecs::entity offerer, flecs::entity responder,
              const std::vector<ItemStack> &give,
              const std::vector<ItemStack> &receive);

// Drops any pending offer between these two, in either direction.
void ClearOffersBetween(flecs::entity a, flecs::entity b);

// Rewrites each stack's name to the canonical DisplayName of the template it
// matches, so stored terms and echoes read consistently ("Bronze Coin" becomes
// "Bronze Coins"). A name that matches nothing is left exactly as written, so a
// mis-parse stays visible in the error message rather than being quietly
// repaired into something the model did not ask for.
void CanonicalizeNames(flecs::entity context, std::vector<ItemStack> &stacks);

// True when `holder` carries everything in `wanted`.
bool CanAfford(flecs::entity holder, const std::vector<ItemStack> &wanted,
               ShortfallInfo &shortfall);

enum class TradeOutcome {
  Done,
  ResponderCannotPay,
  OffererCannotPay,
};

// Validates both sides against the live inventories before moving anything, then
// swaps the items. Nothing moves unless both sides can pay in full, so a trade
// that fails validation leaves both inventories untouched.
TradeOutcome ExecuteTrade(flecs::entity offerer, flecs::entity responder,
                          const TradeOffer &offer, ShortfallInfo &shortfall);

// "- Iron Ingot (3)\n- Flour\n" for the holder's inventory, or "" when empty.
std::string DescribeInventory(flecs::entity holder);

} // namespace Trading
