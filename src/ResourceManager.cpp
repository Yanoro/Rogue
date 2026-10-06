#include <iostream>
#include <memory>
#include <string>

#include "RaylibException.hpp"
#include "ResourceManager.h"
#include "raylib-cpp.hpp"

ResourceManager::ResourceManager() {};

raylib::Texture *ResourceManager::GetTexture(const std::string &filePath) {
  const auto found = textures.find(filePath);
  if (found != textures.end()) {
    // Either the texture, or a remembered failure (a null value). Returning here
    // is what keeps a bad path from being retried and logged every frame.
    return found->second.get();
  }

  try {
    auto texture = std::make_unique<raylib::Texture>(filePath);
    // A file that parses but yields no GPU texture (an empty or zero-sized image)
    // is a failure like any other, not something to draw.
    if (texture->id == 0) {
      std::cerr << "Texture loaded empty [" << filePath << "]" << std::endl;
      textures.emplace(filePath, nullptr);
      return nullptr;
    }
    // Sprites are pixel art drawn at or near 1:1, so the nearest sample is what
    // the artist drew. The default bilinear filter would blur every edge.
    texture->SetFilter(TEXTURE_FILTER_POINT);
    auto inserted = textures.emplace(filePath, std::move(texture));
    return inserted.first->second.get();
  } catch (const raylib::RaylibException &e) {
    std::cerr << "Texture faild to load [" << filePath << "]: " << e.what()
              << std::endl;
    textures.emplace(filePath, nullptr);
    return nullptr;
  }
}

bool ResourceManager::IsKnownBad(const std::string &filePath) const {
  const auto found = textures.find(filePath);
  return found != textures.end() && found->second == nullptr;
}

bool ResourceManager::IsKnownMissing(const std::string &filePath) const {
  return missing.find(filePath) != missing.end();
}

raylib::Texture *ResourceManager::TryGetTexture(const std::string &filePath) {
  // A remembered failure already reported itself; a remembered absence is the
  // ordinary "this content has no sprite" case and is deliberately silent.
  if (IsKnownBad(filePath) || IsKnownMissing(filePath)) {
    return nullptr;
  }
  if (!FileExists(filePath.c_str())) {
    missing.insert(filePath);
    return nullptr;
  }
  return GetTexture(filePath);
}

