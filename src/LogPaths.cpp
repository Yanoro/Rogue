#include "LogPaths.h"

#include "TimeUtils.hpp"

#include <cctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace LogPaths {
namespace {

std::string SafeName(const std::string &name) {
  std::string out;
  out.reserve(name.size());
  bool lastUnderscore = false;
  for (const unsigned char ch : name) {
    const bool ok = std::isalnum(ch) != 0 || ch == '-' || ch == '_';
    if (ok) {
      out.push_back(static_cast<char>(ch));
      lastUnderscore = ch == '_';
    } else if (!lastUnderscore) {
      out.push_back('_');
      lastUnderscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  if (out.empty()) {
    out = "agent";
  }
  if (out.size() > 64) {
    out.resize(64);
  }
  return out;
}

std::mutex &KeyMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, std::uint64_t> &AgentKeys() {
  static std::unordered_map<std::string, std::uint64_t> keys;
  return keys;
}

}  // namespace

const std::filesystem::path &SessionRoot() {
  static const std::filesystem::path root = []() {
    std::filesystem::path path =
        std::filesystem::path("logs") / TimeUtils::GetStartTimeString();
    std::error_code error;
    std::filesystem::create_directories(path, error);
    for (const char *sub : {"npcs", "designer", "system1", "tile-designer",
                            "navigator"}) {
      std::filesystem::create_directories(path / sub, error);
    }
    return path;
  }();
  return root;
}

std::filesystem::path Subdir(const std::string &name) {
  std::filesystem::path path = SessionRoot() / name;
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return path;
}

std::string AgentKey(const std::string &name, std::uint64_t entityId) {
  const std::string base = SafeName(name);
  std::lock_guard<std::mutex> lock(KeyMutex());
  auto &keys = AgentKeys();
  const auto found = keys.find(base);
  if (found == keys.end() || found->second == entityId || entityId == 0) {
    keys[base] = entityId;
    return base;
  }
  std::string key = base + "-" + std::to_string(entityId);
  keys[key] = entityId;
  return key;
}

std::filesystem::path NpcDir(const std::string &name, std::uint64_t entityId) {
  std::filesystem::path path =
      Subdir("npcs") / AgentKey(name, entityId);
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return path;
}

std::filesystem::path NpcFile(const std::string &name, std::uint64_t entityId,
                              const std::string &file) {
  return NpcDir(name, entityId) / file;
}

std::filesystem::path NavigatorFile(const std::string &name,
                                    std::uint64_t entityId) {
  return Subdir("navigator") /
         (AgentKey(name, entityId) + ".txt");
}

}  // namespace LogPaths
