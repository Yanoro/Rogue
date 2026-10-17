#include "SystemOneProfiles.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

#include "Settings.h"

namespace SystemOne {
namespace {

std::string ToLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool EqualsIgnoreCase(const std::string &a, const std::string &b) {
  return a.size() == b.size() && ToLower(a) == ToLower(b);
}

// A chat endpoint is not a decisions endpoint. The shipped config's
// `model.endpoint` is `.../api/generate` -- Ollama's chat route -- and copying it
// into the profile is how a decisions body once got posted to a chat server, which
// fails as "model does not exist" rather than as an endpoint error.
bool IsChatEndpoint(const std::string &endpoint) {
  return endpoint.find("/api/generate") != std::string::npos;
}

// Applies the endpoint rule and the timeout rule to a config that may have come from
// a preset, explicit fields, or both.
void FinaliseConfig(JevClassifier::Config &config, long statedTimeoutSeconds) {
  if (!config.endpoint.empty() && IsChatEndpoint(config.endpoint)) {
    // Treated as "not stated": the preset's decisions endpoint stays in place.
    config.endpoint = JevClassifier::Preset::HostedJev().endpoint;
  }
  if (statedTimeoutSeconds > 0) {
    config.timeoutSeconds = statedTimeoutSeconds;
  } else if (config.IsLocalEndpoint()) {
    config.timeoutSeconds = JevClassifier::kLocalTimeoutSeconds;
  }
}

ModelProfile FromFlatSettings(const GameSettings &settings) {
  ModelProfile profile;
  profile.id = "default";
  profile.label = "Default";
  profile.preset.clear();

  // Same rules the section-level block has always had, so an existing config resolves
  // to exactly the backend it did before profiles existed.
  JevClassifier::Config config;
  if (!settings.system1.model.endpoint.empty()) {
    config.endpoint = settings.system1.model.endpoint;
  }
  if (!settings.system1.model.model.empty()) {
    config.model = settings.system1.model.model;
  } else if (!settings.model.model.empty()) {
    // Empty means "inherit the main model", which is why SystemOneSection clears the
    // field on load rather than letting ModelSection's llama3 default stand.
    config.model = settings.model.model;
  }
  config.apiKey = settings.system1.model.apiKey.empty() ? settings.model.apiKey
                                                        : settings.system1.model.apiKey;

  FinaliseConfig(config, settings.system1.hasTimeoutSeconds
                             ? settings.system1.timeoutSeconds
                             : 0);

  profile.config = config;
  profile.minConfidence = settings.system1.minConfidence;
  return profile;
}

ModelProfile FromAuthoredProfile(const GameSettings::SystemOneProfileSection &authored,
                                 const GameSettings &settings) {
  ModelProfile profile;
  profile.id = authored.id;
  profile.label = authored.label.empty() ? authored.id : authored.label;
  profile.preset = authored.preset;

  // Start from the named preset, or from the plain hosted defaults when a profile
  // names none. Starting from a preset is what keeps "local_kev" a single word
  // instead of three fields that must agree.
  JevClassifier::Config config;
  const std::string preset = ToLower(authored.preset);
  if (preset == "local_kev" || preset == "kev") {
    config = JevClassifier::Preset::LocalKev();
  } else if (preset.empty() || preset == "hosted_jev" || preset == "jev") {
    config = JevClassifier::Preset::HostedJev();
  } else {
    // Unknown preset: reported rather than silently treated as hosted, because a typo
    // in a two-word field would otherwise point the game at the wrong backend.
    std::cerr << "Settings: system1 profile '" << authored.id << "' names unknown preset '"
              << authored.preset << "'; using explicit fields only" << std::endl;
    config = JevClassifier::Preset::HostedJev();
    config.endpoint.clear();
  }

  if (!authored.model.endpoint.empty()) {
    config.endpoint = authored.model.endpoint;
  }
  if (!authored.model.model.empty()) {
    config.model = authored.model.model;
  }
  if (!authored.model.apiKey.empty()) {
    config.apiKey = authored.model.apiKey;
  } else if (config.apiKey.empty()) {
    // Falls back to the main key for the same reason the flat block does: an
    // unconfigured run shares the NPC backend's credential. A local profile never
    // reaches this because its preset carries the placeholder key.
    config.apiKey = settings.model.apiKey;
  }

  FinaliseConfig(config, authored.timeoutSeconds);

  profile.config = config;
  profile.minConfidence = authored.minConfidence;
  return profile;
}

}  // namespace

bool IsLocalEndpoint(const std::string &endpoint) {
  return endpoint.find("127.0.0.1") != std::string::npos ||
         endpoint.find("localhost") != std::string::npos ||
         endpoint.find("[::1]") != std::string::npos;
}

std::vector<ModelProfile> ResolveProfiles(const GameSettings &settings) {
  std::vector<ModelProfile> profiles;
  profiles.push_back(FromFlatSettings(settings));

  for (const GameSettings::SystemOneProfileSection &authored : settings.system1.profiles) {
    ModelProfile profile = FromAuthoredProfile(authored, settings);
    // An authored profile may deliberately reuse "default" to replace the flat block;
    // anything else is appended in file order.
    const auto existing = std::find_if(
        profiles.begin(), profiles.end(),
        [&profile](const ModelProfile &candidate) {
          return EqualsIgnoreCase(candidate.id, profile.id);
        });
    if (existing != profiles.end()) {
      // Duplicate ids are a config error worth naming: the picker and the persisted
      // choice both key on the id, so the second one would be unreachable.
      std::cerr << "Settings: system1 profile '" << profile.id
                << "' is declared more than once; the later entry replaces the earlier"
                << std::endl;
      *existing = std::move(profile);
    } else {
      profiles.push_back(std::move(profile));
    }
  }
  return profiles;
}

const ModelProfile *FindProfile(const std::vector<ModelProfile> &profiles,
                                const std::string &id) {
  for (const ModelProfile &profile : profiles) {
    if (EqualsIgnoreCase(profile.id, id)) return &profile;
  }
  return nullptr;
}

std::string ResolveActiveId(const std::vector<ModelProfile> &profiles,
                            const GameSettings &settings,
                            const std::string &persistedId,
                            const std::string &cliProfile) {
  // Each candidate is tried in order and only accepted when it names a profile that
  // exists, so a stale persisted id (a profile deleted from the config) or a typo
  // falls through instead of leaving the game without a classifier.
  for (const std::string *candidate : {&cliProfile, &persistedId}) {
    if (!candidate->empty() && FindProfile(profiles, *candidate) != nullptr) {
      return FindProfile(profiles, *candidate)->id;
    }
  }
  if (!settings.system1.activeProfile.empty() &&
      FindProfile(profiles, settings.system1.activeProfile) != nullptr) {
    return FindProfile(profiles, settings.system1.activeProfile)->id;
  }
  if (!profiles.empty()) return profiles.front().id;
  return "default";
}

}  // namespace SystemOne
