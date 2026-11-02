#pragma once

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
};

// One clip the rig can play.
//
// `frames` is the count every animating part must supply, and it lives on the clip rather
// than on each part so a mismatch is detectable. The brief and pose lines are the
// authoring intent kept with the rig: regeneration has to redraw from what was asked for,
// not from a list of frame names.
struct RigClip {
  std::string id;
  int frames = 1;
  int fps = 8;
  bool loop = true;
  std::string brief;
  std::vector<std::string> poses;
};

// The sprite convention for one frame of a part's clip: <part sprite>_<clip>_f<index>.
//
// Deterministic on purpose: the rig JSON can declare its frames before the art exists, and
// the animation document (whose frames the compiler writes to <base>_<name>) lands exactly
// where the declaration points.
inline std::string AnimationFrameSprite(const std::string &partSprite,
                                        const std::string &clipId, int frame) {
  return partSprite + "_" + clipId + "_f" + std::to_string(frame);
}

// The document base an animation document is requested under. Its frames are written as
// <base>_f1.., which is what AnimationFrameSprite names.
inline std::string AnimationDocumentName(const std::string &partSprite,
                                         const std::string &clipId) {
  return partSprite + "_" + clipId;
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
