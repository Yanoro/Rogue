#include "EntityInfoWindow.h"
#include "Components.h"
#include "imgui.h"

EntityInfoWindow::EntityInfoWindow(flecs::entity entity, Map* map) : entity(entity), map(map) {}

void EntityInfoWindow::Draw() {
  if (!entity.is_alive()) {
    return;
  }

  bool isOpen = true;
  ImGui::Begin("Entity Info", &isOpen);
  
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
  if (const TargetPath *targetPath = entity.get<TargetPath>()) {
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
