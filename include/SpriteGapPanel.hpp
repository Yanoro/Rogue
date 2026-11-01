#pragma once

// The "Sprites" section under a building preview: what this design cannot be drawn
// with, and a button per item that asks the sprite designer for it.
//
// Three different reasons an item appears, in the order they are shown, because they
// mean different things:
//
//   * **requested** -- the designer itself said it needs this asset (`needs`). This is
//     the real trigger: the model is the one who knows what the building needs, and the
//     description it wrote is the brief the sprite designer works from.
//   * **not in the world** -- the design names a material the catalog does not define
//     without asking for it. Still worth offering, because a hand-written design can do
//     this and the building has a hole either way.
//   * **no art yet** -- the definition exists and the pack has no sprite for it, so the
//     world falls back to a glyph. The building is complete; it just cannot be drawn.
//     This is the common case (see data/gaps.json).
//
// What this does NOT do is decide anything: it offers, and the Sprite Designer shows the
// crop and the style read before anything is sent.
//
// One implementation, two previews. The Designer window and the Blueprints window both
// show a design, and a section that existed in only one of them reads as a missing
// feature rather than as a different window -- which is how it was first reported.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "Blueprint.hpp"
#include "BlueprintExpansion.hpp"
#include "BlueprintNeed.hpp"
#include "BlueprintRegistry.h"
#include "Game.h"
#include "ObjectFactory.h"
#include "SpriteGapOffer.hpp"
#include "SpritePaths.hpp"
#include "TileRegistry.h"
#include "imgui.h"

namespace SpriteGapPanel {

// One material the design uses, and which op names it.
struct Use {
  std::string kind;  // "paint" (ground tile) or "place" (object)
  std::string name;
};

inline void Draw(Game *game, const TileRegistry &tiles, const ObjectFactory &objects,
                 const Blueprint &design,
                 const std::vector<BlueprintNeed> &needs = {}) {
  if (game == nullptr) return;

  ImGui::Separator();
  ImGui::Text("Sprites");

  // One line per asset, whichever list found it first.
  std::set<std::string> listed;
  int drawn = 0;

  const auto drawOffer = [&](const std::string &name, const std::string &kind,
                             bool isVocabularyGap, const std::string &description) {
    ImGui::PushID(("offer|" + kind + "|" + name).c_str());
    ImGui::BulletText("%s  %s", name.c_str(),
                      isVocabularyGap ? "(not in the world)" : "(no art yet)");
    ImGui::SameLine();

    const SpriteGapOffer *active = game->ActiveSpriteGap();
    const bool alreadyOpen = active != nullptr && active->Active() &&
                             active->Gap().material == name && active->Gap().kind == kind;
    ImGui::BeginDisabled(alreadyOpen);
    if (ImGui::SmallButton(alreadyOpen ? "Open in Sprite Designer" : "Design sprite")) {
      MissingMaterial gap;
      gap.material = name;
      gap.kind = kind;
      // Only an asset the world lacks is drawn as a hole; one that exists and lacks art
      // is drawn as itself, because the building is already complete without it.
      game->BeginSpriteGap(design, gap, isVocabularyGap, description);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          alreadyOpen
              ? "This one is already open in the Sprite Designer."
              : "Render this building, read the art around it, and open the Sprite\n"
                "Designer with it. Nothing is sent until you press Send there.");
    }
    if (!description.empty()) {
      // The designer's own words, wrapped under the button: what the sprite designer
      // will be told it is. Shown here so the request can be judged before sending it.
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("    %s", description.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::PopID();
    ++drawn;
  };

  // 1. What the designer asked for.
  for (const BlueprintNeed &need : needs) {
    // The dedupe key has to use the *op* spelling, not the need's own vocabulary. A need
    // says "object"; a part says "place". Keying on different words let an asset that is
    // both requested and named -- which is the normal case, since the design places what
    // it asked for -- appear twice, under two identical ImGui IDs.
    const std::string opKind = need.kind == "tile" ? "paint" : "place";
    const std::string key = opKind + "|" + need.name;
    if (listed.count(key) != 0) continue;
    listed.insert(key);

    const bool exists = need.kind == "tile"
                            ? tiles.Get(need.name) != nullptr
                            : objects.GetTemplates().count(need.name) != 0;
    if (exists) {
      // Already there, so there is nothing to draw -- but say so, because a request that
      // silently disappears is worse than one that says why.
      ImGui::BulletText("%s  (already exists)", need.name.c_str());
      ++drawn;
      continue;
    }
    drawOffer(need.name, opKind, true, need.description);
  }

  // 2. Named by the design and absent from the world, whether or not it was requested.
  const std::vector<MissingMaterial> gaps =
      FindMissingVocabulary(design, &tiles, &objects);
  for (const MissingMaterial &gap : gaps) {
    const std::string key = gap.kind + "|" + gap.material;
    if (listed.count(key) != 0) continue;
    listed.insert(key);
    // No description: the design said the name and nothing else. The sprite designer
    // still has the crop and the style read to work from.
    drawOffer(gap.material, gap.kind, true, std::string());
  }

  // 3. Used by the design, defined, and unarted.
  std::map<std::string, Use> used;
  const BlueprintExpansion::Expansion expansion = BlueprintExpansion::Expand(design);
  for (const BlueprintExpansion::ExpandedCell &cell : expansion.cells) {
    if (!cell.tile.empty()) {
      used["paint|" + cell.tile] = {"paint", cell.tile};
    }
    if (!cell.object.empty() && !cell.carve) {
      used["place|" + cell.object] = {"place", cell.object};
    }
  }

  std::vector<Use> missingArt;
  for (const auto &entry : used) {
    const Use &use = entry.second;
    if (listed.count(entry.first) != 0) continue;

    std::string explicitPath;
    std::string conventionId;
    if (use.kind == "paint") {
      const TileDef *definition = tiles.Get(use.name);
      if (definition != nullptr) explicitPath = definition->sprite;
      conventionId = TileSpriteId(use.name);
    } else {
      const auto &templates = objects.GetTemplates();
      const auto found = templates.find(use.name);
      if (found != templates.end() && found->second.is_object()) {
        explicitPath = found->second.value("sprite", std::string());
      }
      conventionId = use.name;
    }
    if (!SpriteResolves(explicitPath, conventionId)) {
      listed.insert(entry.first);
      missingArt.push_back(use);
    }
  }

  if (!missingArt.empty()) {
    ImGui::TextDisabled("Used by this design, but the art pack has no sprite:");
    for (const Use &use : missingArt) {
      // No description and nothing missing: only the drawing is absent.
      drawOffer(use.name, use.kind, false, std::string());
    }
  }

  if (drawn == 0) {
    ImGui::TextDisabled(
        "Nothing to design: every material this design uses exists and has art.");
  }
}

}  // namespace SpriteGapPanel
