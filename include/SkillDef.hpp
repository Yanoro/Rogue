#pragma once

#include <cctype>
#include <string>
#include <vector>

#include "SourceScaling.hpp"

// How a skill is used.
//
// The four kinds exist because "skill" is the umbrella, not a synonym for a
// button: a profession and a combat move are both skills, and the difference is
// only whether they are always on or have to be activated.
//
//   Passive  always on, no button. Farming, Cooking, Blacksmithing.
//   Active   activated for a one-shot effect (a strike, a heal, a dash).
//   Hybrid   always-on effects AND an activation. Calling a skill with passive
//            effects "active" would misdescribe it, which is why this is its own
//            type rather than a flag.
//   Toggle   activated on and off; its effects persist only while it is on.
//            A stance or aura -- a distinct type because a toggle usually costs
//            a resource or has a downside while on, so the player has to decide
//            to keep paying it.
//
// A toggle's effects are NOT always-on (see SkillHasAlwaysOnEffects), which is
// what stops a stance from silently applying before it is ever switched on.
enum class SkillType { Passive, Active, Hybrid, Toggle };

// Case-insensitive, matching every other content vocabulary in the project.
// Returns false for an unknown name, leaving `out` untouched.
inline bool ParseSkillType(const std::string &name, SkillType &out) {
  std::string lower;
  lower.reserve(name.size());
  for (char c : name) {
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lower == "passive") {
    out = SkillType::Passive;
    return true;
  }
  if (lower == "active") {
    out = SkillType::Active;
    return true;
  }
  if (lower == "hybrid") {
    out = SkillType::Hybrid;
    return true;
  }
  if (lower == "toggle") {
    out = SkillType::Toggle;
    return true;
  }
  return false;
}

inline const char *SkillTypeName(SkillType type) {
  switch (type) {
    case SkillType::Passive: return "Passive";
    case SkillType::Active: return "Active";
    case SkillType::Hybrid: return "Hybrid";
    case SkillType::Toggle: return "Toggle";
  }
  return "Passive";
}

// True when the skill has a button. Passive is the only kind that does not.
inline bool SkillIsActivatable(SkillType type) {
  return type != SkillType::Passive;
}

// True when the skill's declared effects apply without being activated.
// A toggle's effects apply only while it is on, so it is deliberately false
// here: that is the difference between a stance and a passive.
inline bool SkillHasAlwaysOnEffects(SkillType type) {
  return type == SkillType::Passive || type == SkillType::Hybrid;
}

// What activating an activatable skill does.
//
// The numbers are deliberately sparse: a combat skill names its damage and its
// cadence, and everything else (stats, weapon, skill level) reaches the result
// through the ordinary fold. Nothing here duplicates a stat's value.
// A sustained skill held down rather than pressed once. Running is the first
// user; the data is generic so a future "channel" or "sprint" can share it.
struct HeldSkillData {
  bool enabled = false;
  std::string resource = "stamina";
  float speedMultiplier = 1.6f;
  float drainPerSecond = 10.0f;
  float regenPerSecond = 6.0f;
  float regenDelaySeconds = 1.0f;
  float minResource = 10.0f;
};

struct SkillActivation {
  // Take base damage and the swing time from the equipped weapon. The basic
  // attack is the reason this exists: its numbers are the weapon's, not the
  // skill's, so equipping a better sword changes both.
  bool useWeapon = false;

  // Used when useWeapon is false, and as the weapon's fallback when no weapon
  // is equipped.
  float baseDamage = 0.0f;

  // Seconds before it can be used again. Ignored when useWeapon is true: the
  // cooldown is then the weapon's swing time, resolved through the duration
  // fold so haste applies.
  float cooldownSeconds = 1.5f;

  // Centre-to-centre range in tiles. 1 is adjacent in the game's grid.
  float rangeTiles = 1.0f;

  // Whether using this skill starts the global cooldown. The basic attack is
  // exempt on purpose: it runs on its own weapon timer, so it keeps flowing
  // between skills instead of competing with them for the same beat.
  bool gcd = true;
  // Per-skill override of the global cooldown length. A negative value means
  // "use the character's default"; 0 explicitly means "no global cooldown".
  // The distinction matters for cooldown-0 skills such as Ice Lance: an
  // authored 0 must not silently turn into the 1.5 s default.
  float gcdSeconds = -1.0f;

