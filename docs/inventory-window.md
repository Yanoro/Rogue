# Inventory window

## Access

Press `I` to open or close the Inventory. Escape closes it through the normal
window system.

## Layout

```text
+----------------------------------------------------------+
| Inventory                                                |
| [page controls]                                          |
+--------------------------------+-------------------------+
| 3 rows x 10 columns            | transparent body        |
| 30 cells per page              | silhouette + slots      |
+--------------------------------+-------------------------+
```

The grid is **unequipped inventory only**. Equipped items remain owned through
`Holds` (so equipment lookup and `[UNEQUIP]` still work) but are excluded from
inventory cells, capacity, trade and `[INVENTORY]` listings.

## Capacity and pages

`InventorySlots.capacity` is initialized from
`settings.player.inventorySlots` (default 30).

```text
pageCount = max(1, ceil(capacity / 30))
```

- capacity 20: one page, 10 cells crossed out with `inventory_locked`;
- capacity 45: page 1 is 30 cells, page 2 is 15 usable and 15 crossed out;
- capacity 70: three pages.

Changing `player.inventorySlots` in Settings applies to the live player.

The Content Editor's **Give to player inventory** button respects capacity, but
existing map-authored inventories that already exceed capacity are shown only up
to the available cells for now.

## Stacking

`ItemDef` now accepts:

```json
"stackable": true,
"maxStack": 999
```

- Stackable definitions group by item id and share a cell up to `maxStack`.
- Larger stacks split into more cells.
- Equipment remains one item per cell.
- The Content Editor always emits `stackable` and `maxStack`, so a new item can
  opt into stacking through the generic form.

There is no special currency path: money is an item like any other if a
definition is authored for it.

## Body panel

The paper-doll uses a hardcoded `SlotLayout` table in `InventoryWindow.cpp`:

- head at the top,
- neck below,
- torso and back,
- hands left/right,
- rings below the hands,
- legs and feet at the bottom.

Slot positions are selected by slot id first, then by `accepts`. Unknown race
slots are auto-arranged down the right edge and remain interactive.

The silhouette comes from `RaceDef.silhouetteIcon`, which names a
`BodySilhouetteRegistry` definition. That definition supplies both the icon and
normalised slot anchors; see [body-silhouette-anchors.md](body-silhouette-anchors.md).
Empty/unknown falls back to `body_human` and then the hardcoded layout.

## Interactions

- Double-click inventory equipment → equip.
- Double-click a worn body slot → unequip.
- Tooltips show item name, count, damage/armour and worn slot ids.
- Empty slot icons are transparent and low-opacity.
- Unavailable cells use the `inventory_locked` icon.

Drag-and-drop is deliberately deferred.

## Content Editor

Equipment → selected item → **Give to player inventory**:

- spawns one object template,
- adds it to the player's `Holds`,
- does not equip it,
- refuses when the object template is missing or inventory is full.

The button lives in the item preview pane and reports its result in the Content
Editor status line.
