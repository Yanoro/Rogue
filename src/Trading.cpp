#include "Trading.h"

#include <algorithm>

#include "EquipmentRuntime.hpp"
#include "ObjectFactory.h"
#include "StringUtils.hpp"
#include "TradeGrammar.hpp"

namespace Trading {

namespace {

// Collects the concrete entities from `holder` that satisfy `wanted`. Each
// entity is claimed at most once, so a list naming the same item twice cannot
// spend one unit twice.
//
// Matching candidates are counted without the per-stack cap so that "holds none
// of this at all" can be told apart from "does not hold enough of it". That
// distinction is what makes a mis-parsed item name readable instead of looking
// like an empty inventory.
bool CollectItems(flecs::entity holder, const std::vector<ItemStack> &wanted,
                  std::vector<flecs::entity> &out, ShortfallInfo &shortfall) {
  shortfall.unknownItems.clear();
  shortfall.insufficientStacks.clear();

  for (const ItemStack &stack : wanted) {
    std::vector<flecs::entity> found;
    int available = 0;
    holder.each<Holds>([&](flecs::entity child) {
      if (!child.is_alive()) return;
      if (std::find(out.begin(), out.end(), child) != out.end()) return;
      const DisplayName *name = child.get<DisplayName>();
      if (!name || !TradeGrammar::ItemNameMatches(name->name, stack.item)) return;
      available++;
      if (static_cast<int>(found.size()) < stack.count) {
        found.push_back(child);
      }
    });

    if (static_cast<int>(found.size()) < stack.count) {
      if (available == 0) {
        shortfall.unknownItems.push_back(stack.item);
      } else {
        ItemStack lacking;
        lacking.item = stack.item;
        lacking.count = stack.count - static_cast<int>(found.size());
        shortfall.insufficientStacks.push_back(lacking);
      }
      continue;
    }
    out.insert(out.end(), found.begin(), found.end());
  }
  return shortfall.Empty();
}

} // namespace

const TradeOffer *FindOffer(flecs::entity offerer, flecs::entity responder) {
  const TradeOffer *offer = offerer.get<TradeOffer>();
  if (!offer || offer->offerer != offerer || offer->responder != responder) {
    return nullptr;
  }
  return offer;
}

void SetOffer(flecs::entity offerer, flecs::entity responder,
              const std::vector<ItemStack> &give,
              const std::vector<ItemStack> &receive) {
  TradeOffer offer;
  offer.offerer = offerer;
  offer.responder = responder;
  offer.give = give;
  offer.receive = receive;
  offerer.set<TradeOffer>(offer);
}

void ClearOffersBetween(flecs::entity a, flecs::entity b) {
  auto dropIfBetween = [](flecs::entity holder, flecs::entity other) {
    const TradeOffer *offer = holder.get<TradeOffer>();
    if (!offer) return;
    const bool involvesOther =
        (offer->offerer == holder && offer->responder == other) ||
        (offer->offerer == other && offer->responder == holder);
    if (involvesOther) {
      holder.remove<TradeOffer>();
    }
  };
  dropIfBetween(a, b);
  dropIfBetween(b, a);
}

void CanonicalizeNames(flecs::entity context, std::vector<ItemStack> &stacks) {
  if (stacks.empty()) return;

  flecs::world world = context.world();
  auto factoryRes = world.get<ObjectFactoryResource>();
  if (!factoryRes || !factoryRes->factory) return;

  // Template names are the DisplayNames that SpawnObject stamps onto entities,
  // so matching against them needs neither party's inventory and reveals
  // nothing about what the other side is holding.
  const auto &templates = factoryRes->factory->GetTemplates();
  for (ItemStack &stack : stacks) {
    for (const auto &entry : templates) {
      const std::string canonical = entry.second.value("name", entry.first);
      if (TradeGrammar::ItemNameMatches(canonical, stack.item)) {
        stack.item = canonical;
        break;
      }
    }
  }
}

bool CanAfford(flecs::entity holder, const std::vector<ItemStack> &wanted,
               ShortfallInfo &shortfall) {
  std::vector<flecs::entity> items;
  return CollectItems(holder, wanted, items, shortfall);
}

TradeOutcome ExecuteTrade(flecs::entity offerer, flecs::entity responder,
                          const TradeOffer &offer, ShortfallInfo &shortfall) {
  // Collect both sides before moving anything: a trade is all-or-nothing, so a
  // side that cannot pay must not have its goods taken anyway.
  std::vector<flecs::entity> fromResponder;
  std::vector<flecs::entity> fromOfferer;

  if (!CollectItems(responder, offer.receive, fromResponder, shortfall)) {
    return TradeOutcome::ResponderCannotPay;
  }
  if (!CollectItems(offerer, offer.give, fromOfferer, shortfall)) {
    return TradeOutcome::OffererCannotPay;
  }

  for (flecs::entity item : fromResponder) {
    // Traded away is no longer worn: Equipped lives on the item, so this is what
    // stops a handed-over ring from still boosting the giver.
    DropEquipped(item);
    responder.remove<Holds>(item);
    offerer.add<Holds>(item);
    item.child_of(offerer);
  }
  for (flecs::entity item : fromOfferer) {
    DropEquipped(item);
    offerer.remove<Holds>(item);
    responder.add<Holds>(item);
    item.child_of(responder);
  }
  return TradeOutcome::Done;
}

std::string DescribeInventory(flecs::entity holder) {
  // Same shape as [INVENTORY]: one line per distinct name, count in brackets.
  // Shares the stacking rules with the inventory and examine text.
  std::vector<std::string> names;
  holder.each<Holds>([&](flecs::entity child) {
    if (!child.is_alive() || !child.has<DisplayName>()) return;
    names.push_back(child.get<DisplayName>()->name);
  });

  std::string text;
  for (const auto &entry : StringUtils::StackNames(names)) {
    text += "- " + entry.first;
    if (entry.second > 1) {
      text += " (" + std::to_string(entry.second) + ")";
    }
    text += "\n";
  }
  return text;
}

} // namespace Trading