  // Initial state of the right-click "Always casts" toggle. True only for the
  // basic attack, which is what keeps combat moving while the player (and, in
  // a later slice, the agent) is deciding.
  bool autoCast = false;

  // XP granted to the skill itself on a successful use, so an active skill
  // levels by being used exactly the way a passive one levels by being done.
  // 0 disables training for this skill.
  int xpPerUse = 0;

  // False for a self-buff or a utility skill that needs no target.
  bool requiresTarget = true;

  // Whether the player may start this cast while already walking. The default
  // keeps the historical behaviour: free movement during the cast. False means
  // activating it forces the player to a stop, and movement during the cast
  // cancels it. Only meaningful for cast-time skills.
  bool canCastWhileMoving = true;

  // Only meaningful when canCastWhileMoving is false. Preserve the movement the
  // character already had and let it slide to a stop; the cast is canceled by
  // movement *input* (or a clicked path), not by the slide itself. Held input
  // from before the cast is ignored until it returns to neutral.
  bool allowSliding = false;

  // Seconds of wind-up before the effect lands. 0 is instant.
  //
  // A cast is a real commitment: the cooldown starts when the cast begins, the
  // caster cannot start another skill until it resolves, and the target can step
  // out of range or interrupt it. That is what makes a telegraph a decision
  // rather than a slower button.
  float castSeconds = 0.0f;

  // Whether a cast of this skill can be interrupted. Most attacks can; a
  // deliberate "uninterruptible" cast is the exception, and the HUD marks it.
  bool interruptible = true;

  // Whether landing this skill interrupts the target's cast. This is the one
  // explicit interrupt rule: ordinary damage does not break concentration, so a
  // kick has to be a choice rather than something the basic attack does for free.
  bool interrupts = false;

  // ProjectileRegistry id this skill launches instead of applying its impact
  // instantly. Empty means the impact lands on the target directly.
  //
  // The skill still owns the impact (base damage, interrupt), and the projectile
  // owns only the flight: sprite, speed, size. That split is what lets the same
  // arrow visual carry a poisoned arrow later, and the same fireball visual carry
  // an area blast, without either record growing the other's fields.
  std::string projectile;

  // SkillshotRegistry id this skill launches at an aimed direction. A skillshot
  // is aimed, so it needs no target entity; the skill's range is the length of
  // the path.
  //
  // Mutually exclusive with `projectile`: a homing projectile follows a target,
  // an aimed one follows a direction. Keeping them separate mirrors the two
  // different satisfactions (hit the thing vs. hit the place) instead of one
  // union field with a "does this need a target" flag.
  std::string skillshot;

  // One-time resource cost paid when the skill actually commits. Empty means no
  // cost. Held skills use their sustained drain instead.
  std::string costResource;
  float costAmount = 0.0f;

  HeldSkillData held;

  // SurfaceRegistry id created at the impact tile, if any. The surface owns its
  // own triggers and lifetime; the skill only says where it is born.
  std::string surface;
  float surfaceDuration = 0.0f;
  int surfaceRadius = 0;
};

// Definition of one improvable skill, loaded from data/skills/<id>.json.
//
// A skill is a source exactly like a stat -- its level is a magnitude that links
// can scale an outcome by -- and what separates the two is only HOW the magnitude
// moves. A stat changes on level-up or a special event; a skill changes by being
// used (docs/stat-effects-design.md section 1).
//
// Stages are an OPEN set: any number, any names, declared in data. The engine
// only ever compares an integer stage index, so a future race or class can have a
// skill with three stages and no engine code changes at all.
//
// LEVEL CONVENTION. `level` runs 0..MaxLevel(), where 0 is untrained and
// MaxLevel() is the final stage at its final level -- 45 for five stages of nine.
// Stage and position within the stage are DERIVED, never stored, so the two can
// never disagree:
//
//   level  1 -> stage 0, within-stage 1   (Initiate 1/9)
//   level  9 -> stage 0, within-stage 9   (Initiate 9/9)
//   level 10 -> stage 1, within-stage 1   (Journeyman 1/9)
//   level 45 -> stage 4, within-stage 9   (Grandmaster 9/9)
//
// So "finishing the 9th level" is exactly the step that moves you up a stage.
struct SkillDef {
  std::string id;
  std::string name;
  // Flavour for [SKILLS], never a mechanical claim: the "affects harvesting"
  // clause is generated from the registered links (section 9.1).
  std::string description;

