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

  // Forgets everything known about one path, so the next lookup re-stats the
  // filesystem and reloads.
  //
  // This is what makes a hot-swap possible. The memo above is a deliberate
  // performance decision with a documented cost -- "a sprite file added while the
  // game is running is not noticed until the next map load" -- and this is the
  // other half of that decision: a way to say "this one specifically has changed
  // on disk". Both halves matter, because forgetting only the `missing` mark
  // would still be beaten to the answer by a remembered *load failure* in
  // `textures`, and forgetting only `textures` would leave the absent mark that
  // TryGetTexture checks first.
  //
  // A loaded texture is released here, which is the point: the caller is about to
  // re-resolve the path and wants the new file, not the old image. Anything still
  // drawing the old texture from a raw pointer will dangle, so callers re-resolve
  // and re-attach rather than holding pointers -- which is exactly what the
  // Sprite component does (it stores a path, never a texture).
  //
  // Returns true when anything was actually forgotten, so a caller can tell
  // "there was nothing cached" from "the cache was invalidated".
  bool ForgetPath(const std::string &filePath);

  // Forgets every path. For a wholesale reload -- MapReloader's map swap -- rather
  // than for a single new sprite, and coarse on purpose: after it, the next frame
  // re-stats everything the world draws. Prefer ForgetPath when one thing changed.
  size_t ForgetAll();

private:
  // A null value is a remembered failure; see GetTexture.
  std::unordered_map<std::string, std::unique_ptr<raylib::Texture>> textures;
  std::unordered_set<std::string> missing;
};
