#define IMGUI_DEFINE_MATH_OPERATORS
#include "Game.h"
#include "IconPaths.hpp"

#include "Components.h"
#include "CombatResolver.hpp"
#include "ResourceSystem.h"
#include "PortraitResources.hpp"
#include "IconPaths.hpp"
#include "Defaults.h"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

constexpr int kSlotCount = 12;
constexpr float kSlotSize = 54.0f;
constexpr float kSlotGap = 4.0f;
constexpr float kBarPad = 10.0f;
constexpr float kBarTop = 12.0f;
constexpr float kPortraitSize = 72.0f;

// One palette per procedural bar skin. The three skins are not three layouts:
// the row, the slots and the keybinds are identical, and only the frame drawing
// changes. That is deliberate -- a player switching skins should not have to
// relearn where the buttons are.
struct BarPalette {
  ImU32 frameFill;
  ImU32 frameEdge;
  ImU32 frameInner;
  ImU32 slotFill;
  ImU32 slotInner;
  ImU32 slotEdge;
  ImU32 accent;
  ImU32 key;
  ImU32 name;
  ImU32 pip;
};

BarPalette PaletteFor(int style) {
  switch (style) {
    case 1:  // Ironclad: cold steel, red accent, riveted.
      return {IM_COL32(22, 26, 30, 235), IM_COL32(88, 98, 108, 255),
              IM_COL32(38, 44, 50, 255), IM_COL32(28, 33, 38, 255),
              IM_COL32(40, 46, 52, 255), IM_COL32(88, 98, 108, 255),
              IM_COL32(176, 54, 48, 255), IM_COL32(214, 220, 226, 255),
              IM_COL32(226, 230, 234, 255), IM_COL32(120, 220, 120, 255)};
    case 2:  // Runic: slate, chamfered, teal glow.
      return {IM_COL32(18, 20, 25, 235), IM_COL32(62, 74, 88, 255),
              IM_COL32(34, 40, 48, 255), IM_COL32(22, 26, 32, 255),
              IM_COL32(32, 38, 46, 255), IM_COL32(74, 88, 102, 255),
              IM_COL32(88, 190, 178, 255), IM_COL32(176, 206, 200, 255),
              IM_COL32(210, 222, 226, 255), IM_COL32(110, 210, 190, 255)};
    default:  // Warband: iron and stone, gold keybinds, winged end caps.
      return {IM_COL32(26, 23, 20, 235), IM_COL32(126, 110, 82, 255),
              IM_COL32(44, 38, 31, 255), IM_COL32(30, 27, 23, 255),
              IM_COL32(42, 38, 32, 255), IM_COL32(98, 86, 64, 255),
              IM_COL32(200, 170, 110, 255), IM_COL32(214, 190, 140, 255),
              IM_COL32(224, 214, 194, 255), IM_COL32(126, 218, 126, 255)};
  }
}

const char *StyleName(int style) {
  switch (style) {
    case 1: return "Ironclad";
    case 2: return "Runic";
    default: return "Warband";
  }
}

const Texture2D *PortraitTexture(flecs::entity entity,
                                 const PortraitRegistry &portraits,
                                 ResourceManager &resources, DebugLog *log) {
  if (const Portrait *portrait = entity.get<Portrait>()) {
    if (const PortraitDef *def = portraits.Get(portrait->id)) {
      if (const Texture2D *texture = ResolveSpriteReference(def->sprite, resources, log)) {
        return texture;
      }
    }
  }
  return ResolveSpriteReference(UnknownSpritePath(), resources, log);
}

std::string EntityName(flecs::entity entity) {
  if (const DisplayName *name = entity.get<DisplayName>()) {
    if (!name->name.empty()) {
      return name->name;
    }
  }
  if (const char *name = entity.name().c_str()) {
    if (name[0] != '\0') {
      return std::string(name);
    }
  }
  return "Unknown";
}

