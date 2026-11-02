#include "RigRegistry.h"

#include "DebugLog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

namespace {

std::string Slugify(const std::string& text) {
  std::string out;
  bool lastUnderscore = false;
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) != 0) {
      out.push_back(static_cast<char>(std::tolower(u)));
      lastUnderscore = false;
    } else if (!out.empty() && !lastUnderscore) {
      out.push_back('_');
      lastUnderscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

bool ParseRig(const nlohmann::json& document, ActorRig& out, std::string* error,
              std::vector<std::string>* warnings) {
  if (!document.is_object()) {
    if (error != nullptr) *error = "the entry is not a JSON object";
    return false;
  }
  out.name = document.value("name", std::string());
  out.id = Slugify(out.name);
  if (out.id.empty()) {
    if (error != nullptr) *error = "the entry has no \"name\", so it has no id";
    return false;
  }
  out.description = document.value("description", std::string());
  out.width = document.value("width", 32);
  out.height = document.value("height", 32);
  if (out.width <= 0 || out.height <= 0) {
    if (error != nullptr) *error = "width and height must be positive";
    return false;
  }

  // Clips first: a part's frame list names a clip, and the reference is checked once
  // both have been read.
  if (document.contains("clips") && !document["clips"].is_null()) {
    if (!document["clips"].is_array()) {
      if (warnings != nullptr) {
        warnings->push_back("\"clips\" is not an array; ignored");
      }
    } else {
      for (const auto& entry : document["clips"]) {
        if (!entry.is_object()) {
          if (warnings != nullptr) warnings->push_back("a clip is not an object; skipped");
          continue;
        }
        RigClip clip;
        clip.id = entry.value("id", std::string());
        if (clip.id.empty()) {
          if (warnings != nullptr) {
            warnings->push_back("a clip has no \"id\"; skipped");
          }
          continue;
        }
        const bool duplicate =
            std::any_of(out.clips.begin(), out.clips.end(),
                        [&](const RigClip& existing) { return existing.id == clip.id; });
        if (duplicate) {
          if (warnings != nullptr) {
            warnings->push_back("clip '" + clip.id + "' appears more than once; the later one wins");
          }
          out.clips.erase(std::remove_if(out.clips.begin(), out.clips.end(),
                                         [&](const RigClip& existing) {
                                           return existing.id == clip.id;
                                         }),
                          out.clips.end());
        }
        clip.movement = entry.value("movement", std::string());
        clip.frames = entry.value("frames", 1);
        clip.fps = entry.value("fps", 8);
        clip.loop = entry.value("loop", true);
        clip.brief = entry.value("brief", std::string());
        if (entry.contains("poses") && entry["poses"].is_array()) {
          for (const auto& pose : entry["poses"]) {
            if (pose.is_string()) clip.poses.push_back(pose.get<std::string>());
          }
        }
        if (clip.frames < 1 || clip.frames > 64) {
          if (warnings != nullptr) {
            warnings->push_back("clip '" + clip.id + "' frame count is out of range; clamped to 1..64");
          }
          clip.frames = std::clamp(clip.frames, 1, 64);
        }
        if (clip.fps < 1 || clip.fps > 60) {
          if (warnings != nullptr) {
            warnings->push_back("clip '" + clip.id + "' fps is out of range; clamped to 1..60");
          }
          clip.fps = std::clamp(clip.fps, 1, 60);
        }
        out.clips.push_back(std::move(clip));
      }
    }
  }

  if (!document.contains("parts") || !document["parts"].is_array() ||
      document["parts"].empty()) {
    if (error != nullptr) {
      *error = "a rig needs a non-empty \"parts\" array: the layers it is made of";
    }
    return false;
  }

  std::set<std::string> seen;
  for (const auto& entry : document["parts"]) {
    if (!entry.is_object()) {
      if (warnings != nullptr) warnings->push_back("a part is not an object; skipped");
      continue;
    }
    RigPart part;
    part.id = entry.value("id", std::string());
    part.sprite = entry.value("sprite", std::string());
    part.z = entry.value("z", 0);
    if (part.id.empty()) {
      if (warnings != nullptr) {
        warnings->push_back("a part has no \"id\"; skipped");
      }
      continue;
    }
    if (part.sprite.empty()) {
      if (warnings != nullptr) {
        warnings->push_back("part '" + part.id +
                            "' has no \"sprite\"; skipped");
      }
      continue;
    }
    if (entry.contains("clips") && !entry["clips"].is_null()) {
      if (!entry["clips"].is_object()) {
        if (warnings != nullptr) {
          warnings->push_back("part '" + part.id + "': \"clips\" is not an object; ignored");
        }
      } else {
        for (auto it = entry["clips"].begin(); it != entry["clips"].end(); ++it) {
          RigPartClip partClip;
          partClip.clip = it.key();
          if (!it.value().is_array()) {
            if (warnings != nullptr) {
              warnings->push_back("part '" + part.id + "': clip '" + partClip.clip +
                                  "' is not an array; skipped");
            }
            continue;
          }
          for (const auto& frame : it.value()) {
            if (frame.is_string()) partClip.frames.push_back(frame.get<std::string>());
          }
          if (partClip.frames.empty()) {
            if (warnings != nullptr) {
              warnings->push_back("part '" + part.id + "': clip '" + partClip.clip +
                                  "' has no frames; skipped");
            }
            continue;
          }
          part.clips.push_back(std::move(partClip));
        }
      }
    }
    if (!seen.insert(part.id).second) {
      if (warnings != nullptr) {
        warnings->push_back("part '" + part.id +
                            "' appears more than once; the later one wins");
        out.parts.erase(std::remove_if(out.parts.begin(), out.parts.end(),
                                       [&](const RigPart& existing) {
                                         return existing.id == part.id;
                                       }),
                        out.parts.end());
      }
    }
    out.parts.push_back(std::move(part));
  }
  if (out.parts.empty()) {
    if (error != nullptr) *error = "no part could be read";
    return false;
  }

  for (RigPart& part : out.parts) {
    part.clips.erase(
        std::remove_if(part.clips.begin(), part.clips.end(),
                       [&](const RigPartClip& partClip) {
                         const auto found = std::find_if(
                             out.clips.begin(), out.clips.end(),
                             [&](const RigClip& clip) { return clip.id == partClip.clip; });
                         if (found == out.clips.end()) {
                           if (warnings != nullptr) {
                             warnings->push_back("part '" + part.id + "': clip '" +
                                                 partClip.clip +
                                                 "' is not declared in \"clips\"; skipped");
                           }
                           return true;
                         }
                         if (static_cast<int>(partClip.frames.size()) != found->frames &&
                             warnings != nullptr) {
                           warnings->push_back("part '" + part.id + "': clip '" +
                                               partClip.clip + "' has " +
                                               std::to_string(partClip.frames.size()) +
                                               " frames, the clip declares " +
                                               std::to_string(found->frames));
                         }
                         return false;
                       }),
        part.clips.end());
  }
  return true;
}

}  // namespace

