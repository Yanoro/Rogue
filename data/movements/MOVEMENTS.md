# Movements

A movement is the *shared* half of an animation: what "walking", "running" or "a kick"
is, in words and timing. The other half lives on the rig -- the frame sprites for that
particular body -- and is generated from a movement. The same movement therefore becomes
a different set of images on every rig, while the intent is written once.

The id is the slug of the name: `"Sword Swing"` is `sword_swing`, and that is what a skill
writes (`"animation": "sword_swing"`). The registry is loaded from `data/movements` and the
generated overlay `data/generated/movements`; Content Editor overrides live under its own
`movements` directory and are applied at startup.

## Fields

| field | meaning |
|---|---|
| `name` | The id is this slugged. A skill can name it (`"animation": "kick"`). |
| `category` | Free grouping for the picker: `locomotion`, `attack`, `cast`, `social`. |
| `description` | The whole-body motion, in prose. The per-part generation prompts are cut from it. |
| `frames` | How many frames the clip has. Frames are `f1..fN`. |
| `fps` | Playback cadence. A rig may override this; the frame count comes from here. |
| `loop` | Whether the clip repeats. |
| `parts` | Which rig part ids participate. Empty means every part. A part that does not participate keeps its static sprite -- that is how "a kick does not animate the hair" is said. |
| `requires` | Capabilities the rig or its equipment must provide before the movement can be drawn honestly (`weapon`, `focus`). The shipped weapon/cast movements no longer declare these: the grip path draws the equipped hand item. The pipeline still refuses a movement that declares one, so only add a requirement the rig can actually meet. |
| `poses` | One line per frame, in whole-body terms. This is the quality lever: a one-line "walk" produces generic art, a contact/passing plan produces a walk. Not validated for motion -- the compiler cannot see it. |

## Generating

In the Designer's rig pane, create a clip and pick a movement. The clip copies the
movement's frames, fps, loop, brief and poses, stores `"movement": "<id>"`, and
generation queues one multi-frame request per participating part. Frame sprites are
`<part sprite>_<movement>_f1..fN`, declared in the rig JSON before the art exists.

`requires` is a gate, not a hint: the Designer refuses to generate an entry whose
requirements the current rig cannot meet. The grip path made the weapon/focus gate
unnecessary for hand items, so `sword_swing` and `spell_cast` no longer set it; they
generate like any other clip, and the equipped hand item follows the arm on the grip path.
A movement that still declares a requirement is refused until that requirement exists.
