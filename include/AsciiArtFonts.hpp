#pragma once

#include "AsciiArtTypes.hpp"
#include "imgui.h"
#include "raylib.h"

#include <cstdio>
#include <string>
#include <vector>

// Fonts for art cells.
//
// Two things cap how much detail an art block can carry, and this header is both
// of the fixes:
//
//  1. The game rasterizes its font once at the render font size (32px) and draws
//     nearly everything smaller, so an art cell of about 8-10px was a ~3:1
//     nearest-neighbour reduction of a 32px rasterization. Rasterizing the same
//     font at the size a cell is actually drawn at gives a 1:1 glyph, which is
//     what makes a 4x4 grid (8px cells) usable rather than mush.
//  2. raylib's default load covers 0x20-0x7E only, so the CP437 box-drawing
//     characters -- which join across cells -- and the Latin-1 letters the art
//     draws as glyphs (the humanoid face is `õ`) were unreachable. They are
//     requested here. The block and shade characters, which carry the real
//     sub-cell detail, are filled as rectangles instead and so need no font
//     support at all; see AsciiBlockRect. The vocabulary list lives in
//     AsciiArtTypes.hpp because the parser validates against it and must not
//     depend on a font header.
//
// The ImGui font is separate because the windows that preview art (the map
// editor's object palette, inventory rows) draw with ImGui's own font, which is
// ASCII-only. Without it those windows show blanks wherever art uses a block.

// Finds a codepoint in a loaded font, or nullptr when the font has no glyph for
// it. GetGlyphIndex() cannot be used for this: it returns 0 both for the font's
// first glyph and for "not found".
inline const GlyphInfo *FindGlyphInfo(const Font &font, int codepoint) {
  for (int i = 0; i < font.glyphCount; ++i) {
    if (font.glyphs[i].value == codepoint) {
      return &font.glyphs[i];
    }
  }
  return nullptr;
}

// A ladder of rasterizations of the game's font, one per art cell size.
class AsciiArtFonts {
public:
  // Deliberately coarse. A cell is only ever a handful of pixels, so the sizes
  // worth having are the ones a plausible grid produces on a 32px tile: 4x4 -> 8,
  // 3x3 -> 10, 5x5 -> 6, 6x6 -> 5 (rounded up to 6).
  static constexpr int kSizes[] = {6, 8, 10, 12, 16};
  static constexpr int kSizeCount = 5;

  // Rasterizes every ladder size from `fontPath`. A size that fails to load
  // comes back as raylib's default font, which still draws -- just not crisply.
  void Load(const std::string &fontPath) {
    Unload();
    std::vector<int> codepoints = ArtGlyphCodepoints();
    for (const int size : kSizes) {
      Font font = LoadFontEx(fontPath.c_str(), size, codepoints.data(),
                             static_cast<int>(codepoints.size()));
      // Drawn at exactly the size it was rasterized at, so point filtering is
      // correct here and keeps the bitmap look instead of smearing it.
      SetTextureFilter(font.texture, TEXTURE_FILTER_POINT);
      fonts.push_back(font);
    }
  }

  void Unload() {
    for (Font &font : fonts) {
      UnloadFont(font);
    }
    fonts.clear();
  }

  bool IsLoaded() const { return !fonts.empty(); }

  // The rasterization closest to the cell without being smaller than it, so a
  // glyph is never drawn upscaled. Falls back to the largest loaded size.
  Font ForCellSize(int cellSize) const {
    if (fonts.empty()) {
      return GetFontDefault();
    }
    for (size_t i = 0; i < fonts.size(); ++i) {
      if (kSizes[i] >= cellSize) {
        return fonts[i];
      }
    }
    return fonts.back();
  }

  // Every art codepoint the loaded font does not actually have, or a description
  // of them. Empty means all present.
  //
  // raylib reports a codepoint the font lacks as a glyph with an empty image:
  // LoadFontData fills a glyph's image only when stb_truetype finds it (rtext.c,
  // the `index > 0` branch), while a zeroed glyph still keeps its `value`, so it
  // is found by the lookup but has no pixels and no advance. That is the signal
  // used here. The space character is skipped because raylib synthesises an image
  // for it.
  std::string CoverageWarning() const {
    if (fonts.empty()) {
      return "the art font ladder is not loaded";
    }

    const Font &font = fonts.front();

    std::vector<int> missing;
    for (const int codepoint : ArtGlyphCodepoints()) {
      if (codepoint == ' ') {
        continue;
      }
      const GlyphInfo *glyph = FindGlyphInfo(font, codepoint);
      if (glyph == nullptr || glyph->image.width <= 0 ||
          glyph->image.height <= 0) {
        missing.push_back(codepoint);
      }
    }

    if (missing.empty()) {
      return "";
    }

    std::string out =
        std::to_string(missing.size()) +
        " glyph(s) are missing from this font and will draw as blanks:";
    for (const int codepoint : missing) {
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), " U+%04X", codepoint);
      out += buffer;
    }
    return out;
  }

  // Adds an ImGui font carrying the same codepoints, for the windows that preview
  // art with ImGui's own font. Returns nullptr if the file cannot be loaded, in
  // which case those windows fall back to ImGui's ASCII-only font and any
  // box-drawing character in the art draws as a blank.
  //
  // ImGui's built-in font cannot be extended, so this has to be a font added from
  // the file. This ImGui version keeps its font atlas texture dynamic (rlImGui
  // creates it on demand in its render pass), so adding it once, right after
  // rlImGuiSetup and before the first frame, is enough -- there is no reload step.
  static ImFont *AddImGuiArtFont(const std::string &fontPath, float pixelSize) {
    if (fontPath.empty()) {
      return nullptr;
    }

    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
    for (const int codepoint : ArtGlyphCodepoints()) {
      if (codepoint <= 0xFFFF) {
        builder.AddChar(static_cast<ImWchar>(codepoint));
      }
    }
    static ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);

    ImFontConfig config;
    config.OversampleH = 1;
    config.OversampleV = 1;
    config.PixelSnapH = true;

    return ImGui::GetIO().Fonts->AddFontFromFileTTF(fontPath.c_str(), pixelSize,
                                                    &config, ranges.Data);
  }

private:
  // Parallel to kSizes.
  std::vector<Font> fonts;
};

// The ImGui font that carries the art glyph codepoints, set once by Game::Init.
//
// A function-local static in an inline function: one instance for the whole
// program. It is a global because DrawAsciiArtImGui is called from windows (the
// storage window, the map editor palette) that have no path to Game.
inline ImFont *&ArtImGuiFont() {
  static ImFont *font = nullptr;
  return font;
}
