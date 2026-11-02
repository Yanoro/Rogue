# Appearance: a sprite, or the Unknown placeholder

**Every entity in the world and every preview in the editor draws exactly one of
two things: its sprite, or `data/sprites/unknown.png`.** There is no glyph plate,
no ASCII-art block and no third fallback.

This is a decision, not an accident: a missing asset is now *visible* as missing.
The previous precedence — sprite, then ASCII art, then a coloured glyph — meant a
template with no sprite looked deliberate, so nothing ever told you which content
had no art.

## Where it is implemented

| site | what it draws |
|---|---|
| `Game::DrawEntityVisual` (`src/GameECS.cpp:558`) | the world: tiles, objects, characters |
| map editor tile palette (`src/DebugWindows.cpp`) | tile sprite or Unknown |
| map editor object palette | template sprite or Unknown |
| map editor terrain inspector | the tile's sprite or Unknown |
| inventory / storage rows (`src/StorageWindow.cpp`) | item sprite or Unknown |
| `UnknownSpritePath()` (`src/SpritePaths.cpp`) | the one path, deliberately outside the convention namespace |

The placeholder is authored by `tools/sprites/make_unknown.py` — a 32×32 opaque
plate in the pack's slate with a warm question mark. It is opaque because most
cells that need it are **tiles**, and a transparent tile would show the clear
colour through the floor. An art pack can replace it by overwriting the file.

## The crash that forced the font question

Opening the map editor crashed with a heap-buffer-overflow inside
`stbtt__GetGlyphShapeTT`, reached from `DrawAsciiArtImGui` →
`ImFont::CalcTextSizeA`.

Root cause: the art font is `BmPlus_IBM_CGA.otb`, an **OpenType Bitmap** font.
ImGui's text path pushes it through stb_truetype, which parses glyph *outlines*.
A codepoint that exists in the font's cmap but has no outline data sends stb
reading past the end of the 17,800-byte font buffer. It was triggered by a newly
created object whose art used a glyph no earlier template had measured (`@`).

Three things fix it, in order of strength:

1. **Nothing renders ASCII art any more**, so no codepoint reaches stb from the
   game. This is the structural fix.
2. `DrawAsciiArtImGui` no longer uses a font at all: blocks and shades are
   rectangles, and text glyphs are skipped with the reason in the code. Nothing
   calls it today; it is guarded in case something does later.
3. The ImGui art font is no longer added in `Game::Init` — it had no consumer
   left, and loading a bitmap font that must not be rasterized invites the bug
   back.

## What still carries glyphs

- **Tile definitions** keep `character`/`characterColour`: `GetSurroundingsRadar`
  builds the agent's grid from them and the legend names them. They are
  perception, not appearance.
- **Object definitions** keep `character`/colours because the schema requires
  them, and the object designer still asks; nothing draws them. `art`/`palette`
  are accepted and ignored, with one warning per template that has art and
  *neither* an explicit nor a conventional sprite.
- **Character art data** (`AsciiArt` on races and equipment) is still parsed and
  attached, and nothing renders it. Removing the data model is a separate
  decision, and keeping it costs nothing while the direction is still open.

## Consequences to be aware of

- **Characters are placeholder until they have sprites.** Races attach sprites by
  convention id, and only `data/sprites/farmer.png` exists — no `human.png`, and
  `Map_Village_Quest.json`'s NPC has no explicit `sprite`. A village of Unknowns
  is the honest current state, not a regression in the renderer. Generating race
  and player sprites is content work that now has a very visible deadline.
- **The blueprint preview still draws glyphs** (`BlueprintPreview.hpp`). It is a
  schematic of a plan rather than the world's look, and it does not use the art
  font; switching it to sprites is a separate change.
- Debug overlays that print a glyph as text (the ASCII debug view) are unchanged;
  they are diagnostics, not appearance.
