#include "TileArt.hpp"

#include <unistd.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "SpritePaths.hpp"

namespace TileArt {

namespace {

// The palette a generated tile is locked to. Derived once from the art pack by
// `tools/palette.py derive` and then versioned, so this is a read of a committed
// file rather than something derived per run.
//
// The generated layer's own palette when that exists, else the art pack's, else
// nothing -- matching the layering rule that the generated layer is additive and
// yields to the pack.
std::string DerivedPalettePath() {
  const SpriteRootsInfo roots = CurrentSpriteRoots();
  std::vector<std::string> candidates;
  if (!roots.artPackSprites.empty()) {
    // sprites/ -> the pack root, where palette.json is derived to live.
    candidates.push_back(
        (std::filesystem::path(roots.artPackSprites).parent_path() / "palette.json")
            .string());
  }
  candidates.push_back("tools/sprites/palette.json");
  candidates.push_back("data/palette.json");
  for (const std::string &candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
  }
  return {};
}

// Runs a command and captures its combined output. Returns false when the process
// could not be started at all, which is different from it exiting non-zero.
bool RunCommand(const std::string &command, std::string &output, int &exitCode) {
  // popen rather than a reimplementation of the measurement: §7.3 records that
  // this exact metric had a bug and that the number matters more than the code.
  // Two implementations would be two numbers, and they would eventually disagree.
  FILE *pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return false;
  }
  std::array<char, 512> buffer{};
  size_t read = 0;
  while ((read = fread(buffer.data(), 1, buffer.size() - 1, pipe)) > 0) {
    buffer[read] = '\0';
    output += buffer.data();
  }
  const int status = pclose(pipe);
  exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return true;
}

// Quotes a path for the shell. Paths here come from settings and from a tile name
// that a model chose, so a space or a quote is expected input rather than an
// injection attempt -- but the command has to survive it either way.
std::string ShellQuote(const std::string &text) {
  std::string out = "'";
  for (const char c : text) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

// The recolour share out of palette.py's report, or -1 when the line is absent.
double ParseOutsidePercent(const std::string &output) {
  const std::string marker = "recolour share";
  const size_t at = output.find(marker);
  if (at == std::string::npos) return -1.0;
  const size_t percent = output.find('%', at);
  if (percent == std::string::npos) return -1.0;
  // Walk back to the start of the number: "   recolour share      : 0.0% (...)".
  size_t begin = percent;
  while (begin > at && (std::isdigit(static_cast<unsigned char>(output[begin - 1])) ||
                        output[begin - 1] == '.' || output[begin - 1] == '-')) {
    --begin;
  }
  try {
    return std::stod(output.substr(begin, percent - begin));
  } catch (const std::exception &) {
    return -1.0;
  }
}

// The exact count of opaque pixels outside the palette, or -1 when the line is
// absent.
//
// This exists because the percentage is rounded to one decimal by the tool, and one
// stray pixel in a 1024-pixel sprite is 0.09% -- which prints as "0.0%" while the
// count beside it says "1 opaque pixels are not in the palette". A contract of
// "every opaque pixel is in the palette" cannot be decided from the rounded figure:
// it would pass a sprite that violates it. The count is exact, so it is what the
// verdict uses; the percentage is kept only for the human-readable report.
int ParseOutsideCount(const std::string &output) {
  const std::string marker = "are not in the palette";
  const size_t at = output.find(marker);
  if (at == std::string::npos) return -1;
  // Walk back over the parenthesised number to its first digit.
  size_t end = at;
  while (end > 0 && !std::isdigit(static_cast<unsigned char>(output[end - 1]))) {
    --end;
  }
  size_t begin = end;
  while (begin > 0 && std::isdigit(static_cast<unsigned char>(output[begin - 1]))) {
    --begin;
  }
  if (begin == end) return -1;
  try {
    return std::stoi(output.substr(begin, end - begin));
  } catch (const std::exception &) {
    return -1;
  }
}

// A uniquely-named temporary directory, removed when it goes out of scope.
//
// palette.py's report takes a *directory* and measures every sprite in it. Pointing
// it at the generated layer would therefore measure every generated tile, so the
// answer for this sprite would depend on what else has been generated -- a sprite
// could pass or fail because of a neighbour. A directory holding exactly the one
// file makes the measurement mean what it says.
class ScopedMeasureDir {
 public:
  ScopedMeasureDir() = default;
  ScopedMeasureDir(const ScopedMeasureDir &) = delete;
  ScopedMeasureDir &operator=(const ScopedMeasureDir &) = delete;
  ~ScopedMeasureDir() {
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  }

  // Returns the directory, or an empty path on failure.
  std::filesystem::path Prepare(const std::string &spritePath) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    const std::filesystem::path directory =
        base / ("airogue-palette-check-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory, ec);
    if (ec) return {};
    std::filesystem::copy_file(spritePath, directory / "sprite.png",
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
      std::filesystem::remove_all(directory, ec);
      return {};
    }
    path = directory;
    return directory;
  }

  std::filesystem::path path;
};

}  // namespace

Target PlanFor(const std::string &tileName) {
  Target target;
  target.tileName = tileName;
  // The one place the naming rule is applied. TileSpriteId is also what the loader
  // and PaintTile pass to AttachSprite, so the name written here is by
  // construction the name looked up there -- that shared call is what keeps
  // "generated" and "resolved" from drifting apart.
  target.conventionId = TileSpriteId(tileName);
  target.generatedPath = GeneratedSpritePath(target.conventionId);

  const SpriteRootsInfo roots = CurrentSpriteRoots();
  if (!roots.artPackSprites.empty()) {
    target.packPath =
        (std::filesystem::path(roots.artPackSprites) /
         (target.conventionId + ".png"))
            .string();
    std::error_code ec;
    if (std::filesystem::exists(target.packPath, ec)) {
      target.packWins = true;
    }
  }

  std::string resolved;
  if (SpriteResolves("", target.conventionId, &resolved)) {
    target.resolvedPath = resolved;
  }
  return target;
}

CopyResult PlaceSprite(const Target &target, const std::string &sourcePng) {
  CopyResult result;

  if (target.conventionId.empty() || target.tileName.empty()) {
    result.error = "there is no tile name to place art for";
    return result;
  }
  if (target.generatedPath.empty()) {
    result.error =
        "no generated layer is configured, so there is nowhere to put the sprite "
        "without writing into the art pack -- which is never allowed. Set "
        "art.pack (art.generated is derived from it) or content.generatedTiles.";
    return result;
  }

  std::error_code ec;
  if (sourcePng.empty() || !std::filesystem::exists(sourcePng, ec)) {
    result.error = "the source sprite does not exist: " + sourcePng;
    return result;
  }

  // The rule §1.2 states and this function enforces: the art pack is pristine.
  // Both directions are refused -- writing the new file into the pack's directory,
  // and overwriting the pack's existing copy when the destination was
  // misconfigured to point there.
  if (!target.packPath.empty()) {
    const std::filesystem::path source = std::filesystem::weakly_canonical(sourcePng, ec);
    const std::filesystem::path pack = std::filesystem::weakly_canonical(target.packPath, ec);
    const std::filesystem::path generated =
        std::filesystem::weakly_canonical(target.generatedPath, ec);
    if (!pack.empty() && source == pack) {
      result.error = "refusing to write into the art pack (" + target.packPath +
                     "); generated art belongs in " + target.generatedPath;
      return result;
    }
    if (!pack.empty() && generated == pack) {
      result.error = "the generated sprite path resolves to the art pack (" +
                     pack.string() +
                     "); refusing to overwrite hand-authored art";
      return result;
    }
  }

  const std::filesystem::path destination(target.generatedPath);
  if (std::filesystem::exists(destination, ec)) {
    const std::filesystem::path a = std::filesystem::weakly_canonical(sourcePng, ec);
    const std::filesystem::path b = std::filesystem::weakly_canonical(destination, ec);
    if (!a.empty() && a == b) {
      result.ok = true;
      result.alreadyInPlace = true;
      result.path = destination.string();
      return result;
    }
    result.replaced = true;
  }

  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    result.error = "could not create " + destination.parent_path().string() + ": " +
                   ec.message();
    return result;
  }

