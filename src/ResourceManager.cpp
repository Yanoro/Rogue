#include <iostream>
#include <string>
#include <unordered_map>
#include "RaylibException.hpp"
#include "raylib-cpp.hpp"
#include "ResourceManager.h"

ResourceManager::ResourceManager() {};

raylib::Texture* ResourceManager::GetTexture(std::string &filePath) {
  auto it = textures.find(filePath);
  if (it != textures.end()) {
    return it->second.get();
  }
  try {
    auto newTexture = std::make_unique<raylib::Texture>(filePath);
    auto result = textures.emplace(filePath, std::move(newTexture));
    return result.first->second.get();
  } catch (const raylib::RaylibException &e) {
    std::cerr << "Texture faild to load [" << filePath << "]: " 
      << e.what() << std::endl;
    return nullptr;
  }
  
}
