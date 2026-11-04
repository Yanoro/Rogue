# Body silhouette anchors

The inventory paper-doll no longer has to use one global hardcoded slot layout.

## Definitions

`content.bodySilhouettes` points at `data/body_silhouettes` by default. A
definition owns the transparent silhouette icon and its slot anchors:

```json
{
  "id": "body_human",
  "name": "Human",
  "icon": "body_human",
  "anchors": [
    { "slot": "head", "x": 0.50, "y": 0.07, "size": 0.18 },
    { "slot": "main_hand", "x": 0.20, "y": 0.48, "size": 0.18 }
  ]
}
```

Coordinates are normalised to the silhouette canvas:

- `x`, `y`: 0..1, measured on the image itself;
- `size`: optional icon diameter as a fraction of canvas width.

An anchor's `slot` matches a body-slot id first (`main_hand`, `ring_1`), then the
slot's `accepts` kind (`hand`, `ring`). This lets one `ring` anchor serve a race
with rings but no special ring ids.

`RaceDef.silhouetteIcon` references the silhouette id. If no definition exists,
the inventory falls back to the old icon id and the hardcoded layout table.

## Inventory rendering

The body silhouette and every anchor share one aspect-preserving canvas fitted
inside the right panel. Anchors therefore stay on the design regardless of
window aspect ratio; the previous code fitted the image and the percentages
independently, which is why the body could appear half the panel.

Fallback order:

1. silhouette anchor matching the slot id,
2. silhouette anchor matching `accepts`,
3. hardcoded `SlotLayout`,
4. auto-arranged fallback on the right edge.

## Editing

Content Editor → **Body silhouettes**:

- `icon` uses the IconRegistry picker,
- `anchors` is still ordinary JSON,
- the **Slot anchors** preview below the form shows the silhouette,
- drag a marker to move it; the JSON is updated,
- type a slot id and press **Add anchor** to add one.

Dragging does not modify the PNG; the anchor metadata is saved with the
definition.