// A chamfered (eight-sided) polygon inscribed in a rect, inset by `cut`.
void ChamferPoints(ImVec2 a, ImVec2 b, float cut, ImVec2 out[8]) {
  out[0] = ImVec2(a.x + cut, a.y);
  out[1] = ImVec2(b.x - cut, a.y);
  out[2] = ImVec2(b.x, a.y + cut);
  out[3] = ImVec2(b.x, b.y - cut);
  out[4] = ImVec2(b.x - cut, b.y);
  out[5] = ImVec2(a.x + cut, b.y);
  out[6] = ImVec2(a.x, b.y - cut);
  out[7] = ImVec2(a.x, a.y + cut);
}

void DrawSlotFrame(ImDrawList *dl, ImVec2 a, ImVec2 b, int style, bool filled,
                   bool hovered, bool ready) {
  const BarPalette c = PaletteFor(style);
  const ImU32 edge = hovered ? c.accent : (ready ? c.slotEdge : c.slotEdge);

  if (style == 2) {
    ImVec2 outer[8];
    ImVec2 inner[8];
    ChamferPoints(a, b, 6.0f, outer);
    ChamferPoints(a + ImVec2(2, 2), b - ImVec2(2, 2), 5.0f, inner);
    dl->AddConvexPolyFilled(outer, 8, c.slotFill);
    dl->AddConvexPolyFilled(inner, 8, filled ? c.slotInner : c.slotFill);
    dl->AddPolyline(outer, 8, edge, ImDrawFlags_Closed, hovered ? 2.0f : 1.5f);
    if (filled && ready) {
      dl->AddPolyline(inner, 8, c.accent, ImDrawFlags_Closed, 1.0f);
    }
  } else {
    const float rounding = style == 1 ? 3.0f : 1.0f;
    dl->AddRectFilled(a, b, c.slotFill, rounding);
    dl->AddRectFilled(a + ImVec2(2, 2), b - ImVec2(2, 2),
                      filled ? c.slotInner : c.slotFill, rounding);
    dl->AddRect(a, b, edge, rounding, 0, hovered ? 2.0f : 1.5f);

    if (style == 1) {
      // Rivets: the one ornament the Ironclad skin gets.
      const ImU32 rivet = IM_COL32(150, 156, 162, 255);
      dl->AddCircleFilled(ImVec2(a.x + 4.0f, a.y + 4.0f), 1.2f, rivet);
      dl->AddCircleFilled(ImVec2(b.x - 4.0f, a.y + 4.0f), 1.2f, rivet);
      dl->AddCircleFilled(ImVec2(a.x + 4.0f, b.y - 4.0f), 1.2f, rivet);
      dl->AddCircleFilled(ImVec2(b.x - 4.0f, b.y - 4.0f), 1.2f, rivet);
    } else {
      // Warband: a gold tick on each corner reads as a forged frame.
      const ImU32 tick = IM_COL32(150, 130, 88, 180);
      const float t = 5.0f;
      dl->AddLine(ImVec2(a.x + 2, a.y + 2), ImVec2(a.x + t, a.y + 2), tick, 1.0f);
      dl->AddLine(ImVec2(a.x + 2, a.y + 2), ImVec2(a.x + 2, a.y + t), tick, 1.0f);
      dl->AddLine(ImVec2(b.x - 2, a.y + 2), ImVec2(b.x - t, a.y + 2), tick, 1.0f);
      dl->AddLine(ImVec2(b.x - 2, a.y + 2), ImVec2(b.x - 2, a.y + t), tick, 1.0f);
      dl->AddLine(ImVec2(a.x + 2, b.y - 2), ImVec2(a.x + t, b.y - 2), tick, 1.0f);
      dl->AddLine(ImVec2(a.x + 2, b.y - 2), ImVec2(a.x + 2, b.y - t), tick, 1.0f);
      dl->AddLine(ImVec2(b.x - 2, b.y - 2), ImVec2(b.x - t, b.y - 2), tick, 1.0f);
      dl->AddLine(ImVec2(b.x - 2, b.y - 2), ImVec2(b.x - 2, b.y - t), tick, 1.0f);
    }
  }

  if (!filled) {
    // An empty slot is a visible frame, not a hole: the reference bar shows its
    // empty slots, and so does this one. The dot is the "unassigned" mark.
    dl->AddCircleFilled(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), 1.5f,
                        IM_COL32(96, 90, 80, 110));
  }
}

