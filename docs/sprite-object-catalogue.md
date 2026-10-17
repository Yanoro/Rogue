# The sprite object catalogue

What to ask an image model for, in the order it is worth asking. The style prompt that
goes with this list is [sprite-style-prompt.md](sprite-style-prompt.md); paste that first,
then one of the sheets at the bottom.

Names in `code` are the filename the sprite should end up with, which is also the answer to
"what will the engine call this". A definition whose name has no sprite is what
`./AIRogue --gaps` reports in `data/gaps.json`.

Marks:

- **★ needed now** — the code already refers to it: a blueprint paints a ground tile nothing
  defines, or references an object whose template/art is missing.
- **◆ soon** — the next ring out: furniture a built house is visibly missing.
- **· later** — plausible, cheap to defer.
- `[tile]` — a seamless **ground** surface (floor, grass, path, water), seen from directly above,
  no outline. Walls, doors, roofs, fences and fixtures are **never** `[tile]`: they are objects,
  even when they fill the cell. Everything else is a cut-out object on magenta.
- `[icon]` — a held or carried thing. Same 32×32 cell; chunky silhouette beats detail
  because it is drawn small.

**Vocabulary, corrected 2026-10-17.** A **tile is ground only**. See
[tile-and-object-vocabulary.md](tile-and-object-vocabulary.md). The migration removed the old
wall/door/fence/anvil/forge/trough definitions from `data/tiles/`, added their object templates, and
renamed the wall/fence/rock sprites. This catalogue previously listed those assets as `[tile]`; that
form is historical and is not the target.

---

## 1. Openings: doors, gates, windows

Doors, gates, windows and hatches are **objects, not tiles**. Ten of the fourteen blueprints used to
paint a ground tile literally named `door` — 22 times between them — and no such ground tile should
exist. The 2026-10-17 migration converted them to `place: "door"`; the door is still the
highest-value single asset in the game: one cell-filling object texture turns a dozen buildings from
a hole in the wall into a door.

- `door` **★** — a cell-filling object: a closed plank door, two iron hinges, a ring handle, a dark
  timber frame around it, filling the whole cell
- `door_open` **◆** — the same doorway with the door swung inward over a dark interior
- `cellar_hatch` **◆** — a low wooden hatch with an iron pull ring
- `door_arched` **◆** — an arched oak door with two iron bands
- `door_studded` **·** — a heavy oak door studded with iron nails
- `stable_door` **◆** — a half door: a wooden lower leaf with the top half swung open
- `barn_door` **◆** — a wide double door of vertical boards with a diagonal brace
- `gate_wood` **★** — a pale wooden garden gate with a diagonal brace
- `gate_iron` **◆** — a black iron gate of vertical bars with a curved top
- `door_broken` **·** — a door hanging on one hinge, splintered at the edge
- `window_shuttered` **◆** — a small window closed by grey wooden shutters
- `window_leaded` **◆** — a small window of diamond panes in a leaded timber frame
- `window_arched` **◆** — a narrow arched window with a stone sill
- `window_boarded` **·** — a window boarded over with crossed planks
- `window_shop` **·** — a wide low shop window with a sill and a display ledge
- `window_cellar` **·** — a small dark cellar window with an iron grille
- `stairs_stone` **◆** — three worn stone steps
- `stairs_wood` **·** — a short flight of plank stairs
- `handrail` **·** — a short section of timber handrail

## 2. Sleeping and living

- `bed` **★** — a wooden bed: a straw mattress, a grey blanket folded at the foot, one
  pale pillow, a heavy dark timber frame
