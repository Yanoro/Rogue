#pragma once

#include "RigRegistry.h"
#include "ResourceManager.h"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"

#include "imgui.h"
#include "raylib.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Drawing an actor rig and its worn layers.
//
// One place, because the Content Editor's preview and (later) the world renderer must
// agree about order and hiding: a helmet drawn behind the hair in one and in front in the
// other is a bug that reads as an art problem.
namespace RigRender {

// A rig layer's art: the normal convention first (so the art pack and the generated
// layer can override it), then the built-in data/sprites file.
//
// The fallback is not belt-and-braces. With art.pack configured, SpriteCandidatePaths
// looks *only* at the pack and the generated layer, and the rig art ships in
// data/sprites -- so the first preview drew an empty canvas and the worn toggles changed
// nothing. The pack owns the look when it has a layer; a pack that has never heard of
// rigs must not make one invisible.
inline const Texture2D *ResolveRigSprite(const std::string &sprite,
                                         ResourceManager &resources) {
  if (sprite.empty()) return nullptr;
  if (const Texture2D *texture =
          ResolveSpriteTexture(std::string(), sprite, resources, nullptr)) {
    return texture;
  }
  return ResolveSpriteTexture(std::string(SPRITE_DIRECTORY) + sprite + ".png",
                              std::string(), resources, nullptr);
}

// Which frame of which clip to draw. A null `clip` draws every part's static sprite,
// which is what every preview outside the Designer wants.
struct AnimationState {
  const RigClip *clip = nullptr;
  // Seconds since the clip started. The caller owns the clock, so a paused preview simply
  // stops advancing it.
  float time = 0.0f;
};

// The frame the clip is on at `time`. Negative time is not expected, but wrapping it is
// cheaper than reasoning about it.
inline int FrameIndex(const RigClip &clip, float time) {
  if (clip.frames <= 0 || clip.fps <= 0) return 0;
  int index = static_cast<int>(time * static_cast<float>(clip.fps));
  if (clip.loop) {
    index %= clip.frames;
    if (index < 0) index += clip.frames;
  } else {
    index = std::clamp(index, 0, clip.frames - 1);
  }
  return index;
}

// The sprite a part draws for `animation`: its own frames when it has them for that clip,
// its static sprite otherwise. Falling back rather than blanking is deliberate -- a part
// with no frames is a static layer, not a hole in the body.
inline std::string PartSprite(const RigPart &part, const AnimationState *animation) {
  if (animation == nullptr || animation->clip == nullptr) return part.sprite;
  const auto found = std::find_if(
      part.clips.begin(), part.clips.end(),
      [&](const RigPartClip &clip) { return clip.clip == animation->clip->id; });
  if (found == part.clips.end() || found->frames.empty()) return part.sprite;
  const int index =
      std::min(FrameIndex(*animation->clip, animation->time),
               static_cast<int>(found->frames.size()) - 1);
  return found->frames[static_cast<size_t>(index)];
}

// A worn item's layer: art resolved by the caller, ordering and hiding here.
struct WornLayer {
  std::string label;                 // item id, for the layer readout
  const Texture2D* texture = nullptr;
  int z = 0;
  std::vector<std::string> hides;    // part ids this layer covers
};

struct Layer {
  std::string label;
  const Texture2D* texture = nullptr;
  int z = 0;
  bool worn = false;
};

// The layers in draw order, with every part named in a worn layer's `hides` removed.
// Worn layers keep their own z, so an item can sit in front of the body but behind the
// head when that is what the art needs.
inline std::vector<Layer> Order(const ActorRig& rig, ResourceManager& resources,
                                const std::vector<WornLayer>& worn,
                                std::vector<std::string>* hiddenParts = nullptr,
                                const AnimationState* animation = nullptr) {
  std::vector<std::string> hidden;
  for (const WornLayer& layer : worn) {
    hidden.insert(hidden.end(), layer.hides.begin(), layer.hides.end());
  }

  std::vector<Layer> layers;
  for (const RigPart& part : rig.parts) {
    if (std::find(hidden.begin(), hidden.end(), part.id) != hidden.end()) {
      continue;
    }
    Layer layer;
    layer.label = part.id;
    layer.texture = ResolveRigSprite(PartSprite(part, animation), resources);
    layer.z = part.z;
    layers.push_back(std::move(layer));
  }
  for (const WornLayer& wornLayer : worn) {
    Layer layer;
    layer.label = wornLayer.label;
    layer.texture = wornLayer.texture;
    layer.z = wornLayer.z;
    layer.worn = true;
    layers.push_back(std::move(layer));
  }
  // Stable, so two parts with the same z keep the order the rig declared.
  std::stable_sort(layers.begin(), layers.end(),
                   [](const Layer& a, const Layer& b) { return a.z < b.z; });
  if (hiddenParts != nullptr) *hiddenParts = std::move(hidden);
  return layers;
}

// Draws the composed rig into a square at the current cursor and reserves the space.
// `box` is the side in pixels; the 32x32 rig is scaled up with point sampling, because
// that is what the world will do.
inline void Draw(const ActorRig& rig, ResourceManager& resources,
                 const std::vector<WornLayer>& worn, float box,
                 const AnimationState* animation = nullptr) {
  const ImVec2 topLeft = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(box, box));
  const ImVec2 bottomRight(topLeft.x + box, topLeft.y + box);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(topLeft, bottomRight, IM_COL32(26, 26, 30, 255));

  for (const Layer& layer : Order(rig, resources, worn, nullptr, animation)) {
    if (layer.texture == nullptr) continue;
    SetTextureFilter(*layer.texture, TEXTURE_FILTER_POINT);
    const SpriteRect fit =
        SpriteFitRect(*layer.texture, topLeft.x, topLeft.y, box, box);
    if (fit.width <= 0.0f || fit.height <= 0.0f) continue;
    draw->AddImage(
        static_cast<ImTextureID>(static_cast<intptr_t>(layer.texture->id)),
        ImVec2(fit.x, fit.y), ImVec2(fit.x + fit.width, fit.y + fit.height));
  }
  draw->AddRect(topLeft, bottomRight, IM_COL32(90, 90, 96, 255));
}

}  // namespace RigRender