void DrawBarFrame(ImDrawList *dl, ImVec2 origin, ImVec2 size, int style) {
  const BarPalette c = PaletteFor(style);
  const ImVec2 end = origin + size;
  const float extraSide = style == 0 ? 24.0f : 0.0f;

  if (style == 2) {
    ImVec2 outer[8];
    ChamferPoints(origin, end, 10.0f, outer);
    dl->AddConvexPolyFilled(outer, 8, c.frameFill);
    dl->AddPolyline(outer, 8, c.frameEdge, ImDrawFlags_Closed, 2.0f);

    // Rune ticks along the top edge: short vertical strokes, evenly spaced.
    for (float x = origin.x + 30.0f; x < end.x - 30.0f; x += 34.0f) {
      dl->AddLine(ImVec2(x, origin.y + 5.0f), ImVec2(x, origin.y + 10.0f),
                  IM_COL32(88, 190, 178, 90), 1.0f);
      dl->AddLine(ImVec2(x + 6.0f, origin.y + 7.0f),
                  ImVec2(x + 6.0f, origin.y + 11.0f),
                  IM_COL32(88, 190, 178, 60), 1.0f);
    }
    dl->AddRect(origin + ImVec2(4, 4), end - ImVec2(4, 4),
                IM_COL32(88, 190, 178, 40), 7.0f, 0, 1.0f);
  } else {
    const ImVec2 innerA = origin + ImVec2(extraSide + 3.0f, 3.0f);
    const ImVec2 innerB = end - ImVec2(extraSide + 3.0f, 3.0f);
    dl->AddRectFilled(origin, end, c.frameFill, style == 1 ? 6.0f : 3.0f);
    dl->AddRect(origin, end, c.frameEdge, style == 1 ? 6.0f : 3.0f, 0, 2.0f);
    dl->AddRect(innerA, innerB, c.frameInner, style == 1 ? 4.0f : 1.0f, 0, 1.0f);

    if (style == 1) {
      // A thin red line is the Ironclad signature; the end bolts hold it down.
      dl->AddRectFilled(ImVec2(origin.x + 10.0f, origin.y + 3.0f),
                        ImVec2(end.x - 10.0f, origin.y + 5.0f), c.accent);
      for (float x : {origin.x + 7.0f, end.x - 7.0f}) {
        dl->AddCircleFilled(ImVec2(x, (origin.y + end.y) * 0.5f), 2.5f,
                            IM_COL32(52, 60, 68, 255));
        dl->AddCircle(ImVec2(x, (origin.y + end.y) * 0.5f), 2.5f,
                      IM_COL32(120, 132, 144, 255), 8, 1.0f);
      }
    } else {
      // Warband: mirrored wings at the ends and a diamond boss on top.
      // The wing reaches back by less than the side reserve, so it cannot be
      // clipped by the window edge; the boss sits inside the top strip rather
      // than straddling the window's top border.
      const float midY = (origin.y + end.y) * 0.5f;
      const float wing = extraSide - 2.0f;
      ImVec2 leftWing[5] = {
          ImVec2(origin.x + extraSide, origin.y + 8.0f),
          ImVec2(origin.x + extraSide - wing * 0.55f, origin.y + 4.0f),
          ImVec2(origin.x + extraSide - wing, midY),
          ImVec2(origin.x + extraSide - wing * 0.55f, end.y - 4.0f),
          ImVec2(origin.x + extraSide, end.y - 8.0f)};
      ImVec2 rightWing[5] = {
          ImVec2(end.x - extraSide, origin.y + 8.0f),
          ImVec2(end.x - extraSide + wing * 0.55f, origin.y + 4.0f),
          ImVec2(end.x - extraSide + wing, midY),
          ImVec2(end.x - extraSide + wing * 0.55f, end.y - 4.0f),
          ImVec2(end.x - extraSide, end.y - 8.0f)};
      dl->AddConvexPolyFilled(leftWing, 5, IM_COL32(38, 33, 28, 255));
      dl->AddPolyline(leftWing, 5, c.frameEdge, ImDrawFlags_Closed, 1.5f);
      dl->AddConvexPolyFilled(rightWing, 5, IM_COL32(38, 33, 28, 255));
      dl->AddPolyline(rightWing, 5, c.frameEdge, ImDrawFlags_Closed, 1.5f);

      const ImVec2 boss((origin.x + end.x) * 0.5f, origin.y + 8.0f);
      const ImVec2 diamond[4] = {ImVec2(boss.x, boss.y - 6.0f),
                                 ImVec2(boss.x + 7.0f, boss.y + 1.0f),
                                 ImVec2(boss.x, boss.y + 7.0f),
                                 ImVec2(boss.x - 7.0f, boss.y + 1.0f)};
      dl->AddConvexPolyFilled(diamond, 4, IM_COL32(52, 45, 36, 255));
      dl->AddPolyline(diamond, 4, c.frameEdge, ImDrawFlags_Closed, 1.5f);
      dl->AddCircleFilled(boss, 1.8f, c.accent);
    }
  }
}

}  // namespace

