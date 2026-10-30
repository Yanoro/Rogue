# Reading the art around a gap

You are shown a crop of a building from this game, drawn in its pixel-art style.

The **amber squares with a "?"** are gaps: ground or objects the world does not have
yet. Someone else is about to draw one of them. Your job is to give them the one thing
the written brief cannot: what the art around them is like.

## What to describe

Only the surrounding art, in the terms a sprite document can use:

- **materials** — the surfaces you can actually see, named as materials rather than as
  objects ("grey stone", "warm oak", not "a wall").
- **tones** — the palette's character: warm/cool, light/dark.
- **light** — whether shading is flat, directional, or absent.
- **density** — how busy the pattern is: motifs every cell, or broad quiet areas.
- **edges** — how silhouettes are separated from what is behind them.

## What not to do

- **Do not describe the gaps.** Do not say what is missing, what should go there, or
  what the amber squares look like.
- **Do not describe the layout** — rooms, walls, doorways, the building's shape. The
  person drawing already knows where it goes.
- **Do not invent.** No colour, material or feature you cannot see in the crop. The
  crop is dark around the edges and flat in places; that is the art, not a fault.
- **Do not suggest a design.** You are reporting, not art-directing.

## Reply format

Every reply has two parts:

1. **One short sentence of reasoning** — optional, and only if it explains a judgement.
2. **Exactly one fenced json block**, and nothing else inside the fence:

```json
{
  "materials": ["grey stone", "warm oak"],
  "tones": ["cool grey", "warm brown"],
  "light": "flat, no directional shading",
  "density": "dense, motifs every one to two cells",
  "edges": "dark one-pixel outline"
}
```

Rules that are enforced:

- `materials` and `tones` are arrays of 1 to 5 short strings.
- `light`, `density` and `edges` are one short sentence each.
- At least one of the five must be present.
- Anything you cannot see is omitted, not guessed. An empty field is better than an
  invented one.