  // Copied rather than moved: the caller's raw render is kept (§11.2) and is the
  // thing a re-cut or a palette re-derivation re-reads.
  std::filesystem::copy_file(sourcePng, destination,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    result.error = "could not copy " + sourcePng + " to " + destination.string() +
                   ": " + ec.message();
    return result;
  }

  result.ok = true;
  result.path = destination.string();
  return result;
}

PaletteCheck CheckPaletteLocked(const std::string &spritePath,
                                const std::string &paletteFile,
                                const std::string &paletteTool) {
  PaletteCheck check;

  std::error_code ec;
  const std::string palette =
      paletteFile.empty() ? DerivedPalettePath() : paletteFile;
  if (palette.empty() || !std::filesystem::exists(palette, ec)) {
    check.error =
        "no palette file to check against (looked for " +
        (paletteFile.empty() ? std::string("a derived palette") : paletteFile) +
        "); the lock is unverified";
    return check;
  }
  if (!std::filesystem::exists(paletteTool, ec)) {
    check.error = "no " + paletteTool + " to run the check with; the lock is "
                  "unverified";
    return check;
  }

  // A directory holding exactly this sprite, because palette.py measures a whole
  // directory and the generated layer accumulates every generated tile: measuring
  // there would make this sprite's verdict depend on what else has been generated.
  ScopedMeasureDir measure;
  const std::filesystem::path directory = measure.Prepare(spritePath);
  if (directory.empty()) {
    check.error = "could not stage a directory to measure " + spritePath +
                  " in isolation; the lock is unverified";
    return check;
  }

  const std::string command = "python3 " + ShellQuote(paletteTool) + " report" +
                              " --palette " + ShellQuote(palette) + " --sprites " +
                              ShellQuote(directory.string()) + " 2>&1";

  int exitCode = -1;
  if (!RunCommand(command, check.output, exitCode)) {
    check.error = "could not run " + paletteTool;
    return check;
  }
  check.ran = true;
  check.outsidePercent = ParseOutsidePercent(check.output);
  check.outsidePixels = ParseOutsideCount(check.output);
  // "1 sprites, 1024 opaque pixels". Also the guard against a vacuous pass: with
  // nothing opaque measured, "0 off-palette" means nothing at all.
  {
    const size_t marker = check.output.find("opaque pixels");
    if (marker != std::string::npos) {
      size_t end = marker;
      while (end > 0 && !std::isdigit(static_cast<unsigned char>(check.output[end - 1]))) {
        --end;
      }
      size_t begin = end;
      while (begin > 0 &&
             std::isdigit(static_cast<unsigned char>(check.output[begin - 1]))) {
        --begin;
      }
      if (begin != end) {
        try {
          check.opaquePixels = std::stoi(check.output.substr(begin, end - begin));
        } catch (const std::exception &) {
          check.opaquePixels = -1;
        }
      }
    }
  }
  if (check.outsidePixels < 0) {
    check.error = paletteTool +
                  " did not report a count of off-palette pixels (exit " +
                  std::to_string(exitCode) + ")";
    return check;
  }
  if (check.opaquePixels <= 0) {
    // A check on an image with no opaque pixels cannot fail, so it must not pass
    // either: that is the shape a broken sprite or an unreadable file would take.
    check.ok = false;
    check.error = "the sprite measured as having no opaque pixels, so the lock "
                  "could not be verified";
    return check;
  }
  // §7.3: the lock is applied before the downscale, so a sprite that has been
  // through the pipeline should have none. Decided on the exact count rather than
  // the rounded percentage -- see PaletteCheck::outsidePercent.
  check.ok = check.outsidePixels == 0;
  if (!check.ok) {
    check.error = "the sprite is not palette-locked: " +
                  std::to_string(check.outsidePixels) +
                  " opaque pixel(s) are outside the palette";
  }
  return check;
}

SwapResult SwapIn(const std::string &tileName, const std::string &sourcePng,
                  const std::string &paletteFile,
                  const std::function<size_t(const std::string &)> &swap) {
  SwapResult result;
  const Target target = PlanFor(tileName);

  result.copy = PlaceSprite(target, sourcePng);
  if (!result.copy.ok) {
    result.error = result.copy.error;
    return result;
  }

  // Checked after placement, not before: the check reads the file that will
  // actually be drawn, and the generated path is a different file from the source.
  // Before the swap, so a sprite that fails the contract still gets drawn (it is
  // ours, and it is recoverable) but never silently reports success.
  result.palette = CheckPaletteLocked(result.copy.path, paletteFile);

  if (swap) {
    result.refreshedCells = swap(target.conventionId);
  }

  result.ok = result.palette.ok;
  if (!result.ok && result.error.empty()) {
    result.error = result.palette.error;
  }
  return result;
}

}  // namespace TileArt