- `bed_double` **◆** — the same bed wide enough for two, two pillows, the blanket rumpled
- `bed_bunk` **·** — a two-tier bunk of rough planks
- `bed_pallet` **·** — a straw tick on the floor under one coarse blanket
- `cradle` **·** — a small wooden cradle on rockers, one folded cloth inside
- `nightstand` **◆** — a small square cabinet with one drawer and a brass knob
- `wardrobe` **◆** — a tall dark wooden closet, two doors and a brass latch
- `chest_drawers` **◆** — a low chest of three drawers with iron pulls
- `bookshelf` **◆** — a tall open shelf holding a few stacked books and a rolled scroll
- `shelf_wall` **★** — a short plank shelf on two brackets, two clay jars on it
- `table` **★** — a plain plank table with four square legs
- `table_long` **◆** — a long trestle table for a hall
- `chair` **★** — a ladder-back chair with a rush seat
- `stool` **★** — a three-legged stool with a round seat
- `bench` **◆** — a long low backless bench
- `rug_round` **◆** — a flat round braided rug in red and ochre rings
- `rug_rect` **·** — a worn rectangular woven rug
- `mirror` **·** — an oval mirror in a wooden frame on a small stand
- `hearth` **★** — a low stone hearth with a dark firebox and a timber mantel
- `curtain` **·** — a pair of rough cloth curtains on an iron rod
- `room_screen` **·** — a folding wooden screen with cloth panels
- `washstand` **◆** — a small table with a clay basin and a folded towel
- `wall_hook` **·** — an iron hook with a cloth hanging on it

## 3. Cooking and food

- `cauldron` **★** — a round black iron pot with a bail handle, hanging from a hook
- `pot_clay` **◆** — a wide clay cooking pot with a lid
- `kettle` **◆** — a small iron kettle with a spout and a short handle
- `griddle` **·** — a flat iron griddle with a short handle
- `keg` **★** — a small barrel with a wooden tap
- `jug` **◆** — a pale clay jug with a loop handle
- `bottle` **◆** — a dark glass bottle with a cork
- `tankard` **◆ [icon]** — a wooden tankard with two iron bands
- `plate` **· [icon]** — a pale clay plate with a rim
- `bowl` **· [icon]** — a small clay bowl
- `cheese_wheel` **◆ [icon]** — a pale round cheese with a wedge cut out
- `ham` **·** — a cured ham hanging from a hook
- `herbs` **◆** — a bundle of dried herbs hanging by its stems
- `churn` **·** — a tall butter churn with a plunger
- `cutting_board` **·** — a board with a knife and half a loaf
- `firewood_stack` **★** — a stack of split logs against a wall

The tied sack of flour already exists (`flour`), so a sack of grain can reuse that shape.

## 4. Light

The lit versions are what make a village read as inhabited at night, and they are cheap:
the same object with a flame.

- `lantern` **★** — a square iron lantern with glass panes and a flame inside
- `lantern_lit` **◆** — the same lantern from a different angle, flame brighter
- `sconce` **◆** — an iron wall bracket holding a candle
- `torch` **◆** — a pitch torch in an iron wall ring, burning
- `brazier` **◆** — a squat iron brazier of glowing coals
- `street_lamp` **◆** — an iron post with a hanging lantern
- `chandelier` **·** — an iron ring chandelier carrying several candles
- `oil_lamp` **·** — a small clay oil lamp with a spout
- `campfire` **◆** — a ring of stones around burning logs
- `candle_lit` **◆** — a candle with a flame (the plain `candle` exists)
- `fireplace_lit` **·** — a hearth with a fire burning in it

## 5. Containers and storage

- `barrel` **★** — a staved oak barrel with two iron hoops
- `crate` **★** — a slatted pale-plank box
- `sack` **★** — a lumpy burlap sack tied at the neck
- `bucket` **★** — a wooden bucket with an iron band and a rope handle
- `basket` **◆** — a woven wicker basket with a handle
- `chest_open` **◆** — the chest with its lid raised (the closed `chest` exists)
- `strongbox` **◆** — a small iron-bound box with a heavy padlock
- `cupboard` **◆** — a low two-door cupboard
- `barrel_open` **·** — a barrel with the lid off, dark inside
- `crate_broken` **·** — a crate with sprung slats
- `grain_sack_open` **·** — a sack rolled open with a scoop in the grain
- `amphora` **·** — a tall clay jar with two handles
- `jar_sealed` **·** — a clay jar sealed with wax and cloth
- `pouch` **· [icon]** — a small leather pouch with a drawstring

