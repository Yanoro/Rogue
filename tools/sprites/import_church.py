"""Write the object/tile JSON for the church sprite sheet.

The sprite is what the game draws, but every object still needs a template: the
factory spawns from templates and the ASCII art is the fallback when no sprite
resolves. Characters are deliberately unique because the placement radar prints
a glyph grid and a collision makes that grid unreadable.
"""
import json, os

# name: (display, description, components, glyph, rgb, category)
OBJECTS = {
    "church_door":  ("Church Door", "A heavy arched oak door bound with iron.", ["Interactable"], "d", (110, 70, 40), "wood"),
    "rose_window":  ("Rose Window", "A round stained-glass window in a stone frame.", ["Interactable"], "o", (90, 110, 170), "stone"),
    "stained_glass_window": ("Stained Glass Window", "An arched window of coloured glass.", ["Interactable"], "y", (90, 110, 170), "stone"),
    "bell_tower_top": ("Bell Tower", "The top of a stone tower, with a bell hanging in it.", ["Interactable", "Obstacle"], "t", (130, 130, 140), "stone"),
    "stone_archway": ("Stone Archway", "A freestanding arch of dressed stone.", ["Obstacle"], "n", (140, 140, 150), "stone"),
    "church_pew":   ("Church Pew", "A tall-backed pew for the congregation.", ["Interactable", "Obstacle"], "p", (120, 80, 45), "wood"),
    "church_bench": ("Church Bench", "A plain wooden bench for the congregation.", ["Interactable", "Obstacle"], "h", (120, 80, 45), "wood"),
    "pulpit":       ("Pulpit", "A raised wooden pulpit for preaching.", ["Interactable", "Obstacle"], "u", (120, 80, 45), "wood"),
    "altar":        ("Altar", "A stone altar under a white cloth.", ["Interactable", "Obstacle"], "a", (150, 150, 150), "stone"),
    "lectern":      ("Lectern", "A wooden lectern holding an open book.", ["Interactable", "Obstacle"], "l", (120, 80, 45), "wood"),
    "collection_box": ("Collection Box", "A wooden box with a slot in the lid.", ["Interactable", "Storage", "Obstacle"], "x", (120, 80, 45), "wood"),
    "chalice":      ("Chalice", "A golden chalice.", ["Portable"], "c", (218, 175, 60), "gold"),
    "cross":        ("Cross", "A plain wooden cross.", ["Interactable"], "+", (139, 90, 45), "wood"),
    "candle":       ("Candle", "A lit candle in a brass holder.", ["Interactable"], "i", (235, 225, 190), "gold"),
    "candelabra":   ("Candelabra", "A brass candelabra holding three candles.", ["Interactable", "Obstacle"], "b", (218, 175, 60), "gold"),
    "baptismal_font": ("Baptismal Font", "A stone font holding blessed water.", ["Interactable", "Obstacle"], "f", (150, 150, 150), "stone"),
    "censer":       ("Censer", "An iron censer, swung on a chain.", ["Interactable"], "e", (110, 110, 120), "iron"),
    "wall_banner":  ("Wall Banner", "A hanging banner on a wooden pole.", ["Interactable"], "g", (180, 60, 55), "cloth"),
    "potted_plant": ("Potted Plant", "A leafy plant in a clay pot.", ["Interactable", "Obstacle"], "j", (80, 150, 70), "plant"),
}

# Full-bleed sprites: these read as ground, so they belong in the tile catalog
# rather than as objects standing on it.
TILES = {
    "stained_glass_fragment": ("stained glass", "'", (140, 110, 180), (60, 40, 80), False, "A fragment of coloured glass, set into the floor."),
    "stone_floor_tile":       ("stone floor",   ".", (170, 170, 175), (80, 80, 86), False, "Dressed stone flags."),
    "red_carpet":             ("red carpet",    ",", (170, 40, 40), (90, 24, 24), False, "A worn crimson runner."),
    "mossy_stone_wall":       ("mossy stone wall", "#", (150, 150, 155), (70, 78, 66), True, "Stone wall with moss growing in the joints."),
}

SPRITES = "data/sprites"
OBJECTS_DIR = "data/objects"
TILES_DIR = "data/tiles"

def glyph_art(glyph, rgb):
    """A 4x4 fallback picture, so the object still reads without its sprite."""
    darker = tuple(max(0, c - 45) for c in rgb)
    art = [
        f" {glyph}{glyph} ",
        f"{glyph}{glyph}{glyph}{glyph}",
        f"{glyph}{glyph}{glyph}{glyph}",
        f" {glyph}{glyph} ",
    ]
    return art

written = []
for name, (display, desc, comps, glyph, rgb, cat) in OBJECTS.items():
    sprite = os.path.join(SPRITES, name + ".png")
    if not os.path.exists(sprite):
        print(f"  SKIP {name}: no sprite at {sprite}")
        continue
    doc = {
        "name": display,
        "description": desc,
        "components": comps,
        "art": glyph_art(glyph, rgb),
        "palette": {glyph: list(rgb) + [255]},
        "character": glyph,
        "characterColor": list(rgb) + [255],
        "backgroundColor": [0, 0, 0, 0],
        "nameTagColor": list(rgb) + [255],
    }
    if "Storage" in comps:
        doc["capacity"] = 20
    with open(os.path.join(OBJECTS_DIR, name + ".json"), "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    written.append(("object", name, display))

for name, (tile_name, ch, fg, bg, blocks, desc) in TILES.items():
    sprite = os.path.join(SPRITES, name + ".png")
    doc = {
        "name": tile_name,
        "description": desc,
        "character": ch,
        "characterColor": list(fg) + [255],
        "backgroundColor": list(bg) + [255],
        "blocksTile": blocks,
        "sprite": sprite,
    }
    with open(os.path.join(TILES_DIR, name + ".json"), "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    written.append(("tile", name, tile_name))

print(f"\nwrote {len(written)} templates:")
for kind, fname, display in written:
    print(f"  {kind:<6} {fname:<24} {display}")
