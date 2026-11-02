#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "ContentKinds.h"
#include "Window.h"

class Game;

// The Content Editor: one window over the game's data registries.
//
// A registry is a row in ContentKinds, not a window. This class owns the parts that are
// the same for every kind -- the list, the selection, the working copy, the save path, the
// sprite preview -- and nothing that is specific to tiles or objects. Tiles and objects
// are the first two rows; blueprints, races and skills are meant to be more rows.
//
// The document is edited by one generic form over the JSON itself (bool, number, string,
// array, nested object), because a form per registry would be a form per registry. What
// the form cannot express is a *better way to edit a particular kind* -- a sentence to a
// model, say -- which is why ContentKinds::Kind is meant to grow a custom-panel hook
// rather than this class growing a branch per kind.
//
// Saving writes `content.editor/<kind>/<file>.json` and then applies the document to the
// live registry, so the file and the running world cannot disagree. The hand-authored
// catalog is never written to: it is git-tracked content, and an override is the thing
// that can be deleted to get back to it.
class ContentEditorWindow : public Window {
public:
  explicit ContentEditorWindow(Game *game,
                               flecs::entity owner = flecs::entity::null());

  void Draw() override;

  // Opens this editor on one registry entry. Used by the NPC spawner's "Edit
  // template" button; the window is already kept alive by Game.
  void SelectEntry(const std::string &kind, const std::string &entry);

private:
  // Rebuilds the working copy from the live registry. Also what "Revert" does.
  void ReloadDocument();
  void Save();

  void DrawKindAndList();
  void DrawDocumentPane();
  void DrawSpritePane();

  // Sprite variants. The convention file (`<id>.png`) is always the active one, because
  // that is what the world resolves; regenerating first copies it aside as `<id>.vN.png`,
  // and "activate" copies a variant back over it. No new resolution rule, so nothing
  // outside this window has to know variants exist.
  void ArchiveActiveSprite(const std::string &conventionId);
  std::vector<std::filesystem::path> SpriteVariants(
      const std::string &conventionId) const;
  void ActivateVariant(const std::string &conventionId,
                       const std::filesystem::path &variant);
  // Back to the pack's own art: remove the marker and let resolution fall back.
  void UseOriginalSprite(const std::string &conventionId);
  // Deletes a generated sprite and its document. The pack's own art is never deletable --
  // the row for it is not given a button.
  void DeleteVariant(const std::string &conventionId,
                     const std::filesystem::path &variant);
  // Re-resolve the selected asset's textures after a sprite changed under it.
  void RefreshSelectedAsset();

  const ContentKinds::Kind *Kind() const;
  // The registries, wrapped for the kind table. Built per frame; it only holds references.
  ContentKinds::World World_() ;

  Game *game = nullptr;

  // The selected kind and entry. Kept as ids rather than pointers: a save can replace the
  // definition the selection came from, and an id is what the next read takes.
  std::string kindId = "tiles";
  std::string entryId;

  // The working copy. Edited in place, saved or reverted; the registry is only touched on
  // save, so a half-typed document never reaches the world.
  nlohmann::json document;
  bool dirty = false;

  std::string status;
  bool statusIsError = false;

  // Actor-sprite picker state for the NPC template's "sprite" field. Session
  // state, like the Settings picker's filter: it should not survive a reopen.
  std::string npcSpriteFilter;
  bool showActorSpritesWithoutArt = false;
  std::string skillIconFilter;

  // A filter, because both catalogs are the whole world's vocabulary and a list of a few
  // hundred rows is not a list. A plain buffer rather than std::string so ImGui can edit
  // it in place, the same way the other windows' filters work.
  char filterBuffer[96] = {};
};
