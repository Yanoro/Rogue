# Player portrait resources

The player portrait can show a chosen subset of the player's resource pools, in
a chosen order, with optional icons, names and values.

Configuration lives in `settings.json` / `settings.local.json`:

```json
"hud": {
  "portrait": {
    "showIcons": true,
    "showValues": true,
    "showNames": true,
    "compact": false,
    "barHeight": 12,
    "resources": [
      { "id": "health", "visible": true, "showIcon": true },
      { "id": "mana", "visible": true, "showIcon": true }
    ]
  }
}
```

- `resources` order is the portrait order.
- A resource absent from the array is appended in registry/pool order.
- `visible: false` hides the row.
- `showIcon` is the per-resource icon switch; `showIcons` is the master switch.
- An empty `resources` array means "show every current resource in registry
  order".
- Resources are referenced by id, so a race that later grants Mana, a shield or
  a class resource needs no settings migration.

## Settings window

Settings → HUD → Player portrait lists every resource the player currently has.
Each row has:

- up/down order buttons,
- Shown,
- Icon,
- the resource icon and name.

The section also has the master icon switch, show values, show names, compact
mode, bar height, Reset order and a live preview.

The setting applies only to the player portrait. Target/enemy portraits keep the
automatic primary-resource layout.

## HUD behavior

The player portrait reads the settings every frame. Target portraits continue to
show the primary vital resource automatically; this keeps enemy information
predictable and avoids configuring every possible target.

## Suggested future additions

- Per-resource bar colors and widths.
- Icon-only mode, or icon plus a thin unlabeled bar.
- Two-column portrait layout for many resources.
- Hiding a row when full, or only showing it in combat.
- Low-resource pulse/flash and threshold markers.
- Hover tooltips with resource description and regeneration rate.
- Per-resource alignment and label orientation.
- Portrait scale, opacity, background and name visibility.
- Separate target-portrait configuration.
- Drag-and-drop ordering in addition to the arrow buttons.
- Named presets (minimal, full, combat-only).
