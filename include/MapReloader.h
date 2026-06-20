#pragma once

#include <string>
#include <vector>
#include <filesystem>

class MapReloader {
public:
  MapReloader(const std::string &mapsDirectory = "./");

  // Get list of available map files in the directory
  const std::vector<std::string> &GetMapList() const { return mapList; }

  // Refresh the list of map files
  void RefreshMapList();

  // Get the full path of a map by index
  std::string GetMapPath(size_t index) const;

  // Get the directory path
  const std::string &GetDirectory() const { return mapsDirectory; }

private:
  std::string mapsDirectory;
  std::vector<std::string> mapList;

  bool IsJsonMapFile(const std::filesystem::path &filePath) const;
};