## 6. Smithing and metal

A forge, an anvil and a workbench are **objects, not ground**. The migration added the `forge` and
`workbench` templates and retired the old `data/tiles/forge.json` and `data/tiles/anvil.json`; `anvil`
already existed as an object (`data/sprites/anvil.png`), and the generated forge art was renamed to
`forge.png`.

- `forge` **★** — a stone forge hearth as a cut-out object: glowing coals and a dark hood
- `workbench` **★** — a scarred workbench top with tools and nails, as a cut-out object
- `bellows` **◆** — a teardrop bellows of wood and leather with two handles
- `quench_barrel` **◆** — a half barrel of dark water with a ladle
- `grindstone` **◆** — a round grey stone wheel on a timber frame with a treadle
- `tool_rack` **◆** — a wall board with tongs, a hammer and a chisel hanging
- `ore_pile` **◆** — a heap of grey rock shot through with orange iron flecks
- `coal_pile` **◆** — a heap of black coal lumps
- `ingot_stack` **◆** — a stack of grey iron ingots
- `ingot_mould` **·** — a stone mould for casting a bar
- `slag_heap` **·** — a dull grey pile of smelting slag
- `horseshoe` **· [icon]** — an iron horseshoe

## 7. Other crafts and labour

- `loom` **◆** — a timber loom with a half-woven cloth on it
- `spinning_wheel` **◆** — a spinning wheel with a bobbin
- `kiln` **◆** — a domed brick kiln with a dark mouth
- `tanning_rack` **·** — a stretched hide on a timber frame
- `potter_wheel` **·** — a potter's wheel with a wet clay bowl on it
- `saw_buck` **·** — a two-man saw cutting a log
- `dye_vat` **·** — a wooden vat of dark blue dye
- `mine_cart` **◆** — a small wheeled cart of ore on plank rails
- `ladder` **★** — a plain wooden ladder of two stringers and rungs
- `wheelbarrow` **★** — one wheel, a wooden tray and two handles
- `handcart` **◆** — two large wheels and a flat bed with low sides
- `rope_coil` **◆** — a coil of hemp rope
- `plank_stack` **·** — a stack of sawn planks
- `brick_stack` **·** — a pile of clay bricks
- `stone_block` **·** — one dressed stone block

## 8. Farming, crops and animals

Wheat already has its full growth chain (`wheat_seed`, `wheat_seed_planted`,
`wheat_sprout`, `wheat_mature`), and `Evolvable` switches stage by template — so the crops
below fit the existing machinery as soon as the art exists.

- `water_trough` **★** — a hollowed log trough of water, as a cut-out object. The old
  `data/tiles/water_trough.json` was retired in the 2026-10-17 migration
- `well` **★** — a round stone well with a timber frame and a bucket on a rope
- `hay_bale` **★** — a bound rectangle of yellow straw, lying on its side
- `haystack` **◆** — a tall conical stack of loose hay
- `scarecrow` **◆** — a cross of poles with a straw head and a patched coat
- `beehive` **◆** — a domed straw skep on a small stand
- `chicken_coop` **◆** — a small timber coop with a ramp
- `feed_trough` **◆** — a timber feed trough
- `plow` **◆** — a wooden plough with an iron share and two handles
- `harrow` **·** — a toothed timber harrow
- `compost_heap` **·** — a steaming heap of dark compost
- `millstone` **·** — a round millstone standing on edge
- `windmill_sails` **·** — four wooden sails on a hub
- `stump` **◆** — a cut tree stump
- `log_pile` **◆** — a stack of unsplit logs
- `hen` **◆** — a plump brown hen
- `rooster` **·** — a rooster with a red comb
- `cow` **·** — a brown-and-white cow, side on
- `sheep` **·** — a woolly sheep
- `pig` **·** — a pink-grey pig
- `goat` **·** — a brown goat with short horns
- `pigsty` **·** — a low fence and a mud patch with a wooden shelter

