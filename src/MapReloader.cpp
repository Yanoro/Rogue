#include "MapReloader.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include "DebugLog.h"
#include "Defaults.h"

MapReloader::MapReloader(const std::string &mapsDir, DebugLog* debugLog) 
    : mapsDirectory(mapsDir.empty() ? MAPS_DIRECTORY : mapsDir), debugLog(debugLog) {
  RefreshMapList();
}

void MapReloader::RefreshMapList() {
  mapList.clear();

  if (!std::filesystem::exists(mapsDirectory)) {
    std::string errStr = "Maps directory does not exist: " + mapsDirectory + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
    return;
  }

  try {
    for (const auto &entry : std::filesystem::directory_iterator(mapsDirectory)) {
      if (entry.is_regular_file() && IsJsonMapFile(entry.path())) {
        mapList.push_back(entry.path().filename().string());
      }
    }
    std::sort(mapList.begin(), mapList.end());
    std::cout << "Found " << mapList.size() << " map files in "
              << mapsDirectory << std::endl;
  } catch (const std::exception &e) {
    std::string errStr = "Error reading maps directory: " + std::string(e.what()) + "\n";
    if (debugLog) debugLog->LogError(errStr);
    else std::cerr << errStr;
  }
}

std::string MapReloader::GetMapPath(size_t index) const {
  if (index >= mapList.size()) {
    return "";
  }
  return mapsDirectory + "/" + mapList[index];
}

bool MapReloader::IsJsonMapFile(const std::filesystem::path &filePath) const {
  if (filePath.extension() != ".json") {
    return false;
  }

  std::ifstream file(filePath);
  if (!file.is_open()) {
    return false;
  }

  try {
    auto j = nlohmann::json::parse(file);
    return j.contains("mapWidth") &&
           j.contains("mapHeight") &&
           j.contains("tileWidth") &&
           j.contains("tileHeight") &&
           j.contains("locations") &&
           j.contains("tileInfo") &&
           j.contains("positions");
  } catch (...) {
    return false;
  }
}
