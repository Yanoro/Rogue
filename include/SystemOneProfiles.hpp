#pragma once

#include <string>
#include <vector>

#include "JevClassifier.h"

struct GameSettings;

// Turning the settings into the set of System 1 backends the game can switch
// between, and the rules for which one is active.
//
// Why resolution is a separate, pure step rather than something the Game or a window
// does inline: three different places need to agree about what a profile means --
// startup, the picker, and the headless harnesses that run before there is a Game.
// A second copy of the endpoint rule (in particular) is the kind of duplication that
// silently sends a decisions request to a chat endpoint.
namespace SystemOne {

// One resolved backend: everything needed to construct a classifier, plus the
// knobs that belong to the model rather than the game.
struct ModelProfile {
  std::string id;
  // Never empty: falls back to `id` during resolution, so every UI and log line has
  // something to show.
  std::string label;
  // "hosted_jev", "local_kev", or empty for a profile built from explicit fields
  // only. Kept for display -- the resolved `config` is what is actually used.
  std::string preset;
  JevClassifier::Config config;
  // Below this, a verdict is reported as unsure rather than acted on. Per profile
  // because it is calibration, which does not transfer between models.
  float minConfidence = 0.0f;
};

// The configured profiles, in a deterministic order: "default" first (built from the
// flat system1 fields), then the `profiles` array in file order.
//
// Always returns at least one entry. A config with no `profiles` array therefore
// yields exactly the single backend it did before profiles existed, which is what
// makes this change additive.
std::vector<ModelProfile> ResolveProfiles(const GameSettings &settings);

// Last-known health of one backend, as the picker shows it.
//
// "Untested" and "Unreachable" are different states and both are different from
// "usable but slow": a local model that has not been used yet is the normal case
// right after a switch, and reporting it as a failure would make every cold profile
// look broken.
enum class ProfileHealth {
  Untested,     // never probed or never used
  Reachable,    // the endpoint accepted a connection
  Unreachable,  // the endpoint refused or could not be resolved
  InUse,        // currently serving calls
};

struct SystemOneProfileStatus {
  ProfileHealth health = ProfileHealth::Untested;
  // Short reason or note for the status line: the probe error, or a reminder that a
  // local model may still be loading.
  std::string detail;
};

// Lookup by id, case-insensitive. Null when absent, which callers report rather than
// falling back -- silently switching to some other model than the one asked for is
// worse than refusing.
const ModelProfile *FindProfile(const std::vector<ModelProfile> &profiles,
                                const std::string &id);

// Which profile should be active, applying the documented precedence:
//   1. `cliProfile`    -- the --system1-profile flag
//   2. `persistedId`   -- the choice saved from a previous run
//   3. `settings.activeProfile`
//   4. "default"
//
// Returns the id of an existing profile, never a missing one: a stale persisted id or
// a typo in the config falls through to the next source rather than leaving the game
// with no classifier.
std::string ResolveActiveId(const std::vector<ModelProfile> &profiles,
                            const GameSettings &settings,
                            const std::string &persistedId,
                            const std::string &cliProfile = {});

// True for a URL that looks like a local server on this machine. Exposed because the
// timeout rule and the picker's "local" hint both need it.
bool IsLocalEndpoint(const std::string &endpoint);

}  // namespace SystemOne
