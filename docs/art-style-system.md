# Art style system

Generated sprite prompts are composed from layered prose instead of repeating the
same style paragraph in every asset brief.

## Files

`content.artStyles` points at `data/art_styles` by default. Each file is an
`ArtStyleDef`:

```json
{
  "id": "ui_slot_icon",
  "name": "Inventory Slot Icons",
  "appliesTo": ["slot_icon"],
  "positive": "Readable at 32x32: one centred monochrome symbol ...",
  "negative": "Do not use text, backgrounds, gradients ..."
}
```

The Content Editor has an **Art styles** kind, so profiles are authored there
without hand-writing a file.

## Selection order

0. The asset's `style` is `none` (see **No style** below): no style at all, and no
   fallback to any of the steps that follow.
1. An asset's own `style` field.
2. The visual asset type's theme.
3. The global default theme.
4. No style paragraph.

## No style

An asset can be created with no art style at all. That is a different choice from
"inherit", and it needs its own value because an empty `style` already means "use the
family default":

```json
{ "name": "Rusted Cog", "style": "none" }
```

`none` is reserved (`kNoArtStyle` in `include/ArtStyleRegistry.h`). The registry
refuses a theme whose id slugs to it, so the value can never resolve to a theme by
accident, and the resolver stops instead of falling through to the visual type's theme
or the global default. The request then carries no style prose, negative prose, palette,
`<style-reference>` or section role, and the system prompt uses the global ramps. The
canvas still defaults to 32x32 when the caller names no size.

It is offered in both dropdowns:

- **Art style for generation** in the sprite pane (Content Editor), next to
  `(global default)` and the themes. This is transient: it applies to the next
  Regenerate/Generate ask, so a new asset can be made without a style without changing
  the asset's document.
- The document's own **style** field, next to `Default (family style)` and the themes.
  This persists, so the asset is generated without a style every time.

Choosing a theme in either dropdown clears the no-style choice. Consumers that need
proportions from a style (`GeometryForStyle`) treat `none` like any unknown id and fall
back to the canonical humanoid.

Current families:

- `slot_icon`
- `skill_icon`
- `resource_icon`
- `item` / `object`
- `tile`
- `actor_sprite`
- `surface_sprite`
- `portrait`
- `body_silhouette`

The sprite document request gets the style prose before the asset description,
so the model reads the family voice first and the subject second. The style is
prose only in this slice: no palette quantification, outline pass, alpha
enforcement, or reference images.

## Per-asset overrides

Icon, actor-sprite, surface-sprite and equipment documents carry a top-level
`style` field. The Content Editor renders it as a dropdown of every registered
profile and prints the selected profile's description, positive prose and
negative prose below it. An empty value means the family default.

## Bulk regeneration

The Designer has an **Art style batch** row:

- choose a style,
- press **Regenerate all assets in style**.

It queues every icon, actor sprite, surface sprite and equipment item whose
`style` matches, then runs one document request at a
time. The Designer window must remain open because its Draw pumps replies, and
the batch advances only when the previous request is no longer busy.

This is deliberately a queue rather than parallel requests: the art conversation
has one history and one compiler retry budget.

## What is not here yet

- No visual reference sheets. The style is prose only.
- No deterministic palette/alpha/outline enforcement.
- No style-version metadata on generated markers, so stale assets are not marked
  automatically.
- Bulk regeneration covers icons, actor sprites, surface sprites and equipment
  items that carry an explicit style. Tiles and generic objects still use family
  defaults.
