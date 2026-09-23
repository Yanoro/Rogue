#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "LootDrop.hpp"

// One entry of a loot table, carrying the chance every contributor settled on.
struct LootEntry {
  std::string itemType;
  float chance = 1.0f;
  // Which source contributed this entry. Empty means it came from the authored
  // table rather than from a contributor -- and only an entry's own source may
  // remove it (docs/stat-effects-design.md section 2.3).
  std::string sourceId;
};

// The authored drops as entries: no contributor, no adjustments.
inline std::vector<LootEntry> ToLootEntries(const std::vector<LootDrop> &drops) {
  std::vector<LootEntry> entries;
  entries.reserve(drops.size());
  for (const LootDrop &drop : drops) {
    LootEntry entry;
    entry.itemType = drop.itemType;
    entry.chance = drop.chance;
    entries.push_back(std::move(entry));
  }
  return entries;
}

// A loot table under construction.
//
// Contributors change the table through this and never touch the authored data.
// Two properties make the result independent of the order contributors ran in,
// which is the whole reason loot is handled this way rather than by letting each
// hook return a finished table (section 2.3):
//
//   * entries and adjustments are kept APART. An adjustment targets an item type
//     (or every entry), and is applied when the table is read, so a scaling that
//     was contributed before another source appended an entry still reaches that
//     entry.
//   * within one read, all matching scales are multiplied together and all
//     matching bonuses are summed before being applied once. Multiplication and
//     addition each commute, so mixing them across contributors cannot depend on
//     who went first.
//
// Removal is the one deliberately order-sensitive operation, and ownership is
// what makes it safe: a source can only remove what it appended itself.
class LootTableBuilder {
public:
  LootTableBuilder() = default;

  explicit LootTableBuilder(const std::vector<LootDrop> &base)
      : entries(ToLootEntries(base)) {}

  // Adds an entry owned by `sourceId`, so that only `sourceId` can remove it.
  void Append(std::string itemType, float chance, std::string sourceId) {
    LootEntry entry;
    entry.itemType = std::move(itemType);
    entry.chance = chance;
    entry.sourceId = std::move(sourceId);
    entries.push_back(std::move(entry));
  }

  // Removes every entry of this item type that `sourceId` contributed. Authored
  // entries have no source and so can never be removed here, and an empty
  // sourceId is refused rather than matching them by accident.
  bool RemoveOwned(const std::string &itemType, const std::string &sourceId) {
    if (sourceId.empty()) {
      return false;
    }
    const size_t before = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const LootEntry &entry) {
                                   return entry.itemType == itemType &&
                                          entry.sourceId == sourceId;
                                 }),
                  entries.end());
    return entries.size() != before;
  }

  // Chance adjustments. An empty `itemType` means every entry, including ones
  // appended later by another source.
  void ScaleChance(const std::string &itemType, float factor,
                   std::string sourceId) {
    adjustments.push_back({itemType, factor, 0.0f, std::move(sourceId)});
  }

  void AddChance(const std::string &itemType, float amount,
                 std::string sourceId) {
    adjustments.push_back({itemType, 1.0f, amount, std::move(sourceId)});
  }

  // Every entry with all adjustments applied, clamped to a valid probability.
  // Recomputed on each call, so a contributor always reads the table as it
  // currently stands rather than a stale copy.
  std::vector<LootEntry> Entries() const {
    std::vector<LootEntry> resolved = entries;
    for (LootEntry &entry : resolved) {
      float scale = 1.0f;
      float bonus = 0.0f;
      for (const Adjustment &adjustment : adjustments) {
        if (!adjustment.itemType.empty() &&
            adjustment.itemType != entry.itemType) {
          continue;
        }
        scale *= adjustment.scale;
        bonus += adjustment.bonus;
      }
      float chance = entry.chance * scale + bonus;
      if (chance < 0.0f) {
        chance = 0.0f;
      } else if (chance > 1.0f) {
        chance = 1.0f;
      }
      entry.chance = chance;
    }
    return resolved;
  }

  bool Empty() const { return entries.empty(); }

  // The entries as authored or appended, before adjustments. For debugging and
  // for tests that want to see what was actually added.
  const std::vector<LootEntry> &RawEntries() const { return entries; }

private:
  struct Adjustment {
    std::string itemType; // empty = every entry
    float scale = 1.0f;
    float bonus = 0.0f;
    // Informational: which source asked for this. Adjustments are not restricted
    // by ownership the way removal is, but the tag is what will let the debug UI
    // explain a final chance.
    std::string sourceId;
  };

  std::vector<LootEntry> entries;
  std::vector<Adjustment> adjustments;
};