Crops, three stages each (`_sprout`, `_young`, `_ripe`): `cabbage`, `turnip`, `carrot`,
`potato`, `pumpkin`, `grape_vine`, `onion`.

## 9. Church and shrine

Most of this already exists (`altar`, `pulpit`, `lectern`, `church_bench`, `church_pew`,
`baptismal_font`, `censer`, `chalice`, `cross`, `rose_window`, `stained_glass_window`,
`collection_box`, `candelabra`). What a chapel is visibly missing:

- `bell` **◆** — a bronze bell hanging from a timber yoke
- `prayer_candles` **◆** — a tiered iron stand of small lit candles
- `saint_statue` **◆** — a pale stone robed figure on a small plinth
- `icon` **·** — a small painted panel with a gold halo
- `confessional` **·** — a dark timber confessional with a curtain
- `vestment_chest` **·** — a long low chest with folded cloth inside
- `holy_water_basin` **·** — a small stone basin on an iron bracket
- `gravestone` **◆** — a rounded headstone on a low mound
- `grave_cross` **·** — a simple wooden cross marker
- `tomb_slab` **·** — a flat carved slab lying in the grass
- `coffin` **·** — a plain plank coffin
- `church_gate` **·** — an arched timber gate set in a low stone wall
- `organ` **·** — a small pipe organ against a wall

## 10. Market, trade and wealth

- `stall` **◆** — a market stall: a plank table under a striped awning
- `awning` **◆** — a striped cloth awning on two poles
- `hanging_sign` **◆** — an iron bracket with a swinging painted board
- `signpost` **★** — a wooden post with two pointing boards
- `notice_board` **◆** — a board with pinned parchments
- `coins_silver` **◆** — a small pile of pale silver coins
- `coins_gold` **◆** — a small pile of gold coins
- `scale` **◆** — a balance scale with two pans
- `ledger` **◆** — a closed leather ledger with a strap
- `quill_ink` **·** — a quill standing in an ink pot
- `scroll` **◆ [icon]** — a rolled parchment with a red wax seal
- `book` **◆ [icon]** — a closed book with a metal clasp
- `map_rolled` **· [icon]** — a rolled map with a cord
- `sealed_letter` **· [icon]** — a folded letter with a wax seal
- `gem` **· [icon]** — a cut red gem
- `ring` **· [icon]** — a gold ring
- `silk_bolt` **·** — a rolled bolt of coloured cloth
- `spice_box` **·** — a small open box of dark red powder

## 11. Tavern and inn

- `bar_counter` **◆** — a long scarred wooden counter with a shelf behind it
- `bottle_rack` **·** — a wall rack holding bottles
- `coat_rack` **◆** — a wooden post with pegs and one grey cloak hanging
- `dice` **· [icon]** — two bone dice
- `playing_cards` **· [icon]** — a few worn cards
- `room_key` **· [icon]** — a brass key with a wooden tag
- `menu_board` **·** — a hanging board with a chalked shape (no readable text)
- `bunk_room` — covered by `bed_bunk`

## 12. Tools, weapons and armour

