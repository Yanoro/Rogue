#include "GuideBuilder.h"

#include <algorithm>
#include <set>

#include "Components.h"
#include "ObjectFactory.h"
#include "RecipeFormatting.h"
#include "RecipeRegistry.h"
#include "RequiresParsing.h"
#include "SkillRegistry.h"
#include "StringUtils.hpp"
#include "TrainingParsing.h"

namespace GuideBuilder {
namespace {

// How far the chain is walked. This is a guard, not a policy: the graph is small
// today (the deepest chain is iron_scythe -> iron_ingot -> iron_ore), but a
// cyclic or unusually deep recipe set must never become an infinite loop on an
// agent's turn. Truncation is reported rather than silent.
constexpr int kDepthCap = 3;

const RecipeRegistry *Recipes(flecs::world &world) {
  auto *resource = world.get<RecipeRegistryResource>();
  return resource ? resource->registry : nullptr;
}

const SkillRegistry *Skills(flecs::world &world) {
  auto *resource = world.get<SkillRegistryResource>();
  return resource ? resource->registry : nullptr;
}

std::string Join(const std::vector<std::string> &parts) {
  std::string out;
  for (const std::string &part : parts) {
    if (!out.empty()) out += ", ";
    out += part;
  }
  return out;
}

// Whether a template lists a component in its "components" array. That array --
// not the presence of a JSON key -- is what decides whether ObjectFactory runs a
// setter, so it is also what decides whether a component exists at runtime. The
// guide has to agree with the spawner, not with the file: a "requires" block on a
// template that never lists "Requires" gates nothing, and reporting it would be a
// lie in the other direction.
bool TemplateListsComponent(const nlohmann::json &tmpl,
                            const std::string &name) {
  if (!tmpl.contains("components") || !tmpl["components"].is_array()) {
    return false;
  }
  for (const auto &component : tmpl["components"]) {
    if (component.is_string() && component.get<std::string>() == name) {
      return true;
    }
  }
  return false;
}

// "5 blacksmithing XP" / "2 farming XP, 3 cooking XP". Empty when the subject
// teaches nothing, so a caller can append the whole clause only when it says
// something.
//
// Skills are named by id, matching how they are authored in a "trains" block and
// how [GUIDE] takes its argument -- not by SkillDef::name, which is prose.
std::string FormatTraining(const std::vector<SkillXp> &grants) {
  std::string out;
  for (const SkillXp &grant : grants) {
    if (grant.xp <= 0) continue;
    if (!out.empty()) out += ", ";
    out += std::to_string(grant.xp) + " " + grant.skillId + " XP";
  }
  return out;
}

// "Requires equipped: scythe, pickaxe". Empty when nothing is gated, so the
// clause appears only when the engine would actually refuse the attempt.
std::string FormatRequires(const std::vector<std::string> &tags) {
  if (tags.empty()) return "";
  return "Requires equipped: " + Join(tags);
}

// The station advertising `recipeId`, or empty when no station in this world
// does. A recipe may be offered by more than one station; the first found is
// reported, because this is advisory context rather than an exhaustive answer.
std::string StationFor(flecs::world &world, const std::string &recipeId) {
  std::string station;
  world.filter<Workstation, DisplayName>().each(
      [&](flecs::entity, const Workstation &workstation,
          const DisplayName &name) {
        if (!station.empty()) return;
        for (const std::string &id : workstation.recipes) {
          if (StringUtils::EqualsIgnoreCase(id, recipeId)) {
            station = name.name;
            return;
          }
        }
      });
  return station;
}

// Every recipe whose outputs include `item`, in sorted id order so the answer is
// deterministic. More than one is possible (a byproduct), which is why this
// returns a list while the chain walk below takes only the first.
std::vector<const CraftRecipe *> RecipesProducing(const RecipeRegistry &recipes,
                                                  const std::string &item) {
  std::vector<const CraftRecipe *> found;
  for (const std::string &id : recipes.GetIds()) {
    const CraftRecipe *recipe = recipes.Get(id);
    if (!recipe) continue;
    for (const LootDrop &output : recipe->outputs) {
      if (StringUtils::EqualsIgnoreCase(output.itemType, item)) {
        found.push_back(recipe);
        break;
      }
    }
  }
  return found;
}

// One recipe line, its station and its hint. This is the single primitive both
// verbs render through.
void AppendRecipeLine(flecs::world &world, const CraftRecipe &recipe,
                      const std::string &prefix, const std::string &hintPad,
                      std::string &out) {
  std::string line = prefix + FormatRecipe(recipe);
  const std::string station = StationFor(world, recipe.id);
  if (!station.empty()) {
    line += " @ " + station;
  }
  // What doing this work teaches, from the recipe's own "trains" block. The fact
  // lives on the subject rather than the skill (see SkillXp.hpp), which is why it
  // can only be surfaced here -- the agent has no other way to learn it.
  const std::string training = FormatTraining(recipe.trains);
  if (!training.empty()) {
    line += " - Gives " + training;
  }
  out += line + "\n";
  if (!recipe.hint.empty()) {
    out += hintPad + "hint: " + recipe.hint + "\n";
  }
}

// Every KIND of object that yields `item`, one line each, with the object's
// authored hint when it carries one. This is where a raw material -- which has no
// recipe, and therefore nowhere in data/recipes to hold a hint -- gets to explain
// itself.
//
// One line per source TYPE, not per placed instance: a map with three iron veins
// is making one point about where ore comes from, and repeating it verbatim would
// burn permanent context for nothing (see §9 of the design doc).
//
// Reports rather than skips when nothing yields it: a silent gap reads exactly
// like "cannot be obtained" and hides a content bug, the same reason
// FormatStationRecipes prints unknown ids instead of omitting them.
void AppendRawSources(flecs::world &world, const std::string &item, int indent,
                      bool bullet, std::string &out) {
  const std::string pad(static_cast<size_t>(indent) * 2, ' ');
  const std::string lead = pad + (bullet ? "- " : "");
  bool found = false;
  std::set<std::string> seenSources;

  world.filter<Harvestable, DisplayName>().each(
      [&](flecs::entity entity, const Harvestable &harvestable,
          const DisplayName &name) {
        bool yields = false;
        for (const LootDrop &drop : harvestable.lootTable.drops) {
          if (StringUtils::EqualsIgnoreCase(drop.itemType, item)) {
            yields = true;
            break;
          }
        }
        if (!yields) return;

        // ItemType is the template key, so two instances of the same object
        // collapse to one line while two different objects that both yield the
        // item are still both reported.
        if (const ItemType *type = entity.get<ItemType>()) {
          if (!seenSources.insert(type->id).second) return;
        }

        found = true;
        std::string line =
            lead + item + ": raw, from " + name.name + " (harvestable)";
        // The gate the engine will enforce is stated first, so a model does not
        // set out to harvest something its hands cannot take. Requires is only
        // present when the template declared a non-empty gate, so its presence
        // here is exactly the engine's condition for refusing.
        if (const Requires *gate = entity.get<Requires>()) {
          const std::string required = FormatRequires(gate->equippedTags);
          if (!required.empty()) {
            line += " - " + required;
          }
        }
        // Harvesting is a skill subject too (Trains, stamped from the template's
        // "trains" block). This is how an agent learns that working a wheat field
        // is what advances farming.
        if (const Trains *trains = entity.get<Trains>()) {
          const std::string training = FormatTraining(trains->grants);
          if (!training.empty()) {
            line += " - Gives " + training;
          }
        }
        out += line + "\n";
        if (const Hint *hint = entity.get<Hint>()) {
          if (!hint->text.empty()) {
            out += pad + "  hint: " + hint->text + "\n";
          }
        }
      });

  if (!found) {
    out += lead + item + ": raw; no source found in this world\n";
  }
}

void Expand(flecs::world &world, const RecipeRegistry &recipes,
            const CraftRecipe &recipe, int depth, int indent, std::string &out) {
  const std::string pad(static_cast<size_t>(indent) * 2, ' ');
  AppendRecipeLine(world, recipe, pad + "- ", pad + "  ", out);

  if (depth >= kDepthCap) {
    out += pad + "  (truncated at depth " + std::to_string(kDepthCap) +
           "; step further with [INFO <item>])\n";
    return;
  }

  for (const ItemStack &input : recipe.inputs) {
    const std::vector<const CraftRecipe *> producers =
        RecipesProducing(recipes, input.item);
    if (!producers.empty()) {
      // First producer only: a second one would make the tree fan out, and the
      // model can ask [INFO input] if it wants the alternatives.
      Expand(world, recipes, *producers.front(), depth + 1, indent + 1, out);
    } else {
      AppendRawSources(world, input.item, indent + 1, true, out);
    }
  }
}

} // namespace

std::vector<std::string> Topics(flecs::world &world) {
  const SkillRegistry *skills = Skills(world);
  if (!skills) return {};
  return skills->GetIds();
}

std::vector<std::string> ItemIds(flecs::world &world) {
  std::set<std::string> ids;
  if (const RecipeRegistry *recipes = Recipes(world)) {
    for (const std::string &id : recipes->GetIds()) {
      if (const CraftRecipe *recipe = recipes->Get(id)) {
        for (const LootDrop &output : recipe->outputs) {
          ids.insert(output.itemType);
        }
      }
    }
  }

  // The gathered half comes from object TEMPLATES rather than from the objects
  // actually placed in this world. That keeps the vocabulary a property of the
  // content instead of the current map -- a sparse map must not silently drop
  // iron_ore from the set of things a model may ask about -- and it keeps [INFO]
  // consistent with the guide, whose chain walk names raw inputs directly.
  //
  // Whether a source exists HERE is a different question, and AppendRawSources
  // still answers it from the world.
  if (auto *resource = world.get<ObjectFactoryResource>()) {
    if (const ObjectFactory *factory = resource->factory) {
      for (const auto &entry : factory->GetTemplates()) {
        const nlohmann::json &tmpl = entry.second;
        if (!TemplateListsComponent(tmpl, "Harvestable") ||
            !tmpl.contains("drops") || !tmpl["drops"].is_array()) {
          continue;
        }
        for (const auto &drop : tmpl["drops"]) {
          const std::string item = drop.value("item", "");
          if (!item.empty()) ids.insert(item);
        }
      }
    }
  }

  return {ids.begin(), ids.end()};
}

std::string Resolve(const std::vector<std::string> &candidates,
                    const std::string &query) {
  for (const std::string &candidate : candidates) {
    if (StringUtils::EqualsIgnoreCase(candidate, query)) return candidate;
  }
  return "";
}

struct HarvestTrainer {
  std::string displayName;
  // Not named `requires`: that is a C++20 keyword and cannot be an identifier.
  std::string requiredClause;
  std::string training;
  std::string hint;
};

// Every harvestable OBJECT TEMPLATE that teaches `topic`, sorted by display name.
//
// Read from templates rather than from objects placed in the world, for the same
// reason ItemIds is: what harvesting teaches is a property of the content, so a
// map that happens to place no wheat must not make [GUIDE farming] forget that
// harvesting wheat advances it. Whether one is standing nearby is a different
// question, answered by [LOOK] and [PLACES].
std::vector<HarvestTrainer> HarvestTrainers(flecs::world &world,
                                            const std::string &topic) {
  std::vector<HarvestTrainer> trainers;

  auto *resource = world.get<ObjectFactoryResource>();
  const ObjectFactory *factory = resource ? resource->factory : nullptr;
  if (!factory) return trainers;

  // A malformed "trains" block was already reported at load time by
  // ObjectFactory; re-reporting it here would double every warning.
  const auto ignoreWarning = [](const std::string &) {};

  for (const auto &entry : factory->GetTemplates()) {
    const nlohmann::json &tmpl = entry.second;
    if (!TemplateListsComponent(tmpl, "Harvestable") ||
        !tmpl.contains("trains")) {
      continue;
    }

    std::vector<SkillXp> grants;
    ParseTrainingGrants(tmpl["trains"], "Object template '" + entry.first + "'",
                        grants, ignoreWarning);

    bool teachesTopic = false;
    for (const SkillXp &grant : grants) {
      if (grant.xp > 0 && StringUtils::EqualsIgnoreCase(grant.skillId, topic)) {
        teachesTopic = true;
        break;
      }
    }
    if (!teachesTopic) continue;

    // Gate read only when the template also lists the component, because that
    // array -- not the presence of a "requires" key -- is what makes
    // ObjectFactory create the gate at runtime.
    std::vector<std::string> requiredTags;
    if (TemplateListsComponent(tmpl, "Requires")) {
      requiredTags = ParseRequiredTags(tmpl, entry.first, ignoreWarning);
    }

    trainers.push_back({tmpl.value("name", entry.first),
                        FormatRequires(requiredTags), FormatTraining(grants),
                        tmpl.value("hint", std::string(""))});
  }

  // Templates live in an unordered_map, so sort for a stable answer.
  std::sort(trainers.begin(), trainers.end(),
            [](const HarvestTrainer &a, const HarvestTrainer &b) {
              return a.displayName < b.displayName;
            });
  return trainers;
}

std::string BuildTopicGuide(flecs::world &world, const std::string &topic) {
  const RecipeRegistry *recipes = Recipes(world);
  const SkillRegistry *skills = Skills(world);
  const SkillDef *definition = skills ? skills->Get(topic) : nullptr;
  const std::string display = definition ? definition->name : topic;

  std::vector<const CraftRecipe *> roots;
  if (recipes) {
    for (const std::string &id : recipes->GetIds()) {
      const CraftRecipe *recipe = recipes->Get(id);
      if (!recipe) continue;
      for (const SkillXp &grant : recipe->trains) {
        if (StringUtils::EqualsIgnoreCase(grant.skillId, topic)) {
          roots.push_back(recipe);
          break;
        }
      }
    }
  }

  const std::vector<HarvestTrainer> trainers = HarvestTrainers(world, topic);

  std::string out;
  if (!roots.empty()) {
    out += display + " - " + std::to_string(roots.size()) + " recipe(s).\n";
    for (const CraftRecipe *recipe : roots) {
      Expand(world, *recipes, *recipe, 0, 0, out);
    }
  }

  // How a non-crafting activity is actually advanced. Without this, [GUIDE
  // farming] could only report that no recipe trains it, which is true and
  // useless.
  if (!trainers.empty()) {
    out += "Trained by harvesting:\n";
    for (const HarvestTrainer &trainer : trainers) {
      std::string line = "- " + trainer.displayName + " (harvestable)";
      if (!trainer.requiredClause.empty()) {
        line += " - " + trainer.requiredClause;
      }
      if (!trainer.training.empty()) line += " - Gives " + trainer.training;
      out += line + "\n";
      if (!trainer.hint.empty()) {
        out += "  hint: " + trainer.hint + "\n";
      }
    }
  }

  if (out.empty()) {
    // Honest rather than silent: an activity can be declared with neither a
    // recipe nor a harvestable subject, and saying so beats an empty list.
    out += display + " - nothing in this world makes or trains this.\n";
    if (definition && !definition->trainedBy.empty()) {
      out += "It is trained by: " + Join(definition->trainedBy) + ".\n";
    }
  }
  return out;
}

std::string BuildItemInfo(flecs::world &world, const std::string &item) {
  std::string out;
  if (const RecipeRegistry *recipes = Recipes(world)) {
    for (const CraftRecipe *recipe : RecipesProducing(*recipes, item)) {
      AppendRecipeLine(world, *recipe, "", "", out);
    }
    if (!out.empty()) return out;
  }

  // No recipe makes it, so it is gathered rather than crafted. Unchanged from
  // the chain walk's leaf rendering, minus the bullet.
  AppendRawSources(world, item, 0, false, out);
  return out;
}

} // namespace GuideBuilder
