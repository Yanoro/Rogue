#include "HybridSpikeWindow.h"

#include "DesignerWindow.h"
#include "Game.h"
#include "RigRender.hpp"
#include "RigGripDraw.hpp"
#include "SpritePaths.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

struct Layer {
  std::string label;
  const Texture2D *texture = nullptr;
  int z = 0;
  bool worn = false;
};

constexpr float kBox = 224.0f;

}  // namespace

HybridSpikeWindow::HybridSpikeWindow(Game *game) : game(game) {}

void HybridSpikeWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(580, 560), ImGuiCond_FirstUseEver);
  ImGui::Begin("Hybrid spike", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->hybridSpikeWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  const ActorRig *rig = game->rigRegistry.Get("orc_brute");
  if (rig == nullptr) {
    ImGui::TextWrapped("This spike needs the orc_brute rig.");
    ImGui::End();
    return;
  }

  // Items that have worn art; the two test weapons first.
  std::vector<const ItemDef *> wornItems;
  for (const ItemDef *candidate : game->itemRegistry.GetAll()) {
    // Hand items only: a helmet has worn art but no grip for this comparison.
    if (candidate->slot != "hand" || candidate->wornSprite.empty()) continue;
    wornItems.push_back(candidate);
  }
  std::sort(wornItems.begin(), wornItems.end(),
            [](const ItemDef *a, const ItemDef *b) { return a->id < b->id; });
  if (wornItems.empty()) {
    ImGui::TextWrapped("No item has worn art.");
    ImGui::End();
    return;
  }
  const ItemDef *item = game->itemRegistry.Get(itemId);
  if (item == nullptr || item->wornSprite.empty()) {
    item = wornItems.front();
    itemId = item->id;
  }

  ImGui::TextDisabled(
      "Hybrid experiment: existing frame-swapped body + worn equipment. Compare generated "
      "worn frames with a static weapon on a grip path derived from the arm frames. "
      "Throwaway.");
  ImGui::TextDisabled("rig %s  |  %zu item(s) with worn art", rig->id.c_str(),
                      wornItems.size());

  int itemSelected = 0;
  std::vector<const char *> itemNames;
  for (size_t i = 0; i < wornItems.size(); ++i) {
    itemNames.push_back(wornItems[i]->name.empty() ? wornItems[i]->id.c_str()
                                                   : wornItems[i]->name.c_str());
    if (wornItems[i]->id == itemId) itemSelected = static_cast<int>(i);
  }
  ImGui::SetNextItemWidth(200.0f);
  if (ImGui::Combo("##hybrid_item", &itemSelected, itemNames.data(),
                   static_cast<int>(itemNames.size()))) {
    itemId = wornItems[static_cast<size_t>(itemSelected)]->id;
    time = 0.0f;
    status.clear();
  }
  ImGui::SameLine();
  const char *kModes[] = {"static", "generated frames", "grip path"};
  ImGui::SetNextItemWidth(150.0f);
  ImGui::Combo("worn", &wornMode, kModes, 3);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "static: one worn sprite at the item's z.\n"
        "generated frames: the <worn>_<clip>_fN set, if generated.\n"
        "grip path: the static sprite placed at the hand and angle read from the arm's "
        "animation frames -- the cheap alternative to per-weapon frames.");
  }

  if (rig->clips.empty()) {
    ImGui::TextWrapped("orc_brute has no clips; add one in the Content Editor first.");
    ImGui::End();
    return;
  }
  if (clipId.empty() || std::none_of(rig->clips.begin(), rig->clips.end(),
                                     [&](const RigClip &clip) {
                                       return clip.id == clipId;
                                     })) {
    clipId = rig->clips.front().id;
    for (const RigClip &clip : rig->clips) {
      if (clip.id == "walk") clipId = clip.id;
    }
    time = 0.0f;
  }
  const RigClip *clip = nullptr;
  for (const RigClip &candidate : rig->clips) {
    if (candidate.id == clipId) clip = &candidate;
  }
  if (clip == nullptr || clip->frames < 1) {
    ImGui::TextWrapped("The selected clip has no frames.");
    ImGui::End();
    return;
  }

  int clipSelected = 0;
  std::vector<const char *> clipIds;
  for (size_t i = 0; i < rig->clips.size(); ++i) {
    clipIds.push_back(rig->clips[i].id.c_str());
    if (rig->clips[i].id == clipId) clipSelected = static_cast<int>(i);
  }
  ImGui::SetNextItemWidth(120.0f);
  if (ImGui::Combo("##hybrid_clip", &clipSelected, clipIds.data(),
                   static_cast<int>(clipIds.size()))) {
    clipId = rig->clips[static_cast<size_t>(clipSelected)].id;
    time = 0.0f;
  }
  ImGui::SameLine();
  if (ImGui::Button(playing ? "Pause" : "Play")) playing = !playing;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(100.0f);
  ImGui::SliderFloat("speed", &speed, 0.25f, 2.0f, "%.2fx");

  const float duration = clip->fps > 0 ? clip->frames / static_cast<float>(clip->fps)
                                       : 0.0f;
  if (playing && duration > 0.0f) {
    time += std::min(ImGui::GetIO().DeltaTime, 0.1f) * speed;
    if (time >= duration) time = std::fmod(time, duration);
  }
  int frame = 0;
  if (clip->fps > 0) {
    frame = std::min(static_cast<int>(time * static_cast<float>(clip->fps)),
                     clip->frames - 1);
  }
  ImGui::SetNextItemWidth(240.0f);
  if (ImGui::SliderInt("##hybrid_frame", &frame, 0, clip->frames - 1, "frame %d")) {
    playing = false;
    time = clip->fps > 0 ? frame / static_cast<float>(clip->fps) : 0.0f;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("frame %d / %d", frame + 1, clip->frames);

  // The generated frame set, if any.
  std::vector<std::string> wornFrameSprites;
  int wornFramesPresent = 0;
  for (int i = 1; i <= clip->frames; ++i) {
    const std::string sprite =
        item->wornSprite + "_" + clip->id + "_f" + std::to_string(i);
    wornFrameSprites.push_back(sprite);
    if (SpriteResolves(std::string(), sprite, nullptr)) ++wornFramesPresent;
  }

  const auto refresh = [this](const std::string &sprite) {
    std::string path;
    if (!SpriteResolves(std::string(), sprite, &path) || path.empty()) return;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) return;
    const auto found = stamps.find(sprite);
    if (found != stamps.end() && found->second == stamp) return;
    stamps[sprite] = stamp;
    game->ForgetSpriteMemo(std::string(), sprite);
  };
  for (const RigPart &part : rig->parts) {
    refresh(part.sprite);
    for (const RigPartClip &partClip : part.clips) {
      for (const std::string &sprite : partClip.frames) refresh(sprite);
    }
  }
  refresh(item->wornSprite);
  for (const std::string &sprite : wornFrameSprites) refresh(sprite);

  std::vector<std::string> hidden = item->wornHides;
  std::vector<Layer> layers;
  std::string armFrameSprite = "rig_orc_brute_arm_right";
  for (const RigPart &part : rig->parts) {
    std::string sprite = part.sprite;
    for (const RigPartClip &partClip : part.clips) {
      if (partClip.clip != clip->id || partClip.frames.empty()) continue;
      const int index = std::min(static_cast<int>(partClip.frames.size()) - 1, frame);
      sprite = partClip.frames[static_cast<size_t>(index)];
    }
    if (part.id == "arm_right") armFrameSprite = sprite;
    if (std::find(hidden.begin(), hidden.end(), part.id) != hidden.end()) continue;
    Layer layer;
    layer.label = part.id;
    layer.texture = RigRender::ResolveRigSprite(sprite, game->resourceManager);
    layer.z = part.z;
    layers.push_back(std::move(layer));
  }

  const bool gripPath = wornMode == 2;
  const Texture2D *staticWorn =
      RigRender::ResolveRigSprite(item->wornSprite, game->resourceManager);
  if (!gripPath) {
    std::string wornSprite = item->wornSprite;
    if (wornMode == 1 && wornFramesPresent == clip->frames) {
      wornSprite = wornFrameSprites[static_cast<size_t>(frame)];
    }
    Layer worn;
    worn.label = item->id;
    worn.texture = RigRender::ResolveRigSprite(wornSprite, game->resourceManager);
    worn.z = item->wornZ;
    worn.worn = true;
    layers.push_back(std::move(worn));
  }
  std::stable_sort(layers.begin(), layers.end(),
                   [](const Layer &a, const Layer &b) { return a.z < b.z; });

  const ImVec2 topLeft = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(kBox, kBox));
  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(topLeft, ImVec2(topLeft.x + kBox, topLeft.y + kBox),
                      IM_COL32(26, 26, 30, 255));
  const float scale = kBox / 32.0f;
  int missing = 0;
  for (const Layer &layer : layers) {
    if (layer.texture == nullptr) {
      ++missing;
      continue;
    }
    SetTextureFilter(*layer.texture, TEXTURE_FILTER_POINT);
    const SpriteRect fit = SpriteFitRect(*layer.texture, topLeft.x, topLeft.y, kBox, kBox);
    if (fit.width <= 0.0f || fit.height <= 0.0f) continue;
    draw->AddImage(
        static_cast<ImTextureID>(static_cast<intptr_t>(layer.texture->id)),
        ImVec2(fit.x, fit.y), ImVec2(fit.x + fit.width, fit.y + fit.height));
  }

  // Grip path: place the static weapon at the hand read from the arm frame, rotated by the
  // arm direction plus an offset. Angle snapping approximates a few pre-drawn angles.
  if (gripPath && staticWorn != nullptr) {
    const Texture2D *armTexture =
        RigRender::ResolveRigSprite(armFrameSprite, game->resourceManager);
    const RigGrip::ArmGrip arm =
        armTexture != nullptr ? RigGrip::AnalyzeArm(*armTexture) : RigGrip::ArmGrip{};
    if (arm.ok) {
      float angle = RigGrip::WeaponAngle(arm, *item) + gripOffset;
      if (gripAngleStep > 0) {
        angle = std::round(angle / static_cast<float>(gripAngleStep)) *
                static_cast<float>(gripAngleStep);
      }
      RigGrip::DrawWeapon(draw, *staticWorn, *item, arm, topLeft, kBox, angle);
    } else {
      missing += 1;
    }
  }
  draw->AddRect(topLeft, ImVec2(topLeft.x + kBox, topLeft.y + kBox),
                IM_COL32(90, 90, 96, 255));
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextDisabled("Layers");
  for (const Layer &layer : layers) {
    ImGui::TextDisabled("%4d  %s%s%s", layer.z, layer.label.c_str(),
                        layer.worn ? "  (worn)" : "",
                        layer.texture == nullptr ? "  -- MISSING" : "");
  }
  if (gripPath) {
    ImGui::TextDisabled("%4d  %s  (grip path)", item->wornZ, item->id.c_str());
  }
  if (missing > 0) {
    ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.5f, 1.0f), "%d layer(s) have no sprite file.",
                       missing);
  }
  ImGui::TextDisabled("worn frames: %d / %d", wornFramesPresent, clip->frames);
  ImGui::EndGroup();

  ImGui::Separator();
  if (gripPath) {
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("grip offset", &gripOffset, -45.0f, 45.0f, "%.0f deg");
    ImGui::SameLine();
    static const int kSteps[] = {0, 15, 30, 45, 90};
    const char *kStepLabels[] = {"smooth", "15", "30", "45", "90"};
    int stepIndex = 0;
    for (int i = 0; i < 5; ++i) {
      if (gripAngleStep == kSteps[i]) stepIndex = i;
    }
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("angle step", &stepIndex, kStepLabels, 5)) {
      gripAngleStep = kSteps[stepIndex];
    }
    ImGui::TextDisabled(
        "The grip comes from the arm frame's pixels: the bottom is the hand, the "
        "shoulder-to-hand direction is the arm. No generation per weapon; angle snapping "
        "stands in for pre-drawn angles.");
  } else if (wornFramesPresent < clip->frames) {
    ImGui::TextDisabled(
        "No complete worn frame set for this item/clip yet. Generate it: the request "
        "includes the right arm's animation document so the model can put it in the hand.");
  }
  if (ImGui::Button(("Generate " + item->id + " " + clip->id + " frames").c_str())) {
    std::string error;
    if (game->designerWindow == nullptr) {
      error = "The Designer window is not available.";
    }
    if (error.empty() &&
        game->designerWindow->QueueWornClipArt(item->id, *rig, *clip, &error)) {
      status = "Queued " + item->id + " " + clip->id +
               " frames; watch the Designer transcript.";
      statusIsError = false;
    } else {
      status = error;
      statusIsError = true;
    }
  }
  if (!status.empty()) {
    if (statusIsError) {
      ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.5f, 1.0f), "%s", status.c_str());
    } else {
      ImGui::TextDisabled("%s", status.c_str());
    }
  }
  ImGui::TextDisabled(
      "Compare the three modes on each item. Frames are the best look but cost one request "
      "per item per clip; the grip path costs no generation per weapon but rotates one "
      "image, so judge the katana's curve and the sword's straight blade at the same "
      "angles.");
  ImGui::End();
}
