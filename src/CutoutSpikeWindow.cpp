#include "CutoutSpikeWindow.h"

#include "Game.h"
#include "RigRender.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace {

// Angles in degrees, measured from the arm pointing straight down. The sword is relative
// to the forearm.
struct Pose {
  float shoulder;
  float elbow;
  float sword;
};

// Raised behind, wound up, driven down, follow through. Enough to see a joint bend and the
// sword follow the hand.
constexpr Pose kPoses[] = {
    {-115.0f, -20.0f, 0.0f},
    {-80.0f, -35.0f, -10.0f},
    {30.0f, 20.0f, 10.0f},
    {100.0f, 30.0f, 20.0f},
};
constexpr int kPoseCount = static_cast<int>(sizeof(kPoses) / sizeof(kPoses[0]));
constexpr float kSecondsPerPose = 0.35f;

// Source geometry of the chosen art, in the 32x32 layer.
constexpr float kArmX = 22.0f, kArmW = 4.0f;
constexpr float kUpperY = 12.0f, kUpperH = 6.0f;
constexpr float kForeY = 18.0f, kForeH = 6.0f;
constexpr float kShoulderSrcX = 24.0f, kShoulderSrcY = 12.0f;
constexpr float kElbowSrcX = 24.0f, kElbowSrcY = 18.0f;
constexpr float kWristSrcX = 24.0f, kWristSrcY = 23.0f;
constexpr float kSwordX = 21.0f, kSwordY = 4.0f, kSwordW = 7.0f, kSwordH = 21.0f;
constexpr float kGripSrcX = 24.0f, kGripSrcY = 22.0f;

constexpr float kScale = 8.0f;

Pose InterpolatedPose(float time) {
  const float phase = time / kSecondsPerPose;
  const int index = static_cast<int>(std::floor(phase)) % kPoseCount;
  const int next = (index + 1) % kPoseCount;
  const float t = phase - std::floor(phase);
  const Pose &a = kPoses[index];
  const Pose &b = kPoses[next];
  return {a.shoulder + (b.shoulder - a.shoulder) * t,
          a.elbow + (b.elbow - a.elbow) * t,
          a.sword + (b.sword - a.sword) * t};
}

ImVec2 Rotate(ImVec2 v, float degrees) {
  const float r = degrees * 3.14159265358979f / 180.0f;
  const float c = std::cos(r);
  const float s = std::sin(r);
  return ImVec2(v.x * c - v.y * s, v.x * s + v.y * c);
}

// Draws a source rectangle rotated about a source-space pivot placed at `pivotScreen`.
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
  ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_FirstUseEver);
  ImGui::Begin("Cutout spike", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->cutoutSpikeWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  const Texture2D *arm = RigRender::ResolveRigSprite("rig_orc_brute_arm_right",
                                                     game->resourceManager);
  const Texture2D *sword = RigRender::ResolveRigSprite("worn_iron_sword",
                                                       game->resourceManager);
  if (arm == nullptr || sword == nullptr) {
    ImGui::TextWrapped(
        "Missing art: %s.", arm == nullptr ? "rig_orc_brute_arm_right" : "worn_iron_sword");
    ImGui::TextDisabled(
        "The spike uses orc_brute's right arm and the iron sword; generate/keep that "
        "art to see it.");
    ImGui::End();
    return;
  }

  ImGui::TextDisabled(
      "Hardcoded: orc_brute arm_right split at the elbow, iron sword at the wrist. "
      "Throwaway spike, not a rig format.");
  if (ImGui::Button(playing ? "Pause" : "Play")) playing = !playing;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.0f);
  ImGui::SliderFloat("speed", &speed, 0.25f, 2.0f, "%.2fx");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(180.0f);
  ImGui::SliderFloat("time", &time, 0.0f, kPoseCount * kSecondsPerPose,
                     "%.2f s");

  if (playing) {
    const float delta = std::min(ImGui::GetIO().DeltaTime, 0.1f) * speed;
    time += delta;
    const float duration = kPoseCount * kSecondsPerPose;
    if (time >= duration) time = std::fmod(time, duration);
  }

  const Pose pose = InterpolatedPose(time);
  const float upper = pose.shoulder;
  const float fore = pose.shoulder + pose.elbow;
  const float bladeAngle = pose.shoulder + pose.elbow + pose.sword;

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 area(420.0f, 320.0f);
  ImGui::Dummy(area);
  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + area.x, origin.y + area.y),
                      IM_COL32(26, 26, 30, 255));
  draw->AddRect(origin, ImVec2(origin.x + area.x, origin.y + area.y),
                IM_COL32(70, 70, 76, 255));

  const ImVec2 shoulder(origin.x + 150.0f, origin.y + 70.0f);
  const ImVec2 elbow(
      shoulder.x + Rotate(ImVec2(0.0f, kElbowSrcY - kShoulderSrcY), upper).x * kScale,
      shoulder.y + Rotate(ImVec2(0.0f, kElbowSrcY - kShoulderSrcY), upper).y * kScale);
  const ImVec2 wrist(
      elbow.x + Rotate(ImVec2(0.0f, kWristSrcY - kElbowSrcY), fore).x * kScale,
      elbow.y + Rotate(ImVec2(0.0f, kWristSrcY - kElbowSrcY), fore).y * kScale);

  DrawSegment(draw, *arm, kArmX, kUpperY, kArmW, kUpperH, kShoulderSrcX,
              kShoulderSrcY, shoulder, upper);
  DrawSegment(draw, *arm, kArmX, kForeY, kArmW, kForeH, kElbowSrcX, kElbowSrcY,
              elbow, fore);
  DrawSegment(draw, *sword, kSwordX, kSwordY, kSwordW, kSwordH, kGripSrcX,
              kGripSrcY, wrist, bladeAngle);

  // The unarticulated arm and sword, for comparison.
  const ImVec2 reference(origin.x + 320.0f, origin.y + 60.0f);
  draw->AddText(ImVec2(reference.x - 10.0f, reference.y - 24.0f),
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
  ImGui::Text("shoulder %.0f deg   elbow %.0f deg   sword %.0f deg", pose.shoulder,
              pose.elbow, pose.sword);
  ImGui::TextDisabled(
      "Look at the elbow seam and the sword angle across the four poses. If the seams "
      "or rotations read badly, cutout needs new segment art or more than one sword "
      "angle before it is worth production work.");
  ImGui::End();
}
