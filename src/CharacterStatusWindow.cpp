#include "CharacterStatusWindow.h"

#include <cfloat>
#include <cstdio>
#include <string>

#include "Components.h"
#include "SkillRegistry.h"
#include "StatRegistry.h"
#include "imgui.h"

namespace {

// Wrapped hover tooltip for whatever was drawn last.
//
// The descriptions are whole sentences, so an unwrapped tooltip would run off the
// edge of the screen; wrapping at roughly thirty characters' width keeps it
// readable.
void DescriptionTooltip(const std::string &description) {
  if (description.empty() || !ImGui::IsItemHovered()) {
    return;
  }
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
  ImGui::TextUnformatted(description.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

} // namespace

CharacterStatusWindow::CharacterStatusWindow(flecs::entity character,
                                             flecs::entity owner)
    : character(character), owner(owner) {}

void CharacterStatusWindow::Draw() {
  const bool alive = character.is_alive();
  const std::string name =
      alive && character.has<DisplayName>() ? character.get<DisplayName>()->name
                                            : "Unknown";

  // The ### suffix pins the window's identity to the character, so two status
  // windows can be open at once and each keeps its own size and position.
  char title[192];
  std::snprintf(title, sizeof(title), "Status: %s###character_status_%llu",
                name.c_str(),
                static_cast<unsigned long long>(alive ? character.id() : 0));

  bool isOpen = true;
  // Sized for a stat list plus a skill list with their descriptions; the user can
  // resize freely afterwards.
  ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
  ImGui::Begin(title, &isOpen, ImGuiWindowFlags_None);

  if (!isOpen) {
    // The carrier keeps its CharacterStatusWindowTarget so reopening reuses it,
    // the same way a closed inventory window does.
    if (owner.is_alive()) {
      owner.remove<ActiveWindow>();
    }
    ImGui::End();
    return;
  }

  if (!alive) {
    ImGui::TextDisabled("This character is gone.");
    ImGui::End();
    return;
  }

  ImGui::Text("Name: %s", name.c_str());

  const StatRegistry *statRegistry = nullptr;
  if (auto registryRes = character.world().get<StatRegistryResource>()) {
    statRegistry = registryRes->registry;
  }
  const SkillRegistry *skillRegistry = nullptr;
  if (auto registryRes = character.world().get<SkillRegistryResource>()) {
    skillRegistry = registryRes->registry;
  }

  // --- Stats ---------------------------------------------------------------
  ImGui::SeparatorText("Stats");
  if (const StatBlock *block = character.get<StatBlock>()) {
    ImGui::TextDisabled("(hover a stat for what it does)");
    for (const StatValue &value : block->values) {
      const StatDef *def = statRegistry ? statRegistry->Get(value.id) : nullptr;
      const std::string label = def ? def->name : value.id;

      ImGui::Text("%s: %g", label.c_str(),
                  static_cast<double>(value.value));
      DescriptionTooltip(def ? def->description : "");
    }
  } else {
    ImGui::TextDisabled("(none)");
  }

  // --- Skills --------------------------------------------------------------
  // Stage first, level as the progress bar, and the description spelled out
  // rather than hidden behind a hover: a skill's stage is the part that carries
  // capability, so it should not need discovering (section 9.1).
  ImGui::SeparatorText("Skills");
  if (const SkillBlock *block = character.get<SkillBlock>()) {
    for (const SkillValue &value : block->values) {
      const SkillDef *def =
          skillRegistry ? skillRegistry->Get(value.id) : nullptr;

      if (!def) {
        ImGui::Text("%s: level %d", value.id.c_str(), value.level);
        continue;
      }

      ImGui::Text("%s: %s %d/%d", def->name.c_str(),
                  def->StageName(def->StageForLevel(value.level)).c_str(),
                  def->LevelWithinStage(value.level), def->levelsPerStage);

      // Progress toward the NEXT level, so the bar refills at every level rather
      // than tracking the whole 45-level climb -- which would look motionless for
      // most of a stage and say nothing about how close the next level is.
      //
      // The exact numbers stay on the bar as its overlay, because a bar alone
      // cannot be read to the point, and XpForNextLevel is what the progression
      // itself uses, so the bar can never disagree with when the level actually
      // turns over.
      const int needed = def->XpForNextLevel(value.level);
      const float fraction =
          needed > 0
              ? static_cast<float>(value.xp) / static_cast<float>(needed)
              : 1.0f;

      char overlay[96];
      if (needed > 0) {
        std::snprintf(overlay, sizeof(overlay), "%d / %d xp", value.xp, needed);
      } else {
        // At the cap there is no next level, so the bar is simply full and says
        // why. The stage name comes from the skill itself rather than being
        // hardcoded, because a skill may declare any stages it likes.
        std::snprintf(overlay, sizeof(overlay), "%s %d/%d - maxed",
                      def->StageName(def->StageForLevel(value.level)).c_str(),
                      def->LevelWithinStage(value.level), def->levelsPerStage);
      }

      ImGui::Indent();
      ImGui::ProgressBar(fraction,
                         ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), overlay);
      ImGui::Unindent();

      if (!def->description.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::Indent();
        ImGui::TextWrapped("%s", def->description.c_str());
        ImGui::Unindent();
        ImGui::PopStyleColor();
      }
    }
  } else {
    ImGui::TextDisabled("(none)");
  }

  ImGui::End();
}
