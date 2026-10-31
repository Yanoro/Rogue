#include "SpriteGapOffer.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>

#include "AI.h"
#include "DebugLog.h"
#include "ObjectFactory.h"
#include "TileRegistry.h"

namespace {

// The crop file is named from the material so a human looking in /tmp can tell which
// gap each picture belongs to. Nothing reads it back: the definition's name is the
// identity, and this is a viewing aid, not a cache.
std::string Slug(const std::string &name) {
  std::string out;
  for (unsigned char character : name) {
    if (std::isalnum(character) != 0) {
      out += static_cast<char>(std::tolower(character));
    } else if (!out.empty() && out.back() != '_') {
      out += '_';
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out.empty() ? "gap" : out;
}

}  // namespace

bool SpriteGapOffer::Begin(const Blueprint &designIn, const MissingMaterial &gapIn,
                           const TileRegistry &tiles, const ObjectFactory &objects,
                           AI *vision, DebugLog *log, bool markGap,
                           const std::string &description) {
  design = designIn;
  gap = gapIn;
  requirement = description;
  active = true;
  isGap = markGap;
  state = State::Reading;

  // Only *this* gap is marked. Stamping the design's other gaps would ask the model to
  // describe holes this document is not being asked to fill, and the prompt already
  // says not to describe what is missing -- showing it more than one would be baiting
  // it to ignore that. A material that exists and only lacks art marks nothing: the
  // building is complete without it.
  const std::unordered_set<std::string> missing =
      markGap ? std::unordered_set<std::string>{gap.material}
              : std::unordered_set<std::string>{};
  crop = BlueprintSnapshot::Render(design, missing, tiles, objects);
  if (!crop.ok) {
    state = State::Failed;
    status = "Could not render the building: " + crop.error;
    return false;
  }
  for (const BlueprintSnapshot::Gap &marked : crop.gaps) {
    if (marked.material == gap.material && marked.kind == gap.kind) {
      gapCells = static_cast<int>(marked.cells.size());
    }
  }

  // Written for a human, not for the model: the image is sent from memory, and a probe
  // whose pixels cannot be looked at is not one anybody should take on trust. A failure
  // to write is not a failure to send.
  cropPath = (std::filesystem::temp_directory_path() /
              ("airorogue-sprite-gap-" + Slug(gap.material) + ".png"))
                 .string();
  if (!BlueprintSnapshot::WritePng(crop.image, cropPath)) {
    cropPath.clear();
  }

  if (vision == nullptr) {
    state = State::Ready;
    status = "No vision backend is configured, so there is no style brief. The design "
             "and the gap are still fully described.";
    return true;
  }

  job.Configure(vision, log);

  std::string base64;
  if (!BlueprintSnapshot::EncodePng(crop.image, &base64, nullptr)) {
    state = State::Ready;
    status = "Could not encode the crop, so there is no style brief. Sending without "
             "one.";
    return true;
  }
  std::string requestReason;
  if (!job.Request(base64, BlueprintSnapshot::Describe(design, crop), &requestReason)) {
    state = State::Ready;
    status = "Could not start the style read (" + requestReason +
             "). Sending without one.";
    return true;
  }

  status = "Reading the art around the gap ...";
  return true;
}

void SpriteGapOffer::Update() {
  if (!active) return;
  if (state != State::Reading) return;

  StyleDescriber::Style read;
  if (!job.Poll(&read)) return;

  style = std::move(read);
  state = State::Ready;
  if (style.ok) {
    status = "Read the art around the gap.";
  } else {
    status = "The style read failed (" + style.error + "). Sending without one.";
  }
}

const Texture2D *SpriteGapOffer::CropTexture() {
  if (textureTried) {
    return texture.id != 0 ? &texture : nullptr;
  }
  textureTried = true;
  if (crop.image.data == nullptr) return nullptr;
  texture = LoadTextureFromImage(crop.image);
  if (texture.id == 0) return nullptr;
  // NEAREST: this is pixel art at one to one scale, and the point of showing it is what
  // the model saw, not a smoothed approximation of it.
  SetTextureFilter(texture, TEXTURE_FILTER_POINT);
  return &texture;
}

void SpriteGapOffer::Cancel() {
  active = false;
  job.Cancel();
}

SpriteGapOffer::~SpriteGapOffer() {
  job.Cancel();
  if (texture.id != 0) {
    UnloadTexture(texture);
    texture = Texture2D{};
  }
}
