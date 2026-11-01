# Navigation attention gate (first slice)

**Status:** implemented, 2026-10-31, in the single navigator. This is the first
slice of [agent-attention-design.md](agent-attention-design.md) §4: a mid-leg
gate over navigation percepts. The general `Perception`/`WakeReason` path, the
`follow`/`back`/`to:` primitives, and the two-context gate are not implemented.

## 1. What it does

While a navigation leg is moving, the engine watches for **new percepts**. When
one appears it briefly pauses the character and asks a System 1 classifier one
question:

- `stop` — hand control back to the navigator now;
- `note` — keep walking, but surface the report at the next decision;
- `ignore` — keep walking; the percept remains in memory and the chunk strip.

There is no confidence, no threshold and no per-monitor answer. The S1 returns a
small choice, which is what a small model is good at.

The engine supplies facts. The S1 supplies the attention judgment. The main
navigator supplies the intention and decides what to do after a stop.

## 2. Why a watch brief, not a monitor list

The navigator writes an optional `watch` string in its plan:

```json
"plan": {
  "strategy": "find a village",
  "subtask": "follow the road east",
  "watch": "Looking for a village. Man-made structures and people matter; stop for anything unusual."
}
```

The `watch` is a **priority, not a filter**. The question the S1 sees always adds
“stop for anything a person would point out or that would change the plan, even
if it is not in the watch.” A clown dancing is a new character, so the gate can
stop for it even though the watch only mentions man-made structures. This is why
the watch does not need to enumerate every case, and why it has no id and no
confidence.

An empty watch means no mid-walk gate; the normal chunk strip still records what
was passed.

## 3. What counts as a new percept

`BuildGateReport` in [NavigationGate.hpp](../include/NavigationGate.hpp) reports:

- **new object groups** — an object content key not known when the leg began,
  with a count and a display name (e.g. `Stone Wall x4`);
- **new characters** — a character record first seen after the leg began;
- **new places** — a semantic place or region first seen after the leg began.

A new instance of an **already-known object group** does not trigger: walking
past another tree is not a new kind of thing. A new place, character or object
group does. This is a deliberate first-slice compromise: a village made entirely
of groups the agent has already seen can still be missed unless it produces a new
place or character. The chunk strip at the next decision is the fallback.

The report is capped in the prompt; all new group keys are still remembered so a
capped group cannot retrigger on the next check.

## 4. Flow

1. `StartLeg`/`StartObjectLeg` call `BeginGateTracking`: record the current
   vision tick, the known non-character object groups, and the current watch.
2. While the leg is moving, `CheckGate` runs at most every 0.2 s. It builds a
   report only when something is new.
3. `StartGate` pauses the leg (`MoveToCellAction::handleInterruption` drops the
   move path), starts the S1 call, and sets `Phase::Gating`.
4. `UpdateGate` polls the shared `GatePending`:
   - `ignore`: resume the same path, keep walking;
   - `note`: append the report to `NOTED`, resume, keep walking;
   - `stop`: record the walked prefix as a chunk leg, set `GATE`, reset the leg,
     and enter `Phase::Deciding`.
5. The next navigator prompt contains `GATE:` and/or `NOTED:` before
   `LAST RESULT:`.

On `note`/`ignore`/timeout the leg resumes the **same A* path** from the current
cell (`MoveToCellAction::resume`), so the chunk trace still covers the whole leg
rather than only the post-gate remainder. On `stop`, the walked prefix is written
as its own trace before the next leg starts; its trace target is `the stopping
point`, not the leg's original destination.

If the S1 is unavailable, or the call fails, or it takes more than 2 s, the gate
degrades to `note`: the character keeps walking and the percepts are surfaced at
the next decision. The gate never blocks indefinitely.

## 5. What the navigator sees

The plan schema gained one optional field:

```json
"watch": "what you want to be interrupted for while moving; a priority, not a filter"
```

The user prompt gained two blocks:

```
GATE:
stopped by the attention gate.
NEW SINCE LAST CHECK:
  objects: Stone Wall x4, Signpost x1
  places: the north orchard
...

NOTED:
NEW SINCE LAST CHECK:
  characters: a trader
...
```

The system prompt tells the navigator that `GATE` is a mid-walk stop and `NOTED`
is a percept seen while walking that did not stop it. The Navigation window's
**Chunks** tab shows the pending `GATE`/`NOTED` text.

## 6. Limits

- `kMaxGateCalls = 24` per episode; after that the gate is off and the chunk
  strip is the only record.
- `kGateTimeoutSeconds = 2.0`.
- `kGateScanSeconds = 0.2` between reports.
- The gate runs only after a leg has started; it does not run during the
  decision model call or a survey.
- Only the single navigator is wired. The two-context navigator still uses its
  strategist monitors at decision points.

## 7. Verification

- `build/AIRogue` builds green.
- A standalone test of `BuildGateReport`/`ParseGateVerdict` checks:
  - a new wall group and a new clown are both reported;
  - a known wall group does not hide the new clown;
  - no new percept produces an empty report;
  - `stop`/`note`/`ignore` map correctly, and an empty/unknown choice maps to
    `note`.
- Runtime model behaviour (whether the S1 choices are useful, how often the gate
  fires, whether the watch brief is written well) still needs a real map run.

## 8. Open questions

1. Should the gate also fire on a **new instance of a known structural group**
   (a new building in a village of known stone walls), or is the new-place/new-
   character signal enough?
2. Is 0.2 s the right scan cadence at the game’s movement speed, or should it
   scale with tiles/second?
3. Should a `stop` pause the main navigator on the spot, or first step to the
   nearest safe/notable cell?
4. Should the gate budget be a setting rather than a constant?

The next slice — the primitive vocabulary (`follow`, `back`, `to:`) with this
gate attached to every leg — is implemented in
[traversal-primitives-first-slice.md](traversal-primitives-first-slice.md).
After that, the two-context path can reuse the same gate.
