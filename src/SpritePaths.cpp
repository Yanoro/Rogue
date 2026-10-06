#include "SpritePaths.hpp"

#include <cctype>
#include <iostream>
#include <string>

#include "Components.h"
#include "DebugLog.h"
#include "ResourceManager.h"

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

  if (!conventionId.empty()) {
    const std::string conventionPath =
        std::string(SPRITE_DIRECTORY) + conventionId + ".png";
    if (const Texture2D *texture =
            TryPath(conventionPath, resources, debugLog, false, resolvedPath)) {
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