- `axe` **★ [icon]** — a wood axe with a wooden haft
- `hammer` **★ [icon]** — a smith's hammer with a wrapped handle
- `tongs` **★ [icon]** — blacksmith's tongs
- `pickaxe` **★ [icon]** — an iron pickaxe on a haft
- `hoe` **★ [icon]** — a long-handled hoe
- `sickle` **★ [icon]** — a short curved harvest sickle
- `pitchfork` **★ [icon]** — a wooden pitchfork with two tines
- `shovel` **◆ [icon]** — an iron-bladed shovel
- `watering_can` **◆ [icon]** — a metal watering can with a rose spout
- `rake` **◆ [icon]** — a wooden rake
- `hatchet` **· [icon]** — a small hatchet
- `mallet` **· [icon]** — a wooden mallet
- `whetstone` **· [icon]** — a worn sharpening stone
- `sword` **◆ [icon]** — a plain iron sword in a leather scabbard
- `dagger` **· [icon]** — a short dagger
- `spear` **· [icon]** — a spear with an iron head
- `bow` **· [icon]** — a simple wooden bow with a string
- `arrows` **· [icon]** — a bundle of arrows
- `shield` **· [icon]** — a round wooden shield with an iron boss
- `helmet` **· [icon]** — a plain iron helmet
- `mail_shirt` **· [icon]** — a mail hauberk, folded
- `padlock` **· [icon]** — an iron padlock
- `key` **· [icon]** — a heavy iron key
- `nails` **· [icon]** — a small heap of iron nails
- `chain` **· [icon]** — a short length of chain

## 13. Nature and wild props

- `bush` **★** — a round green bush
- `rock` **★** — a grey boulder with a flat base
- `bush_berries` **◆** — a bush carrying red berries
- `hedge` **◆** — a clipped hedge section
- `oak` **◆** — a broad oak with a dark trunk
- `tree_dead` **◆** — a bare grey dead tree
- `mushroom` **◆ [icon]** — a cluster of pale-capped mushrooms
- `flowers` **◆** — a patch of small red and white flowers
- `pine` **·** — a dark conifer
- `birch` **·** — a pale-trunked birch
- `sapling` **·** — a thin young tree
- `bramble` **·** — a thorny tangle
- `fern` **·** — a single fern
- `tall_grass` **·** — a tuft of tall grass
- `reeds` **·** — reeds at the water's edge
- `pebbles` **·** — a scatter of small stones
- `cliff_chunk` **·** — a lump of dark cliff face
- `moss_mound` **·** — a low mossy mound
- `ivy_patch` **·** — a patch of ivy
- `puddle` **·** — a shallow puddle with a rim of mud
- `lily_pad` **·** — flat leaves floating on water
- `cave_mouth` **·** — a dark cave entrance in a rock face
- `burrow` **·** — a small animal burrow
- `nest` **·** — a twig nest with eggs
- `bones` **·** — a scatter of bones
- `spider_web` **·** — a web strung between two posts

## 14. Ground tiles

**Ground only.** Existing ground: `floor`, `grass`, `packed_earth`, `path`, `wooden_floor`,
`tilled_soil`, `water`, `red_carpet`. The rest of a village's ground surfaces:

- `tile_cobble` **◆** — round cobblestones set in dark mortar
- `tile_gravel` **◆** — coarse grey gravel
- `tile_mud` **◆** — wet churned mud
- `tile_straw` **◆** — scattered straw over earth
- `tile_shallow_water` **◆** — pale clear water over pebbles
- `tile_bridge_planks` **◆** — transverse bridge decking
- `tile_sand` **·** — pale dry sand
- `tile_snow` **·** — trodden snow
- `tile_slab` **·** — large worn flagstones
- `tile_parquet` **·** — herringbone parquet
- `tile_marble` **·** — pale veined marble
- `tile_rubble` **·** — broken stone debris
- `tile_scree` **·** — loose stone slope
- `tile_boards_weathered` **·** — grey outdoor boards

**Formerly misclassified, now migrated:** `rock_face`, `stone_wall`, `wooden_wall`,
`mossy_stone_wall` and `fence` used to exist in `data/tiles/`. They are objects now, and their
templates live in `data/objects/`. Do not add another tile definition for them. See
`tile-and-object-vocabulary.md` §4.

### Cell-filling structure objects

These are **objects**, not `[tile]`, even though each fills its cell so neighbouring cells meet.
Ask for them with the object prompt and name them without the `tile_` prefix:

- `thatch_roof` **◆** — overlapping golden thatch
- `shingle_roof` **◆** — split wooden shingles in rows
- `plaster_wall` **◆** — off-white lime plaster with hairline cracks
- `halftimber_wall` **◆** — plaster panels between dark timber framing
- `brick_wall` **◆** — red-brown clay brick in courses
- `hedge_segment` **◆** — a solid leafy hedge section
- `log_wall` **·** — horizontal stacked logs with chinking
- `picket_fence` **·** — a low picket fence

