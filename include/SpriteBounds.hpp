#pragma once

// Alpha-tight bounds for a sprite texture. This is deliberately not a per-pixel
// collider: callers use the tight box for picking and for the AABB movement
// footprint. The image is read once per path, then cached because every actor of
// the same sprite shares it.

#include <algorithm>
#include <string>
#include <unordered_map>

#include "raylib-cpp.hpp"
#include "ResourceManager.h"

struct SpriteAlphaBounds {
  bool valid = false;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

inline const SpriteAlphaBounds &GetTextureAlphaBounds(
    const Texture2D &texture) {
  static std::unordered_map<unsigned int, SpriteAlphaBounds> cache;
  const auto cached = cache.find(texture.id);
  if (cached != cache.end()) return cached->second;
  SpriteAlphaBounds bounds;
  if (texture.id != 0 && texture.width > 0 && texture.height > 0) {
    Image image = LoadImageFromTexture(texture);
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    if (image.data != nullptr && image.width > 0 && image.height > 0) {
      const Color *pixels = static_cast<const Color *>(image.data);
      constexpr unsigned char kAlphaThreshold = 8;
      int minX = image.width;
      int minY = image.height;
      int maxX = -1;
      int maxY = -1;
      for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
          if (pixels[y * image.width + x].a <= kAlphaThreshold) continue;
          minX = std::min(minX, x);
          minY = std::min(minY, y);
          maxX = std::max(maxX, x);
          maxY = std::max(maxY, y);
        }
      }
      if (maxX >= minX && maxY >= minY) {
        bounds.valid = true;
        bounds.x = minX;
        bounds.y = minY;
        bounds.width = maxX - minX + 1;
        bounds.height = maxY - minY + 1;
      }
    }
    UnloadImage(image);
  }
  cache.emplace(texture.id, bounds);
  return cache[texture.id];
}

inline const SpriteAlphaBounds &GetSpriteAlphaBounds(
    const std::string &path, ResourceManager &resources) {
  static std::unordered_map<std::string, SpriteAlphaBounds> cache;
  const auto cached = cache.find(path);
  if (cached != cache.end()) return cached->second;

  SpriteAlphaBounds bounds;
  raylib::Texture *texture = resources.GetTexture(path);
  if (texture != nullptr && texture->id != 0 && texture->width > 0 &&
      texture->height > 0) {
    Image image = LoadImageFromTexture(*texture);
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    if (image.data != nullptr && image.width > 0 && image.height > 0) {
      const Color *pixels = static_cast<const Color *>(image.data);
      constexpr unsigned char kAlphaThreshold = 8;
      int minX = image.width;
      int minY = image.height;
      int maxX = -1;
      int maxY = -1;
      for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
          if (pixels[y * image.width + x].a <= kAlphaThreshold) continue;
          minX = std::min(minX, x);
          minY = std::min(minY, y);
          maxX = std::max(maxX, x);
          maxY = std::max(maxY, y);
        }
      }
      if (maxX >= minX && maxY >= minY) {
        bounds.valid = true;
        bounds.x = minX;
        bounds.y = minY;
        bounds.width = maxX - minX + 1;
        bounds.height = maxY - minY + 1;
      }
    }
    UnloadImage(image);
  }

  cache.emplace(path, bounds);
  return cache[path];
}
