#include "TrainingParsing.h"

void ParseTrainingGrants(const nlohmann::json &trains, const std::string &ownerId,
                         std::vector<SkillXp> &out,
                         const std::function<void(const std::string &)> &warn) {
  if (!trains.is_object()) {
    warn("Warning: '" + ownerId + "' has a non-object 'trains'; ignoring it.");
    return;
  }

  for (auto it = trains.begin(); it != trains.end(); ++it) {
    SkillXp grant;
    grant.skillId = it.key();

    if (!it.value().is_number_integer()) {
      warn("Warning: '" + ownerId + "' trains '" + grant.skillId +
           "' with a non-integer amount; skipping it.");
      continue;
    }
    grant.xp = it.value().get<int>();
    if (grant.xp <= 0) {
      warn("Warning: '" + ownerId + "' trains '" + grant.skillId +
           "' by " + std::to_string(grant.xp) +
           " XP, which teaches nothing; skipping it.");
      continue;
    }

    out.push_back(std::move(grant));
  }
}
