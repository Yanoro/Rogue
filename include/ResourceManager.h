#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "raylib-cpp.hpp"

// Path-keyed texture cache, so an entity that names an image on disk does not
// reload it every frame.
//
// Failures are cached too: a path that could not be loaded is remembered as a
// null entry rather than retried and re-logged. That matters because the world
// renderer looks textures up per entity per frame (see the Sprite component), so
// an uncached failure would print the same error sixty times a second and bury
// the log. A failure is reported once, on the attempt that discovered it.
//
// Owned by Game for the lifetime of the process. The Sprite component stores only
// a path, never a pointer into here, so entity and texture lifetimes cannot
// dangle against each other.
class ResourceManager {
public:
  ResourceManager();

  // The cached texture, loading it on first request. Returns nullptr when the
  // path cannot be loaded, and does not log again for a path already known bad.
  raylib::Texture *GetTexture(const std::string &filePath);

  // Resolves a path that may simply not exist, and remembers which it was.
  //
  // This is the entry point for callers that ask about a path every frame -- the
  // map editor's template palette draws one cell per template per frame, and
  // without the memo a "does this file exist" check would hit the filesystem
  // sixty times a second for every template that has no sprite, which is most of
  // them. Absence is silent here: having no sprite is the normal case, not an
  // error. A file that exists but will not load still goes through GetTexture,
  // which is what reports it.
  //
  // The memo means a sprite file added while the game is running is not noticed
  // until the next map load. That matches the world path, where sprites are
  // resolved at spawn, so the two cannot disagree.
  raylib::Texture *TryGetTexture(const std::string &filePath);

  // Whether the path is already known to be unloadable, so callers that want to
  // probe quietly can skip a doomed attempt.
  bool IsKnownBad(const std::string &filePath) const;

  // Whether the path is already known not to exist on disk.
  bool IsKnownMissing(const std::string &filePath) const;

private:
  // A null value is a remembered failure; see GetTexture.
  std::unordered_map<std::string, std::unique_ptr<raylib::Texture>> textures;
  std::unordered_set<std::string> missing;
};
