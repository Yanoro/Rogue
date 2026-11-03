#include "HybridSpikeWindow.h"

#include "DesignerWindow.h"
#include "Game.h"
#include "RigRender.hpp"
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
  ImGui::SetNextWindowSize(ImVec2(560, 470), ImGuiCond_FirstUseEver);
  ImGui::Begin("Hybrid spike", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->hybridSpikeWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  const ActorRig *rig = game->rigRegistry.Get("orc_brute");
  const ItemDef *item = game->itemRegistry.Get("iron_sword");
  if (rig == nullptr || item == nullptr || item->wornSprite.empty()) {
    ImGui::TextWrapped(
        "This spike needs the orc_brute rig and the iron_sword item (with worn art).");
    ImGui::End();
    return;
  }

  ImGui::TextDisabled(
      "Hybrid experiment: existing frame-swapped body + generated sword frames, drawn at "
      "the item's z. No anchors, no rotation. Throwaway.");
  ImGui::TextDisabled("rig %s  |  item %s (%s)", rig->id.c_str(), item->id.c_str(),
                      item->wornSprite.c_str());

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
  ImGui::SameLine();
  ImGui::Checkbox("worn frames", &useWornFrames);

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

  // The sword's frame set, if one has been generated for this clip.
  std::vector<std::string> wornFrameSprites;
  int wornFramesPresent = 0;
  for (int i = 1; i <= clip->frames; ++i) {
    const std::string sprite =
        item->wornSprite + "_" + clip->id + "_f" + std::to_string(i);
    wornFrameSprites.push_back(sprite);
    if (SpriteResolves(std::string(), sprite, nullptr)) ++wornFramesPresent;
  }

  // Generated frames land asynchronously; forget the cached texture when a file changes.
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

  // Compose the body from the clip's frames, then the sword above it.
  std::vector<std::string> hidden = item->wornHides;
  std::vector<Layer> layers;
  for (const RigPart &part : rig->parts) {
    if (std::find(hidden.begin(), hidden.end(), part.id) != hidden.end()) continue;
    std::string sprite = part.sprite;
    for (const RigPartClip &partClip : part.clips) {
      if (partClip.clip != clip->id || partClip.frames.empty()) continue;
      const int index = std::min(static_cast<int>(partClip.frames.size()) - 1, frame);
      sprite = partClip.frames[static_cast<size_t>(index)];
    }
    Layer layer;
    layer.label = part.id;
    layer.texture = RigRender::ResolveRigSprite(sprite, game->resourceManager);
    layer.z = part.z;
    layers.push_back(std::move(layer));
  }
  std::string wornSprite = item->wornSprite;
  if (useWornFrames && wornFramesPresent == clip->frames) {
    wornSprite = wornFrameSprites[static_cast<size_t>(frame)];
  }
  Layer sword;
  sword.label = item->id;
  sword.texture = RigRender::ResolveRigSprite(wornSprite, game->resourceManager);
  sword.z = item->wornZ;
  sword.worn = true;
  layers.push_back(std::move(sword));
  std::stable_sort(layers.begin(), layers.end(),
                   [](const Layer &a, const Layer &b) { return a.z < b.z; });

  const ImVec2 topLeft = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(kBox, kBox));
  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(topLeft, ImVec2(topLeft.x + kBox, topLeft.y + kBox),
                      IM_COL32(26, 26, 30, 255));
  int missing = 0;
  for (const Layer &layer : layers) {
    if (layer.texture == nullptr) {
      ++missing;
      continue;
    }
    SetTextureFilter(*layer.texture, TEXTURE_FILTER_POINT);
    const SpriteRect fit =
        SpriteFitRect(*layer.texture, topLeft.x, topLeft.y, kBox, kBox);
    if (fit.width <= 0.0f || fit.height <= 0.0f) continue;
    draw->AddImage(
        static_cast<ImTextureID>(static_cast<intptr_t>(layer.texture->id)),
        ImVec2(fit.x, fit.y), ImVec2(fit.x + fit.width, fit.y + fit.height));
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
  if (missing > 0) {
    ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.5f, 1.0f),
                       "%d layer(s) have no sprite file.", missing);
  }
  ImGui::TextDisabled("worn frames: %d / %d", wornFramesPresent, clip->frames);
  ImGui::EndGroup();

  ImGui::Separator();
  if (wornFramesPresent < clip->frames) {
    ImGui::TextDisabled(
        "No complete sword frame set for this clip yet. Generate it: the request includes "
        "the right arm's animation document so the model can put the sword in the hand.");
  }
  if (ImGui::Button(("Generate " + clip->id + " sword frames").c_str())) {
    std::string error;
    if (game->designerWindow == nullptr) {
      error = "The Designer window is not available.";
    }
    if (error.empty() &&
        game->designerWindow->QueueWornClipArt(item->id, *rig, *clip, &error)) {
      status = "Queued sword frames for " + clip->id +
               "; watch the Designer transcript. They appear here as they land.";
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
      "Compare with 'worn frames' off (the static sword). Judge whether the frames put "
      "the grip in the hand and keep the blade plausible; if they do, the hybrid is worth "
      "a real data model. If not, the alignment needs anchors.");
  ImGui::End();
}
