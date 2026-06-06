#include "MapReloader.h"
#include <algorithm>
#include <iostream>

MapReloader::MapReloader(const std::string &mapsDirectory)
    : mapsDirectory(mapsDirectory) {
  RefreshMapList();
}

void MapReloader::RefreshMapList() {
  mapList.clear();

  if (!std::filesystem::exists(mapsDirectory)) {
    std::cerr << "Maps directory does not exist: " << mapsDirectory
              << std::endl;
    return;
  }

  try {
    for (const auto &entry : std::filesystem::directory_iterator(mapsDirectory)) {
      if (entry.is_regular_file() && IsJsonMapFile(entry.path().filename().string())) {
        mapList.push_back(entry.path().filename().string());
      }
    }
    std::sort(mapList.begin(), mapList.end());
    std::cout << "Found " << mapList.size() << " map files in "
              << mapsDirectory << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "Error reading maps directory: " << e.what() << std::endl;
  }
}

std::string MapReloader::GetMapPath(size_t index) const {
  if (index >= mapList.size()) {
    return "";
  }
  return mapsDirectory + "/" + mapList[index];
}

bool MapReloader::IsJsonMapFile(const std::string &filename) const {
  // Check if file ends with .json
  if (filename.length() < 5) {
    return false;
  }
  return filename.compare(filename.length() - 5, 5, ".json") == 0;
}
