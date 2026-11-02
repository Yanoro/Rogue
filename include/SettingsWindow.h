#pragma once

#include "SettingsSchema.h"
#include "Window.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>

class Game;

// Every value settings.json can carry, editable in place.
//
// The rows come from SettingsSchema::Fields(), so this window cannot fall out of
// step with the loader: a setting that exists is listed, and a setting that is
// listed is the one the loader reads. Writes go to settings.local.json beside
// the loaded settings file (see Settings.h) and apply to the running object
// immediately; a row whose consumer only reads at startup is tagged **restart**.
//
// Deliberately not persisted in debug_windows_state.json: unlike a debug overlay,
// this is a form, and coming back to a half-filled form after a restart is not
// obviously better than opening it again.
class SettingsWindow : public Window {
public:
  explicit SettingsWindow(Game *game);

  void Draw() override;

private:
  void DrawTree(int &index);
  void DrawFieldsFor(const std::string &sectionKey);
  void DrawProfiles();
  void DrawAIProfiles();
  void DrawAIProfileField(const SettingsSchema::AIProfileFieldSpec &field,
                          GameSettings::AIProfileSection &profile,
                          const std::string &profileId);
  void CommitProfileReference(const SettingsSchema::FieldSpec &field,
                              const nlohmann::json &value);
  // The player's actor sprite: a filter and a compact thumbnail grid drawn in
  // the row itself, so choosing a look does not open a second window.
  void DrawPlayerActorSpriteRow(const SettingsSchema::FieldSpec &field);
  void SelectActorSprite(const SettingsSchema::FieldSpec &field,
                         const std::string &id);
  // The corpse visual: an IconRegistry picker rather than a raw id string.
  void DrawDeathIconRow(const SettingsSchema::FieldSpec &field);
  void SelectDeathIcon(const SettingsSchema::FieldSpec &field,
                       const std::string &id);
  // Re-applies the player settings that land in components after a reload or
  // reset, so those rows are genuinely live rather than only when edited.
  void RefreshPlayerSettings();
  // Points a model block at `id`; an empty id forks the current profile's values
  // into the block's inline fields first, so "no profile" keeps what worked.
  void AssignProfile(const SettingsSchema::FieldSpec &field,
                     const std::string &id, const std::string &kind);
  void NewProfileForBlock(const SettingsSchema::FieldSpec &field,
                          const std::string &kind);
  void DrawField(const SettingsSchema::FieldSpec &field);
  void DrawProfileField(const SettingsSchema::CollectionFieldSpec &field,
                        GameSettings::SystemOneProfileSection &profile);

  bool Matches(const SettingsSchema::FieldSpec &field) const;
  void Commit(const SettingsSchema::FieldSpec &field,
              const nlohmann::json &value);
  void CommitProfiles();
  void Report(bool ok, const std::string &message);

  Game *game;
  std::string filter;
  std::string selectedSection;
  // The id typed into the add row of the ai.profiles section.
  std::string newProfileId;
  // The actor-sprite picker's filter. Session state: a filter that persisted
  // would hide sprites the next time the window opened.
  std::string actorSpriteFilter;
  bool showActorSpritesWithoutArt = false;
  std::string iconFilter;
  std::string status;
  bool statusOk = true;
  bool showRestartOnly = false;
  bool showOverriddenOnly = false;
  bool showSecrets = false;

  // Raw-JSON editor text, keyed by field path so it survives a changing filter.
  std::unordered_map<std::string, std::string> jsonBuffers;

  // Leaf paths the override file carries. Refreshed after every write and on
  // open, not per row per frame: 150 rows times 60 frames of file reads would be
  // a silly way to answer "is this one overridden".
  std::set<std::string> overrides;
  // Per block: how many keys the files set besides `profile`, so an override
  // hidden behind the picker is still reported.
  std::map<std::string, int> explicitBlockKeys;
  // Blocks/entries whose inline fields are revealed despite naming a profile.
  std::set<std::string> revealedBlocks;
  std::set<std::string> revealedEntries;
  bool overrideCacheDirty = true;
};