## 15. Characters

Four exist: `farmer`, `miller`, `smith`, `trader`. Each is 16–19 px wide and 30 px tall
inside the 32×32 cell, front-facing, feet on the bottom edge, and the trade is carried by
the silhouette — a broad hat, a leather apron, a pack — not by detail.

- `innkeeper` **◆** — a stout figure with a cloth over one arm
- `priest` **◆** — a figure in a long dark robe with a pale stole
- `guard` **◆** — a figure in a leather jerkin and a simple helm, holding a spear
- `woodcutter` **◆** — a broad figure with an axe over one shoulder
- `miner` **◆** — a figure in a dull tunic with a lamp on the belt
- `baker` **◆** — a figure with a flour-dusted apron and rolled sleeves
- `child` **◆** — a short figure in a plain tunic
- `elder` **◆** — a stooped figure with a walking stick and grey hair
- `merchant` **·** — a figure in a good coat with a heavy purse
- `healer` **·** — a figure in a pale apron carrying a basket
- `noble` **·** — a figure in a rich coloured cloak
- `beggar` **·** — a seated figure in a torn cloak

Only the front facing exists in the art. A four-direction walk is a sprite-sheet job and a
different prompt shape entirely; do not ask for it as one image.

## 16. What not to ask for yet

- **Multi-tile buildings.** A mill is one 32×32 sprite today. A building spanning tiles is a
  layout problem (see `docs/blueprint-design.md`), and a sprite that assumes a footprint
  cannot be placed by the current renderer.
- **Animation frames.** Frame-to-frame consistency from a single sheet request is not
  achievable yet; one still per object is the deliverable.
- **Isometric rotation.** The world is orthogonal top-down; an isometric sprite cannot sit in
  a square tile.
- **Readable text on signs and boards.** At 32px a letterform is noise. Ask for a shape — a
  painted emblem, a chalked mark, a nailed parchment.
- **Hi-res sprites.** `SpriteFitRect` fits into a 32px box and snaps to integer scale, so a
  64px sprite is downscaled by a fraction and turns to mush. Deliver 32×32.

---

## 17. Ready-to-paste sheets

Paste [Block A](sprite-style-prompt.md) first, then one sheet. 18 per sheet (6 columns by
3 rows); sheet 0 is an object sheet and uses the object wording in the same document.

### Sheet 0 — the five gaps (objects, 5 cells)

These are objects, not ground tiles. The door is the one cell-filling object; the other four are
cut-out objects on magenta.

```
Draw these 5 objects as a strict grid of 3 columns by 2 rows, filling the first five cells in
reading order and leaving the last cell empty:
1) a closed plank door: dark vertical boards, two black iron hinges and a ring handle, a heavy
   timber frame around it, seen straight on and filling the cell edge to edge (a cell-filling
   object, not a ground tile)
2) a stone forge hearth: grey stone blocks, a glowing orange coal bed and a dark smoke hood,
   a single isolated object seen from slightly above
3) a workbench: a scarred wooden bench top with a hammer, a few nails and a vice, a single
   isolated object seen from slightly above
4) a blacksmith's anvil: a dark iron anvil with a horn, a single isolated object seen from
   slightly above
5) a water trough: a hollowed log trough filled with dark water, a single isolated object seen
   from slightly above
```

### Sheet 1 — a room a person could live in (18)

