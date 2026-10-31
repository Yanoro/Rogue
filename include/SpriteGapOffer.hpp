#pragma once

#include <raylib.h>

#include <string>

#include "Blueprint.hpp"
#include "BlueprintRegistry.h"
#include "BlueprintSnapshot.hpp"
#include "StyleDescriber.hpp"

class AI;
class DebugLog;
class TileRegistry;
class ObjectFactory;

// "Design a sprite for this gap": one gap in one design, carried from the Designer
// window to the Sprite Designer window.
//
// Why it is a thing of its own and not state on either window. The two windows are
// independent: either can be closed at any moment, and the vision read takes seconds.
// Putting the request on the window that started it would mean a window toggle strands
// an in-flight request, and putting it on the one that displays it would mean the
// rendering happens in a Draw() -- which is exactly the "unreachable to verification"
// mistake TileDesigner.hpp's header warns about. Game owns it, both windows read it,
// and the class itself is testable through --blueprint-snapshot.
//
// What it holds is deliberately only the *context* of the request: the design, the gap,
// the crop and what was read from it. It never decides the document -- pressing Send is
// a human action, and the document turn stays the one place a sprite is written.
class SpriteGapOffer {
public:
  enum class State {
    Reading,  // the vision read is in flight
    Ready,    // a style brief (or an explicit "no brief") is available
    Failed,   // the crop could not be rendered; nothing to send
  };

  // Renders the crop and starts the vision read. Returns false with `Status()`
  // describing why when the crop could not be rendered -- which is the only failure
  // that stops the flow; a missing vision backend degrades to a Ready offer with no
  // style brief rather than refusing, because the numeric brief alone is still a
  // usable request.
  bool Begin(const Blueprint &design, const MissingMaterial &gap,
             const TileRegistry &tiles, const ObjectFactory &objects, AI *vision,
             DebugLog *log, bool markGap = true,
             const std::string &description = std::string());

  // Polls the describer and uploads the crop texture. Called every frame from
  // Game::Update, so a closed window cannot stall the request.
  void Update();

  // Drops the offer without sending. The texture is released in the destructor.
  void Cancel();

  bool Active() const { return active; }
  State GetState() const { return state; }
  const std::string &Status() const { return status; }
  const Blueprint &Design() const { return design; }
  const MissingMaterial &Gap() const { return gap; }
  // False when the material exists and only its art is missing. The crop is unmarked in
  // that case: nothing about the world is unknown, so stamping a hole would be a lie
  // about a material the engine can already build.
  bool IsGap() const { return isGap; }
  // The designer's own words about what this asset is, when it asked for it. Empty when
  // the material was only absent from the catalog and nobody described it. This is the
  // brief the document turn builds from, so it is shown before Send.
  const std::string &Requirement() const { return requirement; }
  const StyleDescriber::Style &ReadStyle() const { return style; }
  const std::string &CropPath() const { return cropPath; }
  int CropWidth() const { return crop.image.width; }
  int CropHeight() const { return crop.image.height; }
  // How many cells the gap actually marks. Zero is possible and meaningful: a later
  // part can overwrite every cell the material was used on, in which case the design
  // references it but the crop has nothing to show.
  int GapCells() const { return gapCells; }

  // The crop as a texture for the preview, uploaded on first use. Null when the crop
  // is empty or the upload failed, which the panel reports rather than drawing nothing.
  const Texture2D *CropTexture();

  ~SpriteGapOffer();

  SpriteGapOffer() = default;
  SpriteGapOffer(const SpriteGapOffer &) = delete;
  SpriteGapOffer &operator=(const SpriteGapOffer &) = delete;

private:
  Blueprint design;
  MissingMaterial gap;
  BlueprintSnapshot::Snapshot crop;
  StyleDescriber::Job job;
  StyleDescriber::Style style;
  std::string status;
  std::string cropPath;
  std::string requirement;
  State state = State::Reading;
  bool active = false;
  bool isGap = true;
  bool textureTried = false;
  Texture2D texture{};
  int gapCells = 0;
};
