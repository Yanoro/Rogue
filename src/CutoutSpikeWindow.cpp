#include "CutoutSpikeWindow.h"

#include "Game.h"
#include "RigRender.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace {

// Test arm: a sleeve, a forearm and a hand with overlapping joints, drawn as a small
// hand-authored asset (data/sprites/cutout_test_arm.png) so the technique is judged on
// segment art rather than a straight bar cut in half.
constexpr float kUpperX = 12.0f, kUpperY = 8.0f, kUpperW = 8.0f, kUpperH = 8.0f;
constexpr float kForeX = 13.0f, kForeY = 14.0f, kForeW = 6.0f, kForeH = 7.0f;
constexpr float kHandX = 13.0f, kHandY = 20.0f, kHandW = 7.0f, kHandH = 5.0f;

constexpr float kShoulderX = 16.0f, kShoulderY = 9.0f;
constexpr float kElbowX = 16.0f, kElbowY = 15.0f;
constexpr float kWristX = 16.0f, kWristY = 21.0f;
constexpr float kHandCenterX = 16.5f, kHandCenterY = 22.0f;

// The iron sword's grip in its 32x32 image.
constexpr float kSwordX = 21.0f, kSwordY = 4.0f, kSwordW = 7.0f, kSwordH = 21.0f;
constexpr float kGripSrcX = 24.0f, kGripSrcY = 22.0f;

constexpr float kScale = 8.0f;
constexpr float kSecondsPerPose = 0.12f;

// Six eased poses, roughly a downward cut and the return. Shoulder travel is about 85
// degrees, not the 215 of the stress test.
struct Pose {
  float shoulder;
  float elbow;
  float grip;  // sword angle relative to the forearm
};

constexpr Pose kPoses[] = {
    {-30.0f, -12.0f, 0.0f},
    {-45.0f, -20.0f, 8.0f},
    {-15.0f, -5.0f, 12.0f},
    {20.0f, 10.0f, 10.0f},
    {42.0f, 20.0f, 4.0f},
    {10.0f, 0.0f, 0.0f},
};
constexpr int kPoseCount = static_cast<int>(sizeof(kPoses) / sizeof(kPoses[0]));

float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

Pose InterpolatedPose(float time) {
  const float phase = time / kSecondsPerPose;
  const int index = static_cast<int>(std::floor(phase)) % kPoseCount;
  const int next = (index + 1) % kPoseCount;
  const float t = Smooth(phase - std::floor(phase));
  const Pose &a = kPoses[index];
  const Pose &b = kPoses[next];
  return {a.shoulder + (b.shoulder - a.shoulder) * t,
          a.elbow + (b.elbow - a.elbow) * t,
          a.grip + (b.grip - a.grip) * t};
}

ImVec2 Rotate(ImVec2 v, float degrees) {
  const float r = degrees * 3.14159265358979f / 180.0f;
  const float c = std::cos(r);
  const float s = std::sin(r);
  return ImVec2(v.x * c - v.y * s, v.x * s + v.y * c);
}

void DrawSegment(ImDrawList *draw, const Texture2D &texture, float srcX, float srcY,
                 float srcW, float srcH, float pivotSrcX, float pivotSrcY,
                 ImVec2 pivotScreen, float degrees) {
  SetTextureFilter(texture, TEXTURE_FILTER_POINT);
  const ImVec2 corners[4] = {
      ImVec2(srcX - pivotSrcX, srcY - pivotSrcY),
      ImVec2(srcX + srcW - pivotSrcX, srcY - pivotSrcY),
      ImVec2(srcX + srcW - pivotSrcX, srcY + srcH - pivotSrcY),
      ImVec2(srcX - pivotSrcX, srcY + srcH - pivotSrcY),
  };
  ImVec2 screen[4];
  for (int i = 0; i < 4; ++i) {
    const ImVec2 rotated = Rotate(corners[i], degrees);
    screen[i] = ImVec2(pivotScreen.x + rotated.x * kScale,
                       pivotScreen.y + rotated.y * kScale);
  }
  const ImVec2 uv[4] = {
      ImVec2(srcX / texture.width, srcY / texture.height),
      ImVec2((srcX + srcW) / texture.width, srcY / texture.height),
      ImVec2((srcX + srcW) / texture.width, (srcY + srcH) / texture.height),
      ImVec2(srcX / texture.width, (srcY + srcH) / texture.height),
  };
  draw->AddImageQuad(
      static_cast<ImTextureID>(static_cast<intptr_t>(texture.id)),
      screen[0], screen[1], screen[2], screen[3], uv[0], uv[1], uv[2], uv[3],
      IM_COL32_WHITE);
}

}  // namespace

CutoutSpikeWindow::CutoutSpikeWindow(Game *game) : game(game) {}

void CutoutSpikeWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(680, 520), ImGuiCond_FirstUseEver);
  ImGui::Begin("Cutout spike", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->cutoutSpikeWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  const Texture2D *arm = RigRender::ResolveRigSprite("cutout_test_arm",
                                                     game->resourceManager);
  const Texture2D *sword = RigRender::ResolveRigSprite("worn_iron_sword",
                                                       game->resourceManager);
  if (arm == nullptr || sword == nullptr) {
    ImGui::TextWrapped("Missing art: %s.",
                       arm == nullptr ? "cutout_test_arm" : "worn_iron_sword");
    ImGui::End();
    return;
  }

  ImGui::TextDisabled(
      "Fair test: three-segment test arm (sleeve, forearm, hand), sword held at the hand, "
      "six eased poses. Throwaway spike, not a rig format.");
  if (ImGui::Button(playing ? "Pause" : "Play")) playing = !playing;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(100.0f);
  ImGui::SliderFloat("speed", &speed, 0.25f, 2.0f, "%.2fx");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160.0f);
  ImGui::SliderFloat("time", &time, 0.0f, kPoseCount * kSecondsPerPose, "%.2f s");
  ImGui::SameLine();
  ImGui::Checkbox("joint caps", &jointCaps);

  ImGui::SetNextItemWidth(160.0f);
  ImGui::SliderFloat("grip offset", &gripOffset, -45.0f, 45.0f, "%.0f deg");
  ImGui::SameLine();
  static const int kSteps[] = {0, 15, 30, 45, 90};
  const char *kStepLabels[] = {"smooth", "15", "30", "45", "90"};
  int stepIndex = 0;
  for (int i = 0; i < 5; ++i) {
    if (swordAngleStep == kSteps[i]) stepIndex = i;
  }
  ImGui::SetNextItemWidth(110.0f);
  if (ImGui::Combo("sword angle step", &stepIndex, kStepLabels, 5)) {
    swordAngleStep = kSteps[stepIndex];
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "0 = one sword image rotated freely. A step snaps the blade to that many degrees, "
        "which approximates a set of pre-drawn sword angles; real pre-drawn art would not "
        "show the stair-stepping of a rotated image.");
  }

  if (playing) {
    const float duration = kPoseCount * kSecondsPerPose;
    time += std::min(ImGui::GetIO().DeltaTime, 0.1f) * speed;
    if (time >= duration) time = std::fmod(time, duration);
  }

  const Pose pose = InterpolatedPose(time);
  const float upper = pose.shoulder;
  const float fore = pose.shoulder + pose.elbow;
  float bladeAngle = pose.shoulder + pose.elbow + pose.grip + gripOffset;
  if (swordAngleStep > 0) {
    bladeAngle = std::round(bladeAngle / static_cast<float>(swordAngleStep)) *
                 static_cast<float>(swordAngleStep);
  }

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 area(480.0f, 350.0f);
  ImGui::Dummy(area);
  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + area.x, origin.y + area.y),
                      IM_COL32(26, 26, 30, 255));
  draw->AddRect(origin, ImVec2(origin.x + area.x, origin.y + area.y),
                IM_COL32(70, 70, 76, 255));

  const ImVec2 shoulder(origin.x + 150.0f, origin.y + 55.0f);
  const ImVec2 elbow(
      shoulder.x +
          Rotate(ImVec2(0.0f, kElbowY - kShoulderY), upper).x * kScale,
      shoulder.y +
          Rotate(ImVec2(0.0f, kElbowY - kShoulderY), upper).y * kScale);
  const ImVec2 wrist(
      elbow.x + Rotate(ImVec2(0.0f, kWristY - kElbowY), fore).x * kScale,
      elbow.y + Rotate(ImVec2(0.0f, kWristY - kElbowY), fore).y * kScale);
  const ImVec2 handCenter(
      wrist.x + Rotate(ImVec2(kHandCenterX - kWristX, kHandCenterY - kWristY), fore).x *
                    kScale,
      wrist.y + Rotate(ImVec2(kHandCenterX - kWristX, kHandCenterY - kWristY), fore).y *
                    kScale);

  DrawSegment(draw, *arm, kUpperX, kUpperY, kUpperW, kUpperH, kShoulderX,
              kShoulderY, shoulder, upper);
  DrawSegment(draw, *arm, kForeX, kForeY, kForeW, kForeH, kElbowX, kElbowY, elbow,
              fore);
  // The sword is behind the hand: the hand covers the grip.
  DrawSegment(draw, *sword, kSwordX, kSwordY, kSwordW, kSwordH, kGripSrcX, kGripSrcY,
              handCenter, bladeAngle);
  DrawSegment(draw, *arm, kHandX, kHandY, kHandW, kHandH, kWristX, kWristY, wrist,
              fore);
  if (jointCaps) {
    draw->AddCircleFilled(elbow, kScale * 0.9f, IM_COL32(138, 90, 54, 255));
    draw->AddCircleFilled(wrist, kScale * 0.75f, IM_COL32(201, 141, 90, 255));
  }

  // The unarticulated test arm and sword, for shape/colour comparison.
  const ImVec2 reference(origin.x + 330.0f, origin.y + 70.0f);
  draw->AddText(ImVec2(reference.x, reference.y - 22.0f),
                IM_COL32(160, 160, 160, 255), "rest");
  const float refScale = 4.0f;
  draw->AddImage(
      static_cast<ImTextureID>(static_cast<intptr_t>(arm->id)),
      ImVec2(reference.x, reference.y),
      ImVec2(reference.x + arm->width * refScale, reference.y + arm->height * refScale),
      ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);
  draw->AddImage(
      static_cast<ImTextureID>(static_cast<intptr_t>(sword->id)),
      ImVec2(reference.x, reference.y),
      ImVec2(reference.x + sword->width * refScale, reference.y + sword->height * refScale),
      ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);

  ImGui::Separator();
  ImGui::Text("shoulder %.0f  elbow %.0f  grip %.0f  sword %.0f",
              pose.shoulder, pose.elbow, pose.grip + gripOffset, bladeAngle);
  ImGui::TextDisabled(
      "Judge the elbow/wrist seams, whether the hand reads as holding the grip, and "
      "whether the sword angle stays plausible across the six poses. Try the angle steps: "
      "if smooth looks wrong and a step looks acceptable, production needs pre-drawn "
      "sword angles.");
  ImGui::End();
}
