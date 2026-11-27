#include "CraftWindow.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Components.h"
#include "Game.h"
#include "IconPaths.hpp"
#include "InteractionRegistry.h"
#include "ItemDef.hpp"
#include "ItemRegistry.h"
#include "RecipeFormatting.h"
#include "RecipeRegistry.h"
#include "Settings.h"
#include "SkillRegistry.h"
#include "imgui.h"

namespace {

std::string ResolveItemName(const ItemRegistry &items, const std::string &id) {
  if (const ItemDef *definition = items.Get(id)) {
    if (!definition->name.empty()) {
      return definition->name;
    }
  }
  return id;
}

std::map<std::string, int> CountHeld(flecs::entity holder) {
  std::map<std::string, int> counts;
  if (!holder.is_alive()) {
    return counts;
  }
  holder.each<Holds>([&](flecs::entity item) {
    if (!item.is_alive()) {
      return;
    }
    if (const ItemType *type = item.get<ItemType>()) {
      ++counts[type->id];
    }
  });
  return counts;
}

struct InputLine {
  std::string id;
  int have = 0;
  int need = 0;
};

std::vector<InputLine> InputsFor(const CraftRecipe &recipe,
                                 const std::map<std::string, int> &held) {
  std::vector<InputLine> lines;
  for (const ItemStack &input : recipe.inputs) {
    InputLine line;
    line.id = input.item;
    line.need = input.count;
    const auto found = held.find(input.item);
    line.have = found == held.end() ? 0 : found->second;
    lines.push_back(std::move(line));
  }
  return lines;
}

bool Affordable(const std::vector<InputLine> &lines) {
  for (const InputLine &line : lines) {
    if (line.have < line.need) {
      return false;
    }
  }
  return true;
}

std::string MissingSummary(const std::vector<InputLine> &lines,
                           const ItemRegistry &items) {
  std::string text;
  for (const InputLine &line : lines) {
    if (line.have >= line.need) {
      continue;
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += std::to_string(line.need - line.have) + " " +
            ResolveItemName(items, line.id);
  }
  return text;
}

// A progress bar with a label that does not move. ImGui places its own label by a width
// heuristic (inside the bar, or to its right when the text is wide), which is what made
// "Crafting" drift; drawing it ourselves keeps it dead centre for the whole craft.
void DrawCenteredCraftingBar(float progress, const ImVec2 &size) {
  ImGui::ProgressBar(progress, size, "");
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  const ImVec2 textSize = ImGui::CalcTextSize("Crafting");
  const ImVec2 position{min.x + (max.x - min.x - textSize.x) * 0.5f,
                        min.y + (max.y - min.y - textSize.y) * 0.5f};
  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddText(ImVec2(position.x + 1.0f, position.y + 1.0f),
                IM_COL32(0, 0, 0, 220), "Crafting");
  draw->AddText(position, IM_COL32(255, 255, 255, 255), "Crafting");
}

} // namespace

CraftWindow::CraftWindow(Game *game, flecs::entity station)
    : game(game), station(station) {
  seenCraftSerial = game != nullptr ? game->lastCraftMessageSerial : 0;
}

void CraftWindow::SetStation(flecs::entity next) {
  const bool wasOpen = game != nullptr && game->craftWindowEntity.is_alive() &&
                       game->craftWindowEntity.has<ActiveWindow>();
  const bool changed = station != next;
  station = next;
  if (changed) {
    selectedRecipe.clear();
  }
  // Retargeting an open window is not a close: the crafting session's log and its
  // completion baseline survive. Reopening a closed window starts a fresh session.
  if (!wasOpen) {
    messages.clear();
    scrollMessages = false;
    seenCraftSerial = game != nullptr ? game->lastCraftMessageSerial : 0;
  }
}

void CraftWindow::AppendMessage(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  if (text.empty()) {
    return;
  }
  messages.push_back(std::move(text));
  scrollMessages = true;
}

void CraftWindow::Draw() {
  bool isOpen = true;
  std::string stationName = "Workstation";
  if (station.is_alive()) {
    if (const DisplayName *name = station.get<DisplayName>()) {
      stationName = name->name;
    }
  }
  const std::string title = "Craft: " + stationName + "###craft_window";
  ImGui::SetNextWindowSize(ImVec2(780, 460), ImGuiCond_FirstUseEver);
  ImGui::Begin(title.c_str(), &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    messages.clear();
    scrollMessages = false;
    if (game != nullptr && game->craftWindowEntity.is_alive()) {
      game->craftWindowEntity.remove<ActiveWindow>();
    }
    ImGui::End();
    return;
  }
  if (game == nullptr || !station.is_alive() || !station.has<Workstation>()) {
    messages.clear();
    ImGui::TextDisabled("This station is gone.");
    if (game != nullptr && game->craftWindowEntity.is_alive()) {
      game->craftWindowEntity.remove<ActiveWindow>();
    }
    ImGui::End();
    return;
  }

  // A timed craft finishes in CraftResolutionSystem, which records its outcome here. The
  // serial makes the window show each completion once, in the same message area as the
  // "started" / refusal lines.
  if (game->lastCraftMessageSerial != seenCraftSerial) {
    seenCraftSerial = game->lastCraftMessageSerial;
    AppendMessage(game->lastCraftMessage);
  }

  auto registryRes = game->ecs.get<RecipeRegistryResource>();
  const RecipeRegistry *registry =
      registryRes ? registryRes->registry : nullptr;
  if (registry == nullptr) {
    ImGui::TextDisabled("No recipes are loaded.");
    ImGui::End();
    return;
  }

  // The active timed craft, if any: its recipe drives the progress bar, and reading it
  // here means a craft started through the console or an old window still shows progress.
  const CraftRecipe *activeCraft = nullptr;
  float craftProgress = 0.0f;
  if (const Busy *activeBusy = game->playerEntity.get<Busy>()) {
    if (activeBusy->actionEntity.is_alive()) {
      if (const CraftAction *action =
              activeBusy->actionEntity.get<CraftAction>()) {
        activeCraft = registry->Get(action->recipeId);
        if (const ActionTimer *timer =
                activeBusy->actionEntity.get<ActionTimer>()) {
          craftProgress = std::clamp(timer->Progress(), 0.0f, 1.0f);
        }
      }
    }
  }

  const Workstation *workstation = station.get<Workstation>();
  const KnownRecipes *known = game->playerEntity.get<KnownRecipes>();
  const bool showAll = Settings().debug.craft.showAllRecipes;
  const bool ignoreIngredients = Settings().debug.craft.ignoreIngredients;

  // The station's list, filtered by what this PC has learned. Unknown recipes are hidden
  // unless the debug option says otherwise; either way only the station's own recipes are
  // ever shown, never the global registry.
  std::vector<const CraftRecipe *> recipes;
  for (const std::string &id : workstation->recipes) {
    const CraftRecipe *recipe = registry->Get(id);
    if (recipe == nullptr) {
      continue;
    }
    const bool isKnown = known != nullptr && known->Has(recipe->id);
    if (!showAll && !isKnown) {
      continue;
    }
    recipes.push_back(recipe);
  }

  const std::map<std::string, int> held = CountHeld(game->playerEntity);

  auto selected = [&]() -> const CraftRecipe * {
    for (const CraftRecipe *recipe : recipes) {
      if (recipe->id == selectedRecipe) {
        return recipe;
      }
    }
    return nullptr;
  };

  if (selected() == nullptr && !recipes.empty()) {
    selectedRecipe = recipes.front()->id;
  }

  ImGui::BeginChild("##recipe_list", ImVec2(260, 0), ImGuiChildFlags_Borders);
  if (recipes.empty()) {
    if (workstation->recipes.empty()) {
      ImGui::TextWrapped("This station cannot craft anything.");
    } else {
      ImGui::TextWrapped(
          "You don't know any recipes this station can make.");
      if (!showAll) {
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Turn on Settings → Debug → craft → show all recipes to see "
            "everything this station offers.");
      }
    }
  }
  for (const CraftRecipe *recipe : recipes) {
    const bool affordable =
        ignoreIngredients || Affordable(InputsFor(*recipe, held));
    // A recipe the player cannot afford stays listed and selectable; it is drawn in a
    // dimmer ink so the list reads at a glance.
    if (!affordable) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.50f, 0.55f, 1.0f));
    }
    const std::string label = recipe->name + "##" + recipe->id;
    if (ImGui::Selectable(label.c_str(), recipe->id == selectedRecipe)) {
      selectedRecipe = recipe->id;
    }
    if (!affordable) {
      ImGui::PopStyleColor();
    }
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##recipe_details", ImVec2(0, 0), ImGuiChildFlags_Borders);

  const CraftRecipe *recipe = selected();
  if (recipe == nullptr) {
    ImGui::TextDisabled("Select a recipe.");
    ImGui::EndChild();
    ImGui::End();
    return;
  }

  const std::vector<InputLine> inputs = InputsFor(*recipe, held);
  const bool hasMaterials = Affordable(inputs);
  const bool affordable = ignoreIngredients || hasMaterials;
  const bool isKnown = known != nullptr && known->Has(recipe->id);
  const bool busy = game->playerEntity.has<Busy>();

  // An item id resolves through the same icon/convention path the world uses for its look,
  // so a recipe ingredient and the crafted thing show the art the player will see in the
  // inventory. No texture means no image; the text still names it.
  const auto spriteFor = [&](const std::string &id) -> const Texture2D * {
    return IconPaths::TryResolveIconTexture(
        game->iconRegistry, game->resourceManager, id, game->debugLog.get());
  };

  // A craft that started from another selection stays visible: its own sprite and bar,
  // above the recipe the player is currently reading.
  if (activeCraft != nullptr && activeCraft->id != recipe->id) {
    const Texture2D *activeSprite =
        activeCraft->outputs.empty()
            ? nullptr
            : spriteFor(activeCraft->outputs.front().itemType);
    if (activeSprite != nullptr) {
      ImGui::Image(
          static_cast<ImTextureID>(static_cast<intptr_t>(activeSprite->id)),
          ImVec2(32.0f, 32.0f));
      ImGui::SameLine();
    }
    DrawCenteredCraftingBar(craftProgress, ImVec2(220.0f, 20.0f));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", activeCraft->name.c_str());
    ImGui::Separator();
  }

  // The crafted object gets the larger picture, beside its name. When this recipe is the
  // one running, the progress bar sits right next to that picture.
  const Texture2D *resultSprite =
      recipe->outputs.empty() ? nullptr : spriteFor(recipe->outputs.front().itemType);
  if (resultSprite != nullptr) {
    ImGui::Image(
        static_cast<ImTextureID>(static_cast<intptr_t>(resultSprite->id)),
        ImVec2(64.0f, 64.0f));
    ImGui::SameLine();
  }
  if (activeCraft != nullptr && activeCraft->id == recipe->id) {
    DrawCenteredCraftingBar(craftProgress, ImVec2(200.0f, 20.0f));
    ImGui::SameLine();
  }
  ImGui::BeginGroup();
  ImGui::Text("%s", recipe->name.c_str());
  ImGui::TextDisabled("%s", recipe->id.c_str());
  ImGui::EndGroup();
  ImGui::Separator();

  ImGui::TextUnformatted("Needs");
  for (const InputLine &line : inputs) {
    const bool enough = line.have >= line.need;
    const ImVec4 color = enough ? ImVec4(0.60f, 0.85f, 0.60f, 1.0f)
                                : ImVec4(0.95f, 0.55f, 0.45f, 1.0f);
    if (const Texture2D *sprite = spriteFor(line.id)) {
      ImGui::Image(
          static_cast<ImTextureID>(static_cast<intptr_t>(sprite->id)),
          ImVec2(24.0f, 24.0f));
      ImGui::SameLine();
    }
    ImGui::TextColored(color, "%s  %d / %d",
                       ResolveItemName(game->itemRegistry, line.id).c_str(),
                       line.have, line.need);
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Makes");
  for (const LootDrop &output : recipe->outputs) {
    if (const Texture2D *sprite = spriteFor(output.itemType)) {
      ImGui::Image(
          static_cast<ImTextureID>(static_cast<intptr_t>(sprite->id)),
          ImVec2(24.0f, 24.0f));
      ImGui::SameLine();
    }
    std::string line = ResolveItemName(game->itemRegistry, output.itemType);
    if (output.chance < 1.0f) {
      line += " (" + std::to_string(std::lround(output.chance * 100.0f)) +
              "% chance)";
    }
    ImGui::TextUnformatted(line.c_str());
  }

  if (recipe->craftTimeSeconds > 0.0f) {
    ImGui::Text("Time: %s s", FormatSeconds(recipe->craftTimeSeconds).c_str());
  } else {
    ImGui::TextUnformatted("Time: instant");
  }
  if (!recipe->trains.empty()) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Trains");
    const SkillRegistry *skills = nullptr;
    if (auto skillRes = game->ecs.get<SkillRegistryResource>()) {
      skills = skillRes->registry;
    }
    // A skill icon when the skill names one that resolves; otherwise the generic
    // Production gear, so a crafting skill is never iconless in this list.
    const auto skillIconFor = [&](const std::string &skillId) -> const Texture2D * {
      const SkillDef *definition = skills != nullptr ? skills->Get(skillId) : nullptr;
      const Texture2D *icon = nullptr;
      if (definition != nullptr && !definition->icon.empty()) {
        icon = IconPaths::TryResolveIconTexture(
            game->iconRegistry, game->resourceManager, definition->icon,
            game->debugLog.get());
      }
      if (icon == nullptr) {
        icon = IconPaths::TryResolveIconTexture(
            game->iconRegistry, game->resourceManager, "production",
            game->debugLog.get());
      }
      return icon;
    };
    for (const SkillXp &train : recipe->trains) {
      const SkillDef *definition =
          skills != nullptr ? skills->Get(train.skillId) : nullptr;
      if (const Texture2D *icon = skillIconFor(train.skillId)) {
        ImGui::Image(
            static_cast<ImTextureID>(static_cast<intptr_t>(icon->id)),
            ImVec2(18.0f, 18.0f));
        ImGui::SameLine();
      }
      const std::string name =
          definition != nullptr && !definition->name.empty()
              ? definition->name
              : train.skillId;
      ImGui::Text("%s +%d", name.c_str(), train.xp);
    }
  }

  if (!isKnown) {
    ImGui::Spacing();
    ImGui::TextDisabled(
        "Not known by the PC; listed because Settings → Debug → craft → show "
        "all recipes is on.");
  }

  ImGui::Spacing();
  ImGui::Separator();

  // The button is disabled exactly when the craft would be refused for a reason the
  // player can fix: missing materials, or already busy. The interaction still owns the
  // authoritative check (and the timed-action bookkeeping).
  ImGui::BeginDisabled(!affordable || busy);
  if (ImGui::Button("Craft", ImVec2(160.0f, 0.0f))) {
    if (!game->WithinAccessRange(station)) {
      game->ShowPlayerNotice("Too far away");
      AppendMessage("You moved too far from the " + stationName + ".");
    } else {
      for (const InteractionHandler &handler :
           InteractionRegistry::GetAvailableInteractions(station)) {
        if (handler.name == "Craft") {
          AppendMessage(handler.execute(game->playerEntity, station, recipe->id));
          break;
        }
      }
    }
  }
  ImGui::EndDisabled();

  if (!hasMaterials && !ignoreIngredients) {
    ImGui::TextDisabled("Missing: %s",
                        MissingSummary(inputs, game->itemRegistry).c_str());
  } else if (!hasMaterials && ignoreIngredients) {
    ImGui::TextDisabled(
        "Debug: missing ingredients are ignored (Settings → Debug → craft).");
  } else if (busy) {
    ImGui::TextDisabled("You are busy doing something else.");
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("System messages");
  ImGui::BeginChild("##craft_messages", ImVec2(0, 120.0f),
                    ImGuiChildFlags_Borders);
  if (messages.empty()) {
    ImGui::TextDisabled("Nothing crafted in this session yet.");
  } else {
    for (const std::string &message : messages) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextWrapped("%s", message.c_str());
      ImGui::PopTextWrapPos();
    }
  }
  if (scrollMessages) {
    ImGui::SetScrollHereY(1.0f);
    scrollMessages = false;
  }
  ImGui::EndChild();

  ImGui::EndChild();
  ImGui::End();
}