  // Optional IconRegistry id ("skill_attack") for the ability bar. Empty means
  // the slot falls back to the skill's name, so a skill is usable from data
  // alone without an art dependency. Explicit paths/conventions written before
  // the registry existed still resolve through the compatibility fallback.
  std::string icon;

  // How this skill is used. Defaults to Passive so every skill written before
  // the type existed keeps meaning "always on", which is what it already did.
  SkillType type = SkillType::Passive;

  // Only meaningful when SkillIsActivatable(type). A passive skill that declares
  // an activation block is a content error the loader reports and ignores.
  SkillActivation activation;

  // Stage names, lowest first. The size is the number of stages.
  std::vector<std::string> stages;
  int levelsPerStage = 9;

  // Which activities may train this skill at all: "harvest", "craft", or "*".
  //
  // A GUARD, not a selector. It never decides who trains what -- the subject
  // names the skill and the amount -- it only lets the loader report a mismatch
  // at startup. Without it, a recipe declaring `"trains": {"farming": 8}` would
  // be a silent no-op on every craft, which is precisely the class of bug the
  // startup hook check exists to catch.
  std::vector<std::string> trainedBy;

  // XP needed for one level while in a stage, indexed by stage. The last entry
  // repeats for any stage past the end of the list, and an empty list means a
  // flat default.
  //
  // Cost is constant within a stage on purpose: the stage is the meaningful unit
  // of the design, and a within-stage ramp can be added later without changing
  // how anything else reads.
  std::vector<int> xpPerStage;

  // Same meaning as StatDef::baseline and StatDef::scale, so one contribution
  // builder can serve both kinds of source. A skill's neutral is untrained, so
  // baseline is normally 0; `scale` is what keeps a coefficient readable across
  // 45 levels -- set it to levelsPerStage and a coefficient reads as "per stage".
  float baseline = 0.0f;
  float scale = 1.0f;

  static constexpr int kDefaultXpPerLevel = 100;

  // How this definition turns a level into an effect, on the same terms a stat
  // does. The range is the level range, so BaselineMode::Min measures from
  // untrained.
  SourceScaling Scaling() const {
    SourceScaling scaling;
    scaling.baseline = baseline;
    scaling.scale = scale;
    scaling.min = 0.0f;
    scaling.max = static_cast<float>(MaxLevel());
    return scaling;
  }

  int StageCount() const { return static_cast<int>(stages.size()); }

  // Total levels reachable: 45 for five stages of nine.
  int MaxLevel() const { return StageCount() * levelsPerStage; }

  // Stage a level falls in, clamped to the last stage. Untrained is stage 0.
  int StageForLevel(int level) const {
    if (level <= 0 || stages.empty() || levelsPerStage <= 0) {
      return 0;
    }
    const int stage = (level - 1) / levelsPerStage;
    return stage >= StageCount() ? StageCount() - 1 : stage;
  }

  // Position within the current stage: 0 when untrained, otherwise 1..levelsPerStage.
  int LevelWithinStage(int level) const {
    if (level <= 0 || levelsPerStage <= 0) {
      return 0;
    }
    return ((level - 1) % levelsPerStage) + 1;
  }

  // Empty string only for a skill that declares no stages at all.
  const std::string &StageName(int stage) const {
    static const std::string unknown = "";
    if (stages.empty()) {
      return unknown;
    }
    const int last = StageCount() - 1;
    const int clamped = stage < 0 ? 0 : (stage > last ? last : stage);
    return stages[static_cast<size_t>(clamped)];
  }

  // XP to go from `level` to `level + 1`. 0 means already at the cap.
  //
  // The cost comes from the stage the character is currently in: advancing out
  // of Initiate is Initiate work. Where a stage costs nothing, that would be an
  // infinite loop, so a non-positive entry is treated as the default.
  int XpForNextLevel(int level) const {
    if (level >= MaxLevel()) {
      return 0;
    }
    if (xpPerStage.empty()) {
      return kDefaultXpPerLevel;
    }
    const size_t index = static_cast<size_t>(StageForLevel(level));
    const int cost = index < xpPerStage.size() ? xpPerStage[index]
                                               : xpPerStage.back();
    return cost > 0 ? cost : kDefaultXpPerLevel;
  }
};