std::string RigRegistry::IdFor(const nlohmann::json& document) {
  return Slugify(document.value("name", std::string()));
}

void RigRegistry::Load(const std::string& directoryPath) {
  if (directoryPath.empty()) return;
  std::error_code ec;
  if (!std::filesystem::is_directory(directoryPath, ec)) return;  // normal

  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directoryPath, ec)) {
    if (entry.path().extension() == ".json") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  for (const std::filesystem::path& path : files) {
    std::ifstream in(path);
    if (!in.is_open()) continue;
    nlohmann::json document;
    try {
      in >> document;
    } catch (const std::exception& parseError) {
      if (debugLog) {
        debugLog->LogError("Actor rigs: " + path.string() + " is not JSON: " +
                           parseError.what());
      }
      continue;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!AddOrReplace(document, path.string(), &error, &warnings)) {
      if (debugLog) {
        debugLog->LogError("Actor rigs: " + path.string() + " refused: " + error);
      }
      continue;
    }
    if (debugLog) {
      for (const std::string& warning : warnings) {
        debugLog->LogWarning("Actor rigs: " + path.string() + ": " + warning);
      }
    }
  }
  if (debugLog) {
    debugLog->LogInfo("Actor rigs: " + std::to_string(rigs.size()) + " rig(s) from " +
                      directoryPath);
  }
}

bool RigRegistry::AddOrReplace(const nlohmann::json& document,
                               const std::string& source, std::string* error,
                               std::vector<std::string>* warnings) {
  (void)source;
  ActorRig parsed;
  if (!ParseRig(document, parsed, error, warnings)) {
    return false;
  }
  rigs[parsed.id] = std::move(parsed);
  return true;
}

const ActorRig* RigRegistry::Get(const std::string& id) const {
  const auto found = rigs.find(id);
  return found == rigs.end() ? nullptr : &found->second;
}

std::vector<const ActorRig*> RigRegistry::GetAll() const {
  std::vector<const ActorRig*> out;
  out.reserve(rigs.size());
  for (const auto& [id, rig] : rigs) {
    (void)id;
    out.push_back(&rig);
  }
  return out;
}

nlohmann::json RigRegistry::ToJson(const ActorRig& definition) const {
  nlohmann::json document = nlohmann::json::object();
  document["name"] = definition.name;
  if (!definition.description.empty()) {
    document["description"] = definition.description;
  }
  document["width"] = definition.width;
  document["height"] = definition.height;
  nlohmann::json clips = nlohmann::json::array();
  for (const RigClip& clip : definition.clips) {
    nlohmann::json entry = {{"id", clip.id}, {"frames", clip.frames},
                            {"fps", clip.fps}, {"loop", clip.loop}};
    if (!clip.movement.empty()) entry["movement"] = clip.movement;
    if (!clip.brief.empty()) entry["brief"] = clip.brief;
    if (!clip.poses.empty()) entry["poses"] = clip.poses;
    clips.push_back(std::move(entry));
  }
  if (!clips.empty()) document["clips"] = std::move(clips);

  nlohmann::json parts = nlohmann::json::array();
  for (const RigPart& part : definition.parts) {
    nlohmann::json entry = {{"id", part.id}, {"sprite", part.sprite}, {"z", part.z}};
    if (!part.clips.empty()) {
      nlohmann::json partClips = nlohmann::json::object();
      for (const RigPartClip& partClip : part.clips) {
        partClips[partClip.clip] = partClip.frames;
      }
      entry["clips"] = std::move(partClips);
    }
    parts.push_back(std::move(entry));
  }
  document["parts"] = std::move(parts);
  return document;
}
