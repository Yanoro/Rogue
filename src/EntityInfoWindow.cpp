#include "EntityInfoWindow.h"
#include "Components.h"
#include "SkillRegistry.h"
#include "StatRegistry.h"
#include "imgui.h"
#include "Map.h"

EntityInfoWindow::EntityInfoWindow(flecs::entity entity) : entity(entity) {}

void EntityInfoWindow::Draw() {
  if (!entity.is_alive()) {
    return;
  }

  Map *map = nullptr;
  if (auto mapRes = entity.world().get<MapResource>()) {
    map = mapRes->map;
  }

  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(420, 580), ImGuiCond_FirstUseEver);
  ImGui::Begin("Entity Info", &isOpen, ImGuiWindowFlags_None);
  
  if (!isOpen) {
    entity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  ImGui::Text("Entity ID: %lu", entity.id());
  ImGui::Separator();

  if (const GamePosition *gPos = entity.get<GamePosition>()) {
    int pos[2] = {gPos->x, gPos->y};
    if (ImGui::InputInt2("Game Position", pos)) {
      entity.set<GamePosition>({pos[0], pos[1]});
      if (map) {
        entity.set<ScreenPosition>(
            map->GameCoordsToScreenCoords(pos[0], pos[1]));
      }
    }
  }
  if (const ScreenPosition *sPos = entity.get<ScreenPosition>()) {
    float pos[2] = {sPos->x, sPos->y};
    if (ImGui::DragFloat2("Screen Position", pos)) {
      entity.set<ScreenPosition>({pos[0], pos[1]});
    }
  }
  if (const MOVE_THROUGH_PATH_ACTION *targetPath = entity.get<MOVE_THROUGH_PATH_ACTION>()) {
    if (!targetPath->path.empty()) {
      const GamePosition &lastPos = targetPath->path.back();
      ImGui::Text("Moving to: (%d, %d)", lastPos.x, lastPos.y);
    }
  }

  const Velocity* currentVel = entity.get<Velocity>();
  float v[2] = {currentVel ? currentVel->x : 0.0f, currentVel ? currentVel->y : 0.0f};
  if (ImGui::DragFloat2("Velocity", v)) {
    entity.set<Velocity>({v[0], v[1]});
  }

  const Acceleration* currentAccel = entity.get<Acceleration>();
  float a[2] = {currentAccel ? currentAccel->x : 0.0f, currentAccel ? currentAccel->y : 0.0f};
  if (ImGui::DragFloat2("Acceleration", a)) {
    entity.set<Acceleration>({a[0], a[1]});
  }

  if (const MaxSpeed *speed = entity.get<MaxSpeed>()) {
    float s = speed->value;
    if (ImGui::DragFloat("Max Speed", &s)) {
      entity.set<MaxSpeed>({s});
    }
  }
  if (const Friction *friction = entity.get<Friction>()) {
    float f = friction->value;
    if (ImGui::DragFloat("Friction", &f)) {
      entity.set<Friction>({f});
    }
  }

  if (const StatBlock *block = entity.get<StatBlock>()) {
    ImGui::Separator();
    ImGui::Text("Stats:");

    const StatRegistry *registry = nullptr;
    if (auto registryRes = entity.world().get<StatRegistryResource>()) {
      registry = registryRes->registry;
    }

    // Editable so the simulation can be poked while it runs. The baseline is
    // printed next to each value so "neutral" is visible rather than implied:
    // whatever the baseline is, that value produces no effect.
    StatBlock edited = *block;
    bool changed = false;
    for (StatValue &value : edited.values) {
      const StatDef *def = registry ? registry->Get(value.id) : nullptr;
      const float lo = def ? def->min : 0.0f;
      const float hi = def ? def->max : 100.0f;

      ImGui::PushID(value.id.c_str());
      float v = value.value;
      if (ImGui::DragFloat(value.id.c_str(), &v, 0.1f, lo, hi)) {
        value.value = v;
        changed = true;
      }
      ImGui::PopID();

      if (def) {
        ImGui::SameLine();
        ImGui::TextDisabled("base %.0f", def->baseline);
      }
    }
    if (changed) {
      entity.set<StatBlock>(edited);
    }
  }

  if (const SkillBlock *block = entity.get<SkillBlock>()) {
    ImGui::Separator();
    ImGui::Text("Skills:");

    const SkillRegistry *registry = nullptr;
    if (auto registryRes = entity.world().get<SkillRegistryResource>()) {
      registry = registryRes->registry;
    }

    // Editable, like the stats above: being able to set a level is the only way
    // to see a skill's effect without playing through the progression. The stage
    // is derived, so it follows the level as you drag it rather than being a
    // second field to keep in sync.
    SkillBlock edited = *block;
    bool changed = false;
    for (SkillValue &value : edited.values) {
      const SkillDef *def = registry ? registry->Get(value.id) : nullptr;

      ImGui::PushID(value.id.c_str());
      int level = value.level;
      if (ImGui::DragInt(value.id.c_str(), &level, 1.0f, 0,
                         def ? def->MaxLevel() : 100)) {
        value.level = level;
        changed = true;
      }
      ImGui::PopID();

      if (def) {
        ImGui::SameLine();
        ImGui::TextDisabled(
            "%s %d/%d  (%d xp)",
            def->StageName(def->StageForLevel(value.level)).c_str(),
            def->LevelWithinStage(value.level), def->levelsPerStage, value.xp);
      }
    }
    if (changed) {
      entity.set<SkillBlock>(edited);
    }
  }

  ImGui::Separator();
  bool isIntangible = entity.has<Intangible>();
  if (ImGui::Button(isIntangible ? "Remove Intangible" : "Make Intangible")) {
    if (isIntangible) {
      entity.remove<Intangible>();
    } else {
      entity.add<Intangible>();
    }
  }

  ImGui::Separator();
  ImGui::Text("Velocity & Acceleration Vectors:");

  // Layout the velocity and acceleration vectors side by side
  ImVec2 canvasSize(120.0f, 120.0f);
  
  // Velocity Vector (left side)
  ImGui::Text("Velocity:");
  ImGui::SameLine(150.0f);
  ImGui::Text("Acceleration:");
  
  ImVec2 canvasPos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##vel_canvas", canvasSize);

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  ImVec2 center(canvasPos.x + canvasSize.x * 0.5f,
                canvasPos.y + canvasSize.y * 0.5f);
  float maxRadius = canvasSize.x * 0.45f;

  // Draw background circle representing max speed
  drawList->AddCircleFilled(center, maxRadius, IM_COL32(40, 40, 40, 255));
  drawList->AddCircle(center, maxRadius, IM_COL32(100, 100, 100, 255));

  // Draw crosshair axes
  drawList->AddLine(ImVec2(center.x, canvasPos.y + 5),
                    ImVec2(center.x, canvasPos.y + canvasSize.y - 5),
                    IM_COL32(80, 80, 80, 255));
  drawList->AddLine(ImVec2(canvasPos.x + 5, center.y),
                    ImVec2(canvasPos.x + canvasSize.x - 5, center.y),
                    IM_COL32(80, 80, 80, 255));

  const MaxSpeed *maxSpeed = entity.get<MaxSpeed>();

  if (maxSpeed && maxSpeed->value > 0.0f) {
    // Calculate scaled endpoint
    float scaleX = maxRadius / maxSpeed->value;
    float scaleY = maxRadius / maxSpeed->value;
    ImVec2 velEnd(center.x + (currentVel ? currentVel->x : 0.0f) * scaleX, center.y + (currentVel ? currentVel->y : 0.0f) * scaleY);

    // Draw velocity vector line and arrowhead/dot
    drawList->AddLine(center, velEnd, IM_COL32(50, 255, 50, 255), 2.0f);
    drawList->AddCircleFilled(velEnd, 3.0f, IM_COL32(50, 255, 50, 255));
  }

  // Center point
  drawList->AddCircleFilled(center, 2.0f, IM_COL32(255, 255, 255, 255));

  // Draw acceleration vector on the same line, to the right
  ImGui::SameLine();

  ImVec2 accelCanvasSize(120.0f, 120.0f);
  ImVec2 accelCanvasPos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##accel_canvas", accelCanvasSize);

  ImVec2 accelCenter(accelCanvasPos.x + accelCanvasSize.x * 0.5f,
                     accelCanvasPos.y + accelCanvasSize.y * 0.5f);
  float accelMaxRadius = accelCanvasSize.x * 0.45f;

  // Draw background circle
  drawList->AddCircleFilled(accelCenter, accelMaxRadius,
                            IM_COL32(40, 40, 40, 255));
  drawList->AddCircle(accelCenter, accelMaxRadius,
                      IM_COL32(100, 100, 100, 255));

  // Draw crosshair axes
  drawList->AddLine(
      ImVec2(accelCenter.x, accelCanvasPos.y + 5),
      ImVec2(accelCenter.x, accelCanvasPos.y + accelCanvasSize.y - 5),
      IM_COL32(80, 80, 80, 255));
  drawList->AddLine(
      ImVec2(accelCanvasPos.x + 5, accelCenter.y),
      ImVec2(accelCanvasPos.x + accelCanvasSize.x - 5, accelCenter.y),
      IM_COL32(80, 80, 80, 255));

  if (maxSpeed && maxSpeed->value > 0.0f) {
    // Scale relative to max speed (can be tuned later if acceleration exceeds
    // this)
    float scaleX = accelMaxRadius / (maxSpeed->value * 5.0f);
    float scaleY = accelMaxRadius / (maxSpeed->value * 5.0f);
    ImVec2 accelEnd(accelCenter.x + (currentAccel ? currentAccel->x : 0.0f) * scaleX,
                    accelCenter.y + (currentAccel ? currentAccel->y : 0.0f) * scaleY);

    // Draw acceleration vector line and arrowhead/dot (Red instead of Green)
    drawList->AddLine(accelCenter, accelEnd, IM_COL32(255, 50, 50, 255),
                      2.0f);
    drawList->AddCircleFilled(accelEnd, 3.0f, IM_COL32(255, 50, 50, 255));
  }

  // Center point
  drawList->AddCircleFilled(accelCenter, 2.0f, IM_COL32(255, 255, 255, 255));

  ImGui::End();
}
