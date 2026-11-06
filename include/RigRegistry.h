#pragma once

#include <algorithm>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class DebugLog;

// One part of an actor rig: a whole-rig-sized sprite drawn at a z-order.
//
// Parts are not cut out of a base image. Each is its own transparent PNG, already
// registered to the rig's frame, which is what lets a worn helmet cover the hair without
// anything being redrawn: the helmet is simply another layer at a higher z.
// A part's frames for one clip, in playing order.
//
// The sprites are ordinary sprite convention ids, so a frame is drawn and cached exactly
// like any other sprite. A part that does not animate in a clip has no entry and keeps its
// static `sprite`.
struct RigPartClip {
  std::string clip;
  std::vector<std::string> frames;
};

struct RigPart {
  // "legs", "body", "arm_right", "head", "hair". Stable ids, because equipment hides
  // parts by id and an animation moves them by id.
  std::string id;
  // Sprite convention id: data/sprites/<sprite>.png, resolved by the same order every
  // other sprite is (art pack, generated overlay, data/sprites).
  std::string sprite;
  // Draw order within the rig; higher is in front. Ties keep declaration order.
  int z = 0;
  // The clips this part draws, one frame list each.
  std::vector<RigPartClip> clips;
  // How this layer looks, in prose. Authoring intent kept with the rig: the animation
  // prompts are cut from it, so an existing rig can be animated without re-drafting.
  // Empty means the caller falls back to a generic "the <id> layer" description. Last
  // because existing aggregate initializations spell the other fields in order.
  std::string brief;
};

// One clip the rig can play.
//
// `frames` is the count every animating part must supply, and it lives on the clip rather
// than on each part so a mismatch is detectable. The brief and pose lines are the
// authoring intent kept with the rig: regeneration has to redraw from what was asked for,
// not from a list of frame names.
// One stored generation of a clip: a complete part -> frame-list set. The original
// generation is the frames that existed before variants; each later generation is one
// "Generate" run. Selecting a variant copies its frame lists into the parts, so the world
// keeps drawing one active set and every older set stays on disk.
struct RigClipVariant {
  std::string id;       // "original", "v1", "v2", ...
  std::string name;     // "Original", "Generation 1", ...
  std::string created;  // display-only timestamp
  std::unordered_map<std::string, std::vector<std::string>> parts;
};

struct RigClip {
  std::string id;
  // The movement template this clip was generated from, when it came from one. Empty
  // for a hand-written clip. The frames remain the rig's own; the movement is the intent
  // a re-draw starts from and the reason a skill can name this animation.
  std::string movement;
  int frames = 1;
  int fps = 8;
  bool loop = true;
  std::string brief;
  std::vector<std::string> poses;
  // Stored generations, oldest first. The part `clips` lists mirror `activeVariant`.
  std::vector<RigClipVariant> variants;
  std::string activeVariant;
};

// The sprite convention for one frame of a part's clip: <part sprite>_<clip>_f<index>.
//
// Deterministic on purpose: the rig JSON can declare its frames before the art exists, and
// the animation document (whose frames the compiler writes to <base>_<name>) lands exactly
// where the declaration points.
inline std::string VariantSuffix(const std::string &variantId) {
  return (variantId.empty() || variantId == "original") ? std::string() : variantId;
}

inline std::string AnimationFrameSprite(const std::string &partSprite,
                                        const std::string &clipId, int frame,
                                        const std::string &variantId = std::string()) {
  std::string base = partSprite + "_" + clipId;
  const std::string suffix = VariantSuffix(variantId);
  if (!suffix.empty()) base += "_" + suffix;
  return base + "_f" + std::to_string(frame);
}

// The document base an animation document is requested under. Its frames are written as
// <base>_f1.., which is what AnimationFrameSprite names.
inline std::string AnimationDocumentName(const std::string &partSprite,
                                         const std::string &clipId,
                                         const std::string &variantId = std::string()) {
  std::string base = partSprite + "_" + clipId;
  const std::string suffix = VariantSuffix(variantId);
  if (!suffix.empty()) base += "_" + suffix;
  return base;
}

// The next free generation suffix (v1, v2, ...) for a clip; "original" is not a
// generation and does not consume a number.
inline std::string NextClipVariantId(const RigClip &clip) {
  int next = 1;
  for (const RigClipVariant &variant : clip.variants) {
    if (variant.id.size() < 2 || variant.id.front() != 'v') continue;
    try {
      next = std::max(next, std::stoi(variant.id.substr(1)) + 1);
    } catch (...) {
    }
  }
  return "v" + std::to_string(next);
}

inline std::string ClipVariantCreated() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
  return buffer;
}

// A rectangle in the rig's source frame. The right/bottom edges are x+w / y+h, the
// spelling the frame contract uses.
struct RigRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

// Where a rig puts its body parts, hand and feet, in source pixels. The renderer draws
// full-canvas layers, so this is not a skeleton: it is the contract the art prompts and
// the equipment prompts are cut from. The default is the canonical 32x32 humanoid.
struct RigGeometry {
  int width = 32;
  int height = 32;
  RigRect head{11, 4, 10, 8};
  RigRect hair{10, 2, 12, 7};
  RigRect body{10, 11, 12, 11};
  RigRect armLeft{7, 12, 3, 11};
  RigRect armRight{22, 12, 3, 11};
  RigRect legs{12, 20, 10, 11};
  // The right hand (source pixels) and the soles of the feet.
  int handX = 24;
  int handY = 22;
  int footX = 16;
  int footY = 31;
};

