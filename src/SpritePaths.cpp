#include "SpritePaths.hpp"

#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "Components.h"
#include "DebugLog.h"
#include "ResourceManager.h"
#include "Settings.h"

std::string TileSpriteId(const std::string &tileName) {
  std::string slug = "tile_";
  bool pendingSeparator = false;
  for (const char raw : tileName) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (std::isalnum(c)) {
      if (pendingSeparator && slug.size() > 5) {
        slug.push_back('_');
      }
      pendingSeparator = false;
      slug.push_back(static_cast<char>(std::tolower(c)));
    } else {
      // Any run of spaces, punctuation or other separators becomes one
      // underscore, so "tilled soil" and "tilled  soil" name the same file.
      pendingSeparator = true;
    }
  }
  return slug;
}

namespace {

// The generated layer for an art pack, derived rather than required:
// packs/art/<name> gets packs/generated/<name>/sprites, which is the layout in
// docs/asset-generation-design.md. Falls back to a bare "generated/<name>" when
// the pack path has no grandparent to hang it off (a pack named by a single
// component), so a relative pack still gets a generated layer instead of
// silently none.
std::string DerivedGeneratedDirectory(const std::string &artPack) {
  const std::filesystem::path pack(artPack);
  const std::filesystem::path grandparent = pack.parent_path().parent_path();
  const std::filesystem::path base =
      grandparent.empty() ? std::filesystem::path("generated")
                          : grandparent / "generated";
  return (base / pack.filename() / "sprites").string();
}

// The two roots, resolved once. Settings are read at startup and never change,
// and the editor's template palette asks for a candidate list per template per
// frame, so rebuilding these paths every call would be pure waste -- the same
// reasoning that made ResourceManager memoise absent paths.
struct SpriteRoots {
  std::string artPackSprites;    // empty when no art pack is configured
  std::string generatedSprites;  // empty when not applicable
};

const SpriteRoots &Roots() {
  static const SpriteRoots roots = [] {
    SpriteRoots r;
    const GameSettings &settings = Settings();
    if (settings.art.pack.empty()) {
      // No art pack: data/sprites/ is the art, exactly as before the split.
      return r;
    }
    r.artPackSprites =
        (std::filesystem::path(settings.art.pack) / "sprites").string();
    r.generatedSprites = settings.art.generated.empty()
                             ? DerivedGeneratedDirectory(settings.art.pack)
                             : settings.art.generated;
    return r;
  }();
  return roots;
}

void Warn(DebugLog *debugLog, const std::string &message) {
  if (debugLog) {
    debugLog->LogWarning(message);
  } else {
    std::cerr << message << std::endl;
  }
}

const Texture2D *TryPath(const std::string &path, ResourceManager &resources,
                         DebugLog *debugLog, bool warnWhenMissing,
                         std::string *resolvedPath) {
  // Recorded before the attempt, so the warning below can fire on exactly the
  // call that discovered the problem. Without this the editor's per-frame palette
  // would re-warn for an authored path that is missing, once per frame, forever.
  const bool alreadyKnownMissing = resources.IsKnownMissing(path);
  const bool alreadyKnownBad = resources.IsKnownBad(path);

  raylib::Texture *texture = resources.TryGetTexture(path);
  if (texture != nullptr) {
    if (resolvedPath != nullptr) {
      *resolvedPath = path;
    }
    return texture;
  }

  if (!warnWhenMissing) {
    return nullptr;
  }
  // TryGetTexture is silent about absence, so the two cases are told apart here:
  // a path somebody authored but that is not on disk is a mistake worth seeing,
  // while the convention probe most content is expected to fail is not.
  if (!alreadyKnownMissing && resources.IsKnownMissing(path)) {
    Warn(debugLog, "Warning: sprite '" + path +
                       "' does not exist; using the ASCII look instead.");
  } else if (!alreadyKnownBad && resources.IsKnownBad(path)) {
    Warn(debugLog, "Warning: sprite '" + path +
                       "' could not be loaded; using the ASCII look instead.");
  }
  return nullptr;
}

}  // namespace

