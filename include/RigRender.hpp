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
                                std::vector<std::string>* hiddenParts = nullptr) {
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
    layer.texture =
        ResolveSpriteTexture(std::string(), part.sprite, resources, nullptr);
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
                 const std::vector<WornLayer>& worn, float box) {
  const ImVec2 topLeft = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(box, box));
  const ImVec2 bottomRight(topLeft.x + box, topLeft.y + box);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(topLeft, bottomRight, IM_COL32(26, 26, 30, 255));

  for (const Layer& layer : Order(rig, resources, worn, nullptr)) {
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