inline nlohmann::json RigRectToJson(const RigRect &rect) {
  return nlohmann::json::array({rect.x, rect.y, rect.w, rect.h});
}

inline RigRect RigRectFromJson(const nlohmann::json &value, const RigRect &fallback) {
  if (!value.is_array() || value.size() != 4) return fallback;
  for (const auto &part : value) {
    if (!part.is_number_integer()) return fallback;
  }
  RigRect rect;
  rect.x = value[0].get<int>();
  rect.y = value[1].get<int>();
  rect.w = value[2].get<int>();
  rect.h = value[3].get<int>();
  if (rect.w <= 0 || rect.h <= 0) return fallback;
  return rect;
}

inline nlohmann::json RigGeometryToJson(const RigGeometry &geometry) {
  nlohmann::json out = nlohmann::json::object();
  out["width"] = geometry.width;
  out["height"] = geometry.height;
  out["head"] = RigRectToJson(geometry.head);
  out["hair"] = RigRectToJson(geometry.hair);
  out["body"] = RigRectToJson(geometry.body);
  out["arm_left"] = RigRectToJson(geometry.armLeft);
  out["arm_right"] = RigRectToJson(geometry.armRight);
  out["legs"] = RigRectToJson(geometry.legs);
  out["hand"] = nlohmann::json::array({geometry.handX, geometry.handY});
  out["foot"] = nlohmann::json::array({geometry.footX, geometry.footY});
  return out;
}

inline RigGeometry RigGeometryFromJson(const nlohmann::json &value) {
  RigGeometry geometry;
  if (!value.is_object()) return geometry;
  geometry.width = value.value("width", geometry.width);
  geometry.height = value.value("height", geometry.height);
  if (geometry.width <= 0) geometry.width = 32;
  if (geometry.height <= 0) geometry.height = 32;
  if (value.contains("head")) geometry.head = RigRectFromJson(value["head"], geometry.head);
  if (value.contains("hair")) geometry.hair = RigRectFromJson(value["hair"], geometry.hair);
  if (value.contains("body")) geometry.body = RigRectFromJson(value["body"], geometry.body);
  if (value.contains("arm_left")) geometry.armLeft = RigRectFromJson(value["arm_left"], geometry.armLeft);
  if (value.contains("arm_right")) geometry.armRight = RigRectFromJson(value["arm_right"], geometry.armRight);
  if (value.contains("legs")) geometry.legs = RigRectFromJson(value["legs"], geometry.legs);
  if (value.contains("hand") && value["hand"].is_array() && value["hand"].size() == 2) {
    geometry.handX = value["hand"][0].get<int>();
    geometry.handY = value["hand"][1].get<int>();
  }
  if (value.contains("foot") && value["foot"].is_array() && value["foot"].size() == 2) {
    geometry.footX = value["foot"][0].get<int>();
    geometry.footY = value["foot"][1].get<int>();
  }
  return geometry;
}

// A rigged actor: a set of part layers over one canvas.
//
// This is the rigged counterpart of ActorSpriteDef, not a kind of it. An actor sprite is
// one image and stays the right answer for a wolf or a slime; a rig exists so a body can
// be *assembled* -- equipped, animated, hidden part by part -- and pays for that with
// several layers and a shared frame.
struct ActorRig {
  std::string id;
  std::string name;
  std::string description;
  int width = 32;
  int height = 32;
  std::vector<RigPart> parts;
  std::vector<RigClip> clips;
  // Optional art style id. The style supplies the prose and, when the rig has no geometry
  // of its own, the proportions.
  std::string style;
  // Optional geometry override for this rig. Absent means the style's geometry, then the
  // canonical 32x32 humanoid.
  std::optional<RigGeometry> geometry;
};

// The actor-rig catalogue, loaded from data/rigs/<id>.json.
//
// Same contract as ActorSpriteRegistry: a directory of JSON documents, an id from the
// name, add-or-replace for the Content Editor, and a document round trip so the generic
// form can edit it.
class RigRegistry {
public:
  void Load(const std::string& directoryPath);

  // nullptr for an unknown id. The pointer is stable until the next add or replace.
  const ActorRig* Get(const std::string& id) const;
  std::vector<const ActorRig*> GetAll() const;
  size_t Count() const { return rigs.size(); }

  // The document as the Content Editor edits it.
  nlohmann::json ToJson(const ActorRig& definition) const;

  // Validates and stores `document`, replacing any entry with the same id. Parts must
  // name a sprite and have a unique id; a rig with no parts is refused rather than
  // stored as something that can never draw.
  bool AddOrReplace(const nlohmann::json& document, const std::string& source,
                    std::string* error, std::vector<std::string>* warnings);

  // The id a document is stored under: the slug of its name.
  static std::string IdFor(const nlohmann::json& document);

  void SetDebugLog(DebugLog* log) { debugLog = log; }

private:
  std::unordered_map<std::string, ActorRig> rigs;
  DebugLog* debugLog = nullptr;
};