SpriteRootsInfo CurrentSpriteRoots() {
  const SpriteRoots &roots = Roots();
  return {roots.artPackSprites, roots.generatedSprites};
}

std::string GeneratedSpritePath(const std::string &conventionId) {
  if (conventionId.empty()) return {};
  const SpriteRoots &roots = Roots();
  if (roots.generatedSprites.empty()) return {};
  return (std::filesystem::path(roots.generatedSprites) / (conventionId + ".png"))
      .string();
}

std::vector<std::string> SpriteCandidatePaths(const std::string &conventionId) {
  std::vector<std::string> candidates;
  if (conventionId.empty()) {
    return candidates;
  }

  const SpriteRoots &roots = Roots();
  if (roots.artPackSprites.empty()) {
    // No art pack configured: the pre-split lookup, byte for byte. This is what
    // keeps every existing run and every existing map working unchanged.
    candidates.push_back(std::string(SPRITE_DIRECTORY) + conventionId + ".png");
    return candidates;
  }

  // The pack is the art, so its own sprites come first and win. The generated
  // layer is strictly additive: it supplies an appearance only where the pack
  // has none, which is what stops a generated image from shadowing hand-authored
  // art -- and why "load a pack with more art" is always an improvement.
  candidates.push_back(
      (std::filesystem::path(roots.artPackSprites) / (conventionId + ".png"))
          .string());
  if (!roots.generatedSprites.empty()) {
    candidates.push_back(
        (std::filesystem::path(roots.generatedSprites) / (conventionId + ".png"))
            .string());
  }
  return candidates;
}

bool SpriteResolves(const std::string &explicitPath,
                    const std::string &conventionId,
                    std::string *resolvedPath) {
  std::error_code existsError;

  // Same order as ResolveSpriteTexture, and it has to stay that way: this answers
  // "would that draw?", and a report that disagrees with the renderer is worse
  // than no report.
  if (!explicitPath.empty() &&
      std::filesystem::exists(explicitPath, existsError)) {
    if (resolvedPath != nullptr) *resolvedPath = explicitPath;
    return true;
  }
  for (const std::string &candidate : SpriteCandidatePaths(conventionId)) {
    if (std::filesystem::exists(candidate, existsError)) {
      if (resolvedPath != nullptr) *resolvedPath = candidate;
      return true;
    }
  }
  return false;
}

const Texture2D *ResolveSpriteTexture(const std::string &explicitPath,
                                      const std::string &conventionId,
                                      ResourceManager &resources,
                                      DebugLog *debugLog,
                                      std::string *resolvedPath) {
  if (!explicitPath.empty()) {
    if (const Texture2D *texture =
            TryPath(explicitPath, resources, debugLog, true, resolvedPath)) {
      return texture;
    }
  }

  // Convention lookup. Silence is the contract here -- most content has no sprite
  // and a warning per id per root would bury the log -- and the candidates come
  // from SpriteCandidatePaths so the renderer, the editor palettes and the gap
  // report all ask the same question in the same order.
  for (const std::string &candidate : SpriteCandidatePaths(conventionId)) {
    if (const Texture2D *texture =
            TryPath(candidate, resources, debugLog, false, resolvedPath)) {
      return texture;
    }
  }

  return nullptr;
}

bool AttachSprite(flecs::entity entity, const std::string &explicitPath,
                  const std::string &conventionId, ResourceManager &resources,
                  DebugLog *debugLog) {
  std::string resolvedPath;
  if (ResolveSpriteTexture(explicitPath, conventionId, resources, debugLog,
                           &resolvedPath) == nullptr) {
    return false;
  }
  entity.set<Sprite>(Sprite{resolvedPath});
  return true;
}
