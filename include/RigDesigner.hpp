#pragma once

#include <string>
#include <vector>

#include "RigRegistry.h"

// Turning a designer reply into an actor rig.
//
// The model writes the *definition* -- a name, the brief for each canonical part -- and
// the existing sprite-document pipeline draws the art. That split is deliberate: one
// prompt asked to invent a whole aligned body and its six layers at once is where this
// kind of feature usually fails. Small briefs against a fixed frame are something a model
// is good at, and each layer can be redrawn without touching the others.
//
// The rig is humanoid and 32x32 by construction. That is a contract with the worn
// equipment art (a helmet is drawn over this frame), not a limitation to lift later
// without a new frame vocabulary: a wolf uses an actor sprite instead.
namespace RigDesigner {

inline constexpr int kReferenceSide = 32;

// The canonical parts, in the order the pane lists them. The ids are the contract: worn
// items hide parts by id ("hair"), and the frame coordinates below are what the art
// requests promise the model.
struct CanonicalPart {
  const char *id;
  int z;
};
inline constexpr CanonicalPart kCanonicalParts[] = {
    {"legs", 10},     {"body", 20},   {"arm_left", 25},
    {"arm_right", 25}, {"head", 30},   {"hair", 40},
};

struct Part {
  std::string id;
  // The prose the art request is built from: materials, colours, shape, what it covers.
  std::string brief;
  // The sprite convention id its art is requested under: rig_<slug>_<id>.
  std::string sprite;
  int z = 0;
  // Frame lists per clip, in playing order. Carried across a redraft when the part's
  // sprite base is unchanged, because a frame name is <sprite>_<clip>_fN.
  std::vector<RigPartClip> clips;
};

struct Draft {
  std::string name;
  std::string slug;
  std::string description;
  std::string designerNotes;
  int width = kReferenceSide;
  int height = kReferenceSide;
  std::vector<Part> parts;
  // Clips the rig can play, with the authoring intent kept for regeneration.
  std::vector<RigClip> clips;
  std::vector<std::string> warnings;
};

// Moves `from`'s clips (and the frame lists of parts whose sprite base survived) onto a
// newly drafted `to`. A redraft replaces the definition, not the animation work.
void CarryAnimations(Draft &to, const Draft &from);

// The id a name saves under. Public because the pane lets the name be edited before
// saving, and the id has to follow the new name.
std::string SlugFor(const std::string &name);

struct ApplyResult {
  bool ok = false;
  // The sentence fed back to the model for a retry.
  std::string error;
  Draft draft;
  std::string notes;
};

// Takes the raw reply text (prose, reasoning and one fenced JSON block), validates it and
// builds the draft. Never writes anything.
ApplyResult ApplyReply(const std::string &reply);

// The instruction file for the definition turn.
std::string BuildSystemPrompt(const std::string &promptFile);

// The frame every part's art request carries, shared by the pane and the prompt so a part
// is never requested without the coordinates the others are drawn against.
std::string FrameContract();

// Writes <directory>/<slug>.json. The generated layer is ours, so an existing file is
// replaced and `savedPath` says which; the hand-authored catalog is never the target.
bool Save(const Draft &draft, const std::string &directory, std::string *savedPath,
          std::string *error);

}  // namespace RigDesigner
