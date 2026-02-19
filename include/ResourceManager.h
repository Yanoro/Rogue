#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include "raylib-cpp.hpp"

class ResourceManager {
public:
  ResourceManager();
  raylib::Texture* GetTexture(const std::string &filePath);
private:
  std::unordered_map<std::string, std::unique_ptr<raylib::Texture>> textures;
};
