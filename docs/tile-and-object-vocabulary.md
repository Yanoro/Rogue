# Tiles and objects: the corrected vocabulary

**Status: rule applied; the data migration landed 2026-10-17.** This is the canonical statement the
other documents point at when they say "a wall is not a tile". The old wall/door/prop tile definitions
have been removed, the structural objects now exist, and both blueprints and maps use them.

## 1. The rule

**A tile is one cell's ground surface, and nothing else.**

Ground surfaces are tiles: `floor`, `wooden floor`, `stone floor`, `grass`, `path`, `packed earth`,
`tilled soil`, `water`, `sand`, `snow`, `red carpet`, a bridge deck. A ground tile may block
movement — water, a pit, deep mud — and it is still a tile. **Blocking is not what makes something
a wall.**

**A wall, door, gate, window, roof, fence, hedge, piece of furniture, fixture, crop, prop or
character is never a tile.** It is an **object**: an entity placed on or over the ground.

"Tile" is also not a synonym for "cell". A cell has one **ground tile** and may additionally hold
one or more **objects**. Saying "the wall tile at (12,7)" conflates the two: the tile there is
whatever ground the wall stands on; the wall is an object in that cell.

## 2. Why the distinction is load-bearing

The two are different systems, not two names for one record:

| | tile | object |
|---|---|---|
| definition | `TileDef` in `TileRegistry` | template in `ObjectFactory` |
| lives in | the map's ground layer (`Map::tileMap`) | an ECS entity |
| blueprint op | `paint`, `fill{paint}` | `place`, `fill{place}` |
| sprite convention | `data/sprites/tile_<slug>.png` | `data/sprites/<slug>.png` |
| per cell | exactly one ground tile | zero or more objects |
| identity to agents | a surface name, normally no `DisplayName` | an entity with `DisplayName`, components, interactions |
| blocking | `TileDef::blocksTile` says whether the **ground** is passable | `Obstacle` blocks movement |
| visible to agents as | a surface name, normally no `DisplayName`; the placement radar is the tile view | an entity with `DisplayName`, components, interactions |
| behaviour | none beyond blocking | whatever components the template composes |

A wall is therefore not "a tile with `blocksTile: true`". It is an object with `Obstacle`, and the
ground beneath it is still a ground tile. A door is an object too; it may be `Obstacle` until
opened and `Interactable`/`Evolvable` when it can change state.

`carve` is a third thing: it marks an opening (a hole in a wall, a window) and writes neither a
tile nor an object. It does not turn the cell into a "door tile".

## 3. Consequences for content and prose

- **Do not add a wall, door, roof, fence, fixture or prop to `data/tiles/`.** Those belong in
  `data/objects/` as object templates.
- **`paint` is for ground only.** A blueprint that needs a wall or a door uses `place` with an
  object template. `fill{paint}` floors an interior; `fill{place}` fills it with objects.
- **Tile art is ground art.** The tile prompt asks for a seamless, top-down ground surface. Wall
  and door art goes through the object prompt even when it must fill its cell edge-to-edge; it is
  still an object and its sprite is not named `tile_*` unless it really is ground.
- **The tile designer must redirect.** A request for a wall or door is an object request. The tile
  designer may say so and offer the nearest honest ground surface, but it must not define a wall or
  door.
- **The object designer must accept them.** A wall is `Obstacle`; a door is `Obstacle` plus whatever
  makes it open (`Interactable`, and/or `Evolvable` to a `_open` template).
- **A tile's `blocksTile` describes the ground, not the wall standing on it.** Movement, pathfinding
  and placement ask `Map::IsBlocked`, which combines a blocking ground tile with any object carrying
  `Obstacle` and `blocksMovement`. `Tile::blocksTile` alone is not the whole answer.
- **Structure is still an object.** Walls, fences and cliff faces carry a `Structural` tag so the
  name-tag renderer omits them; the radar, vision and rendering still show them. `ShowNameTag` is
  opt-in for objects that should be labelled; characters always show their name.
- **"Invisible wall" is not a tile failure mode.** A tile with no glyph and no sprite is an
  invisible patch of ground.

## 4. What changed in the migration

The data migration landed on 2026-10-17:

- **Removed the misclassified tile definitions:** `wooden wall`, `stone wall`, `mossy stone wall`,
  `fence`, `rock face`, `anvil`, `forge`, `water trough` and the generated `golden box`.
- **Added object templates:** `wooden_wall`, `stone_wall`, `mossy_stone_wall`, `fence`, `rock_face`,
  `forge`, `water_trough`, `workbench`, `golden_box` and `door`. Walls and cliff faces carry
  `Obstacle` + `BlocksVision` + `Structural`; a fence carries `Obstacle` + `Structural`; the door is
  `Interactable` only (there is no open/close mechanic).
- **Renamed the art:** `tile_wooden_wall.png` → `wooden_wall.png`, and the same for stone wall,
  fence and rock face. `mossy_stone_wall.png` already had the object name. The art pack's own
  `stone_wall.png` reference was updated too.
- **Converted 12 blueprints:** every structural `paint` became a `place` with the matching object id.
- **Converted all 12 affected maps:** 4,589 structural cells became a blocking object over the map's
  own base ground. The palette indices were kept, so no map layout shifted.
- **Engine support:** `Map::IsBlocked` now combines a blocking ground tile with objects whose
  `Obstacle::blocksMovement` is true; movement, pathfinding, wall repulsion and placement use it.
  `Structural` objects draw no name tag, while still rendering and appearing in the radar/vision.
  `ShowNameTag` is opt-in; the existing object templates carry it, the new structural templates do
  not.
- **`--gaps` after the migration:** 10 ground tiles, 48 objects, **0 undefined blueprint references**.

What remains is deliberately not a tile/object confusion:

- `data/generated/work/tile_art_worklist.json.log` and some old generated-art worklists are historical
  artifacts and still mention the old names. They are not loaded content.
- Doors are non-blocking until an open/close system exists.
- `%MATERIALS%` still offers every object template under `place`, including walls and doors; that is
  the intended vocabulary, not a tile entry.

Not tiles and still correct: `water`, `red carpet`, `tilled soil`, `sand`, `snow`, `path`, `grass`,
floors and bridge decks. Those are ground surfaces even when they block or are decorative.

## 5. See also

- `docs/blueprint-design.md` — the grammar and the corrected example
- `docs/sprite-style-prompt.md` — the ground-versus-object art prompts
- `docs/sprite-object-catalogue.md` — which asset is which kind
- `docs/sprite-designer-design.md` — the classifier and the two designers
- `data/tiles/TILEDESIGNER.md` — the tile designer's own rule
- `data/objects/OBJECTDESIGNER.md` — the object designer's own rule