```
 1) a wooden bed: a straw mattress, a grey blanket folded at the foot, one pale pillow, on
    a heavy dark timber frame
 2) a small square nightstand with one drawer and a brass knob
 3) a tall dark wooden wardrobe with two doors and a brass latch
 4) a low chest of three drawers with iron pulls
 5) a tall open bookshelf holding a few stacked books and a rolled scroll
 6) a short plank wall shelf on two brackets with two clay jars on it
 7) a plain plank table with four square legs
 8) a ladder-back chair with a rush seat
 9) a three-legged stool with a round seat
10) a long low backless wooden bench
11) a flat round braided rug in red and ochre rings
12) a low stone hearth with a dark firebox and a timber mantel
13) a round black iron cauldron with a bail handle
14) a staved oak barrel with two iron hoops
15) a slatted pale-plank crate
16) a lumpy burlap sack tied at the neck
17) a wooden bucket with an iron band and a rope handle
18) a stack of split firewood logs
```

### Sheet 2 — doors, gates and windows (18)

```
 1) a pale wooden garden gate with a diagonal brace
 2) a black iron gate of vertical bars with a curved top
 3) a half door: a wooden lower leaf with the top half swung open
 4) a wide double barn door of vertical boards with a diagonal brace
 5) a heavy oak door studded with iron nails
 6) an arched oak door with two iron bands
 7) a broken door hanging open on one hinge, splintered at its edge
 8) a low wooden cellar hatch with an iron pull ring
 9) a small window closed by grey wooden shutters
10) a small window of diamond panes in a leaded timber frame
11) a narrow arched window with a stone sill
12) a window boarded over with crossed planks
13) a wide low shop window with a sill and a display ledge
14) a small dark cellar window with an iron grille
15) three worn stone steps
16) a short flight of plank stairs
17) a section of timber handrail
18) a stone archway with a dark opening
```

### Sheet 3 — workshop, farm and street (18)

```
 1) a wooden ladder of two stringers and rungs
 2) a wheelbarrow with one wheel, a wooden tray and two handles
 3) a handcart with two large wheels and a flat bed with low sides
 4) a leather bellows with two handles
 5) a quench barrel: a half barrel of dark water with a ladle
 6) a grindstone: a round grey stone wheel on a timber frame
 7) a wall tool board with tongs, a hammer and a chisel hanging
 8) a heap of grey rock shot through with orange iron ore flecks
 9) a heap of black coal lumps
10) a stack of grey iron ingots
11) a wooden plough with an iron share and two handles
12) a bound rectangle of yellow straw hay bale
13) a tall conical stack of loose hay
14) a round stone well with a timber frame and a bucket on a rope
15) a cross of poles with a straw head and a patched coat, a scarecrow
16) a wooden signpost with two pointing boards
17) a coil of hemp rope
18) a cut tree stump
```

### Sheet 4 — light, church, market (18)

```
 1) a square iron lantern with glass panes and a flame inside
 2) an iron wall bracket holding a candle
 3) a pitch torch in an iron wall ring, burning
 4) a squat iron brazier full of glowing coals
 5) an iron post with a hanging lantern
 6) a ring of stones around burning logs, a campfire
 7) a bronze bell hanging from a timber yoke
 8) a tiered iron stand of small lit candles
 9) a pale stone robed figure on a small plinth
10) a rounded headstone on a low mound of earth
11) a plain plank coffin
12) a long scarred wooden counter with a shelf behind it
13) a market stall: a plank table under a striped awning
14) an iron bracket with a swinging painted wooden board
15) a board with pinned parchments
16) a balance scale with two pans
17) a closed leather ledger with a strap
18) a round green bush with a flat base
```

### Sheet 5 — nature and the edge of the village (18)

```
 1) a grey boulder with a flat base
 2) a bush carrying red berries
 3) a clipped hedge section
 4) a broad oak tree with a dark trunk
 5) a bare grey dead tree
 6) a cluster of pale-capped mushrooms
 7) a patch of small red and white flowers
 8) a dark conifer tree
 9) a thin young sapling
10) a thorny bramble tangle
11) tuft of tall grass
12) reeds at the water's edge
13) a scatter of small stones
14) a lump of dark cliff face
15) a shallow puddle with a rim of mud
16) flat lily pads floating on water
17) a dark cave entrance in a rock face
18) a scatter of bones
```
