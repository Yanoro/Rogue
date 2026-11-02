#pragma once

#include <filesystem>
#include <string>

#include "IconRegistry.h"
#include "ResourceManager.h"
#include "SpritePaths.hpp"

class DebugLog;

namespace IconPaths {

inline bool LooksLikePath(const std::string &reference) {
  return reference.find('/') != std::string::npos ||
         reference.find('\\') != std::string::npos ||
         (reference.size() > 4 &&
          reference.compare(reference.size() - 4, 4, ".png") == 0);
}

// Resolves an icon id to a loaded path. Empty ids return false, because "no icon"
// is a legitimate state for an optional skill icon and must fall back to the
// name rather than to the missing mark.
//
// Unknown ids and icons whose artwork cannot load fall back to the fixed
// "missing" registry entry, then to the global unknown sprite. The global
// fallback for actors/objects/tiles is deliberately unchanged.
inline bool ResolveIconPath(const IconRegistry &icons, const std::string &id,
                            ResourceManager &resources, DebugLog *debugLog,
                            std::string *resolvedPath) {
  if (id.empty()) return false;

  const auto tryReference = [&](const std::string &reference,
                                const std::string &fallbackConvention) {
    if (LooksLikePath(reference)) {
      return ResolveSpriteTexture(reference, fallbackConvention, resources,
                                  debugLog, resolvedPath) != nullptr;
    }
    return ResolveSpriteTexture(std::string(), reference, resources, debugLog,
                                resolvedPath) != nullptr;
  };

  if (const IconDef *definition = icons.Get(id)) {
    // A regenerated icon may still carry an explicit sprite path from before the
    // marker existed. The Content Editor/Designer marker is the newer decision, so
    // prefer the generated PNG when it is present and chosen; otherwise fall back to
    // the authored path/convention.
    if (PrefersGeneratedSprite(definition->id)) {
      const std::string generated = GeneratedSpritePath(definition->id);
      if (!generated.empty() &&
          std::filesystem::exists(generated)) {
        if (tryReference(generated, definition->id)) return true;
      }
    }
    const std::string reference =
        definition->sprite.empty() ? definition->id : definition->sprite;
    if (tryReference(reference, definition->id)) return true;
  } else {
    // Compatibility: existing skill JSON used explicit paths or sprite
    // conventions before icons had ids. A picker now writes ids, but old data
    // should still draw.
    if (tryReference(id, id)) return true;
  }

  if (id != "missing") {
    if (const IconDef *missing = icons.Get("missing")) {
      const std::string reference =
          missing->sprite.empty() ? missing->id : missing->sprite;
      if (tryReference(reference, missing->id)) return true;
    }
  }

  const std::string unknown = UnknownSpritePath();
  if (resolvedPath != nullptr) *resolvedPath = unknown;
  std::error_code ec;
  return std::filesystem::exists(unknown, ec);
}

inline const Texture2D *ResolveIconTexture(const IconRegistry &icons,
                                           ResourceManager &resources,
                                           const std::string &id,
                                           DebugLog *debugLog) {
  if (id.empty()) return nullptr;
  std::string path;
  if (!ResolveIconPath(icons, id, resources, debugLog, &path) || path.empty()) {
    path = UnknownSpritePath();
  }
  if (const Texture2D *texture =
          ResolveSpriteTexture(path, std::string(), resources, debugLog)) {
    return texture;
  }
  return ResolveSpriteTexture(UnknownSpritePath(), std::string(), resources,
                              nullptr);
}

}  // namespace IconPaths