void Game::DrawCombatHud() {
  if (!playerEntity.is_alive()) {
    return;
  }

  const bool firstFrame = !combatHudDrawn;
  combatHudDrawn = true;

  if (!hudStateLoaded) {
    LoadCombatHudState();
  }

  // The HUD lives inside the game's 1920x1080 presentation rectangle, not the
  // window's reported size. The world is blitted at a fixed size and a window
  // taller than the world leaves black space below it; anchoring to the window's
  // bottom would put the HUD in that space.
  const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
  const float visibleWidth = std::min({displaySize.x,
                                       static_cast<float>(GetScreenWidth()),
                                       DEFAULT_PRESENTATION_WIDTH});
  const float visibleHeight = std::min({displaySize.y,
                                        static_cast<float>(GetScreenHeight()),
                                        DEFAULT_PRESENTATION_HEIGHT});

  const bool reset = resetCombatHudPosition;
  const ImGuiCond positionCondition =
      reset ? ImGuiCond_Always : ImGuiCond_FirstUseEver;

  // A saved position that no longer fits the window is discarded, not clamped: a
  // panel saved on a larger monitor would otherwise be unreachable, and F10 is
  // the in-game way back.
  auto panelFits = [&](float x, float y) {
    return x >= 0.0f && y >= 0.0f && x <= visibleWidth - 80.0f &&
           y <= visibleHeight - 40.0f;
  };

  auto drawResourceRow = [&](flecs::entity entity, const std::string &id,
                             bool showIcon, bool showName, bool showValue,
                             float barHeight, bool compact) {
    const ResourceDef *definition = resourceRegistry.Get(id);
    if (definition == nullptr || definition->hidden) return;
    const float maximum = Resources::Max(entity, id);
    if (maximum <= 0.0f) return;
    const float current = Resources::Current(entity, id);
    const float ratio = std::clamp(current / maximum, 0.0f, 1.0f);
    const std::string labelText = definition->name.empty() ? id : definition->name;

    if (showIcon && !definition->icon.empty()) {
      if (const Texture2D *icon = IconPaths::ResolveIconTexture(
              iconRegistry, resourceManager, definition->icon,
              debugLog.get())) {
        ImGui::Image(static_cast<ImTextureID>(
                         static_cast<intptr_t>(icon->id)),
                     ImVec2(compact ? 14.0f : 18.0f,
                            compact ? 14.0f : 18.0f));
        ImGui::SameLine();
      }
    }

    std::string barLabel;
    if (showName) barLabel = labelText;
    if (showValue) {
      char numbers[48];
      std::snprintf(numbers, sizeof(numbers), "%.0f / %.0f", current, maximum);
      if (!barLabel.empty()) barLabel += "  ";
      barLabel += numbers;
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
                          ImVec4(0.06f, 0.06f, 0.08f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                          definition->shield
                              ? ImVec4(0.55f, 0.42f, 0.85f, 1.0f)
                              : (id == "health"
                                     ? ImVec4(0.68f, 0.14f, 0.14f, 1.0f)
                                     : ImVec4(0.25f, 0.48f, 0.82f, 1.0f)));
    ImGui::ProgressBar(ratio, ImVec2(compact ? 130.0f : 150.0f, barHeight),
                       barLabel.empty() ? nullptr : barLabel.c_str());
    ImGui::PopStyleColor(2);
  };

  auto drawPortraitPanel = [&](const char *windowId, flecs::entity entity,
                               bool hostile, float &x, float &y,
                               ImVec2 fallback) {
    // A negative coordinate means "never positioned" -- the target panel in
    // particular is not drawn until a target exists, so the global first frame
    // is not the first frame it appears on.
    if (reset || x < 0.0f || y < 0.0f || (firstFrame && !panelFits(x, y))) {
      x = fallback.x;
      y = fallback.y;
    }
    ImGui::SetNextWindowPos(ImVec2(x, y), positionCondition);
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin(windowId, nullptr, flags)) {
      const ImVec2 origin = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(kPortraitSize, kPortraitSize));
      ImDrawList *draw = ImGui::GetWindowDrawList();
      const ImVec2 end = origin + ImVec2(kPortraitSize, kPortraitSize);

      draw->AddRectFilled(origin, end, IM_COL32(18, 16, 14, 255), 3.0f);
      if (const Texture2D *texture =
              PortraitTexture(entity, portraitRegistry, resourceManager,
                              debugLog.get())) {
        DrawSpriteImGui(*texture, origin + ImVec2(4, 4),
                        ImVec2(kPortraitSize - 8.0f, kPortraitSize - 8.0f));
      }
      draw->AddRect(origin, end, IM_COL32(154, 132, 92, 255), 3.0f, 0, 2.0f);
      draw->AddRect(origin + ImVec2(2, 2), end - ImVec2(2, 2),
                    IM_COL32(64, 55, 42, 255), 2.0f, 0, 1.0f);

      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::TextUnformatted(EntityName(entity).c_str());
      Resources::EnsureInitialized(entity);
      if (entity == playerEntity) {
        const GameSettings::HudSection::PortraitSection &portrait =
            Settings().hud.portrait;
        const ResourcePools *pools = entity.get<ResourcePools>();
        const std::vector<PortraitResourceEntry> entries =
            PortraitResources::Build(pools, portrait.resources);
        for (const PortraitResourceEntry &entry : entries) {
          if (!entry.visible) continue;
          drawResourceRow(entity, entry.id, portrait.showIcons && entry.showIcon,
                          portrait.showNames, portrait.showValues,
                          portrait.compact ? std::max(6.0f, portrait.barHeight - 2.0f)
                                           : portrait.barHeight,
                          portrait.compact);
        }
      } else {
        const std::string vital = Resources::PrimaryVital(entity);
        if (!vital.empty()) {
          drawResourceRow(entity, vital, false, false, true, 14.0f, false);
        }
        if (const ResourcePools *pools = entity.get<ResourcePools>()) {
          for (const ResourcePool &pool : pools->pools) {
            if (pool.id == vital) continue;
            if (pool.current <= 0.0f && !pool.shield) continue;
            drawResourceRow(entity, pool.id, false, true, true, 11.0f, true);
          }
        }
      }

      // A cast is the decision window: the player needs to see their own
      // wind-up, and the target's, because that is when an interrupt matters.
      if (const Casting *casting = entity.get<Casting>()) {
        if (casting->actionEntity.is_alive()) {
          if (const ActionTimer *timer =
                  casting->actionEntity.get<ActionTimer>()) {
            char castLabel[96];
            std::snprintf(castLabel, sizeof(castLabel), "%s %.1fs",
                          casting->label.c_str(), timer->timeRemaining);
            ImGui::PushStyleColor(
                ImGuiCol_FrameBg, ImVec4(0.06f, 0.06f, 0.08f, 1.0f));
            ImGui::PushStyleColor(
                ImGuiCol_PlotHistogram,
                hostile ? ImVec4(0.85f, 0.42f, 0.12f, 1.0f)
                        : ImVec4(0.30f, 0.52f, 0.88f, 1.0f));
            ImGui::ProgressBar(timer->Progress(), ImVec2(150.0f, 12.0f),
                               castLabel);
            ImGui::PopStyleColor(2);
            if (hostile) {
              ImGui::TextDisabled("%s", casting->interruptible
                                            ? "Interruptible"
                                            : "Uninterruptible");
            }
          }
        }
      }

      const float armour = ArmorOf(entity, &itemRegistry);
      if (armour >= 0.5f) {
        ImGui::TextDisabled("Armour %d",
                            static_cast<int>(std::lround(armour)));
      }
      ImGui::EndGroup();

      x = ImGui::GetWindowPos().x;
      y = ImGui::GetWindowPos().y;
    }
    ImGui::End();
    ImGui::PopStyleVar();
  };

  drawPortraitPanel("##PlayerPortrait", playerEntity, false, playerHudX,
                    playerHudY, ImVec2(24.0f, 24.0f));

  // The target panel only exists while there is a living target. Clicking an
  // attackable entity sets LockedTarget; the panel reads the same component
  // combat resolves against, so the numbers cannot disagree.
  flecs::entity target = flecs::entity::null();
  if (const LockedTarget *lock = playerEntity.get<LockedTarget>()) {
    target = lock->target;
  }
  if (target.is_alive() && target != playerEntity &&
      Resources::IsDamageable(target)) {
    drawPortraitPanel("##TargetPortrait", target, true, targetHudX, targetHudY,
                      ImVec2((visibleWidth - 250.0f) * 0.5f, 24.0f));
  }

  // --- Ability bar -----------------------------------------------------------
  const SkillBar *bar = playerEntity.get<SkillBar>();
  if (bar != nullptr) {
    const int style = std::clamp(abilityBarStyle, 0, 2);
    const float extraSide = style == 0 ? 24.0f : 0.0f;
    const float contentWidth =
        kSlotCount * kSlotSize + (kSlotCount - 1) * kSlotGap;
    const float totalWidth = contentWidth + 2.0f * kBarPad + 2.0f * extraSide;
    const float totalHeight = kSlotSize + 2.0f * kBarPad + kBarTop;
    const ImVec2 fallback((visibleWidth - totalWidth) * 0.5f,
                          visibleHeight - totalHeight - 20.0f);

    if (reset || (firstFrame && !panelFits(abilityBarX, abilityBarY))) {
      abilityBarX = fallback.x;
      abilityBarY = fallback.y;
    }
    if (abilityBarX < 0.0f || abilityBarY < 0.0f) {
      abilityBarX = fallback.x;
      abilityBarY = fallback.y;
    }

    ImGui::SetNextWindowPos(ImVec2(abilityBarX, abilityBarY), positionCondition);
    ImGui::SetNextWindowSize(ImVec2(totalWidth, totalHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags barFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##AbilityBar", nullptr, barFlags)) {
      ImDrawList *draw = ImGui::GetWindowDrawList();
      const ImVec2 windowPos = ImGui::GetCursorScreenPos();
      const ImVec2 windowEnd = windowPos + ImVec2(totalWidth, totalHeight);
      DrawBarFrame(draw, windowPos, ImVec2(totalWidth, totalHeight), style);

      const ImVec2 slotOrigin =
          windowPos + ImVec2(kBarPad + extraSide, kBarTop + kBarPad);
      const SkillCooldowns *cooldowns = playerEntity.get<SkillCooldowns>();
      const AutoCastSkills *autoCast = playerEntity.get<AutoCastSkills>();
      const BarPalette palette = PaletteFor(style);

      for (int i = 0; i < kSlotCount; ++i) {
        const ImVec2 a =
            slotOrigin + ImVec2(i * (kSlotSize + kSlotGap), 0.0f);
        const ImVec2 b = a + ImVec2(kSlotSize, kSlotSize);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##slot", ImVec2(kSlotSize, kSlotSize));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

        const SkillDef *def = nullptr;
        std::string skillId;
        if (i < static_cast<int>(bar->ids.size())) {
          skillId = bar->ids[static_cast<size_t>(i)];
          def = skillRegistry.Get(skillId);
          if (def != nullptr && !SkillIsActivatable(def->type)) {
            def = nullptr;
          }
        }
        const bool ready =
            def != nullptr &&
            (cooldowns == nullptr || cooldowns->IsReady(skillId));
        const float remaining =
            (def != nullptr && cooldowns != nullptr)
                ? cooldowns->RemainingFor(skillId)
                : 0.0f;

        DrawSlotFrame(draw, a, b, style, def != nullptr, hovered, ready);

        // A held skill is visibly held rather than merely off cooldown.
        if (const HeldSkillState *held = playerEntity.get<HeldSkillState>()) {
          if (held->active && held->skillId == skillId) {
            draw->AddRect(a, b, IM_COL32(120, 225, 130, 255), 3.0f, 0, 2.5f);
          }
        }

        // Keybind. Slots 11 and 12 have none: the handled keys are 1-9 and 0.
        const std::string key =
            i < 9 ? std::to_string(i + 1) : (i == 9 ? "0" : "");
        if (!key.empty()) {
          const ImVec2 keySize = ImGui::CalcTextSize(key.c_str());
          ImVec2 keyPos;
          if (style == 0) {
            keyPos = ImVec2(a.x + 5.0f, a.y + 2.0f);
          } else if (style == 1) {
            keyPos = ImVec2(a.x + (kSlotSize - keySize.x) * 0.5f,
                            b.y - keySize.y - 2.0f);
          } else {
            keyPos = ImVec2(a.x + (kSlotSize - keySize.x) * 0.5f, a.y + 2.0f);
          }
          draw->AddText(keyPos, palette.key, key.c_str());
        }

        if (def != nullptr) {
          if (const Texture2D *icon = IconPaths::ResolveIconTexture(
                  iconRegistry, resourceManager, def->icon, debugLog.get())) {
            DrawSpriteImGui(*icon, a + ImVec2(6, 6),
                            ImVec2(kSlotSize - 12.0f, kSlotSize - 12.0f));
          } else {
            // Name fallback, clipped to the slot. The tooltip always has the
            // full name, so truncation is a display choice, not lost content.
            std::string label = def->name;
            const float maxWidth = kSlotSize - 10.0f;
            while (!label.empty() &&
                   ImGui::CalcTextSize(label.c_str()).x > maxWidth) {
              label.pop_back();
            }
            if (label.size() < def->name.size() && label.size() > 1) {
              label.back() = '~';
            }
            const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
            draw->AddText(
                ImVec2(a.x + (kSlotSize - labelSize.x) * 0.5f,
                       a.y + (kSlotSize - labelSize.y) * 0.5f + 6.0f),
                palette.name, label.c_str());
          }
        }

        if (def != nullptr && remaining > 0.0f) {
          float duration = remaining;
          if (cooldowns != nullptr) {
            for (const SkillCooldown &entry : cooldowns->cooldowns) {
              if (entry.skillId == skillId && entry.duration > 0.0f) {
                duration = entry.duration;
                break;
              }
            }
          }
          const float fraction =
              duration > 0.0f
                  ? std::clamp(remaining / duration, 0.0f, 1.0f)
                  : 1.0f;
          draw->AddRectFilled(
              a, ImVec2(b.x, a.y + (b.y - a.y) * fraction),
              IM_COL32(0, 0, 0, 170));
          char seconds[16];
          std::snprintf(seconds, sizeof(seconds), "%.1f", remaining);
          const ImVec2 secondsSize = ImGui::CalcTextSize(seconds);
          draw->AddText(
              ImVec2(a.x + (kSlotSize - secondsSize.x) * 0.5f,
                     a.y + (kSlotSize - secondsSize.y) * 0.5f),
              IM_COL32(255, 255, 255, 255), seconds);
        }

        if (def != nullptr && autoCast != nullptr &&
            autoCast->Has(skillId)) {
          draw->AddCircleFilled(
              ImVec2(b.x - 7.0f, b.y - 7.0f), 3.5f, palette.pip);
          draw->AddCircle(ImVec2(b.x - 7.0f, b.y - 7.0f), 4.5f,
                          IM_COL32(20, 24, 20, 200), 0, 1.0f);
        }

        if (def != nullptr && clicked) {
          TryActivatePlayerSkill(i);
        }
        if (def != nullptr && rightClicked) {
          ImGui::OpenPopup("slot-menu");
        }
        if (def != nullptr && ImGui::BeginPopup("slot-menu")) {
          bool on = autoCast != nullptr && autoCast->Has(skillId);
          if (ImGui::Checkbox("Always casts", &on)) {
            if (AutoCastSkills *mutableAutoCast =
                    playerEntity.get_mut<AutoCastSkills>()) {
              mutableAutoCast->Set(skillId, on);
            }
          }
          ImGui::EndPopup();
        }
        if (def != nullptr && hovered) {
          ImGui::BeginTooltip();
          ImGui::Text("%s (%s)", def->name.c_str(), SkillTypeName(def->type));
          ImGui::Separator();
          if (def->activation.useWeapon) {
            ImGui::TextUnformatted("Damage and swing from your weapon.");
          } else {
            ImGui::Text("Damage: %.0f", def->activation.baseDamage);
            ImGui::Text("Cooldown: %.1fs", def->activation.cooldownSeconds);
          }
          ImGui::Text("Range: %.0f tile(s)", def->activation.rangeTiles);
          if (def->activation.castSeconds > 0.0f) {
            ImGui::Text("Cast: %.1fs", def->activation.castSeconds);
          }
          if (def->activation.interrupts) {
            ImGui::TextDisabled("Interrupts casts");
          }
          if (!def->activation.projectile.empty()) {
            ImGui::TextDisabled("Projectile: %s",
                                def->activation.projectile.c_str());
          }
          if (!def->activation.skillshot.empty()) {
            ImGui::TextDisabled("Skillshot: aimed");
          }
          ImGui::TextDisabled("Right-click: Always casts");
          ImGui::EndTooltip();
        }
        ImGui::PopID();
      }

      // A grip along the top strip, so the bar has an obvious place to drag from
      // even when every slot is occupied. The Warband style already has a boss
      // in that spot, so it uses the strip without dots.
      if (style != 0) {
        const ImVec2 grip((windowPos.x + windowEnd.x) * 0.5f,
                          windowPos.y + 5.0f);
        for (int dot = -1; dot <= 1; ++dot) {
          draw->AddCircleFilled(ImVec2(grip.x + dot * 6.0f, grip.y), 1.2f,
                                IM_COL32(150, 140, 120, 110));
        }
      }

      // Three small swatches at the right of the top strip pick a skin directly.
      // F9 still cycles, but a picture of a bar does not advertise F9; the
      // swatches make the three options visible and one click apart.
      const float swatchEnd = windowEnd.x - extraSide - 8.0f;
      for (int swatch = 0; swatch < 3; ++swatch) {
        const float swatchX = swatchEnd - (2 - swatch) * 12.0f;
        ImGui::PushID(100 + swatch);
        ImGui::SetCursorScreenPos(ImVec2(swatchX - 5.0f, windowPos.y + 2.0f));
        ImGui::InvisibleButton("##style", ImVec2(10.0f, kBarTop - 4.0f));
        const bool swatchHovered = ImGui::IsItemHovered();
        const bool active = swatch == style;
        draw->AddCircleFilled(ImVec2(swatchX, windowPos.y + 6.0f),
                              active ? 4.0f : 2.8f,
                              active ? palette.accent
                                     : IM_COL32(126, 116, 100, 150));
        if (swatchHovered) {
          ImGui::SetTooltip("%s", StyleName(swatch));
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
          abilityBarStyle = swatch;
        }
        ImGui::PopID();
      }

      // The three skins are chosen with F9, which is not discoverable from a
      // picture of a bar. Hovering the frame (not a slot) names the current one
      // and repeats the controls.
      if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Ability bar - %s", StyleName(style));
        ImGui::Separator();
        ImGui::TextDisabled("1-0 cast, click slots 11-12");
        ImGui::TextDisabled("Drag the top strip to move");
        ImGui::TextDisabled("Click a dot for a skin - F9 cycles - F10 resets");
        ImGui::EndTooltip();
      }

      abilityBarX = ImGui::GetWindowPos().x;
      abilityBarY = ImGui::GetWindowPos().y;
    }
    ImGui::End();
    ImGui::PopStyleVar();
  }

  resetCombatHudPosition = false;

}
