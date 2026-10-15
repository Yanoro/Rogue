# Screenshots

The root `README.md` embeds these three files. Drop them in with these exact names and the
placeholders become real images — no README edit needed:

| File | What to capture |
|---|---|
| `village.png` | The village zoomed out — `./build/AIRogue maps/Map_Village.json`, then `Shift`+`Z` a couple of times. The wheat farm, apple garden, mill and characters should all be visible. |
| `agent-context.png` | The **Agent Context** window with a live exchange showing: the player prompt, the model's `<think>` block (with "show thoughts" enabled), and the bracketed command it chose. This is the money shot — it shows the game's whole idea in one frame. |
| `debug-windows.png` | The debug console and log panels open over the map, ideally with the A* visualizer or ASCII hitbox overlay visible too. |

## Tips

- Capture at **1920×1080 or larger**; GitHub renders the README on both desktop and mobile.
- Use `PNG`, not `JPEG` — ASCII glyphs and UI text go mushy fast under JPEG compression.
- Keep each file **under ~1 MB** so the README stays quick to load (`oxipng -o4` or
  `pngquant` will do it if needed).
- A short GIF of an agent walking to the mill, starting a conversation and closing a trade
  would do more for this project than any static shot. Aim for **under 10 MB** if you add one.
